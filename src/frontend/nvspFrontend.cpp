/*
TGSpeechBox — Frontend public C API implementation.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "nvspFrontend.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <locale>
#include <mutex>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Locale-independent double parse — NVDA may set process locale to one that
// uses ',' as decimal separator (Hungarian, Polish, etc.), which would cause
// std::strtod/atof to misread YAML values like "0.15" as 0.0.
static double parseDouble(const std::string& s) {
  std::istringstream iss(s);
  iss.imbue(std::locale::classic());
  double v = 0.0;
  iss >> v;
  return v;
}

#include "data_query.h"
#include "utf8.h"
#include "yaml_export.h"
#include "ipa_engine.h"
#include "pack.h"
#include "text_parser.h"

namespace nvsp_frontend {

struct Handle {
  std::string packDir;
  std::string overrideDir;  // Optional dir checked first for lang YAML files
  PackSet pack;
  bool packLoaded = false;
  // True once we have emitted at least one chunk of speech on this handle.
  // Used to optionally insert a tiny silence between consecutive queueIPA calls.
  bool streamHasSpeech = false;
  // True if the last emitted *real phoneme* in the previous chunk was vowel-like
  // (vowel or semivowel). Used to avoid inserting boundary pauses inside
  // vowel-to-vowel transitions (e.g. diphthongs split across chunks).
  bool lastEndsVowelLike = false;
  std::string langTag;
  std::string lastError;
  std::mutex mu;
  
  // Per-handle trajectory limiting state for formant smoothing.
  // This is NOT static - each handle has its own state to avoid data races
  // when multiple engine instances speak concurrently.
  TrajectoryState trajectoryState;
  
  // User-level FrameEx defaults (ABI v2+).
  // These are mixed with per-phoneme values when emitting frames.
  double frameExCreakiness = 0.0;
  double frameExBreathiness = 0.0;
  double frameExJitter = 0.0;
  double frameExShimmer = 0.0;
  double frameExSharpness = 1.0;  // multiplier, 1.0 = neutral
  
  // Buffer for getVoiceProfileNames return value
  std::string profileNamesBuffer;

  // Deferred settings: if setPitchMode / setInflectionScale are called
  // before setLanguage, we stash the values here and apply them once
  // the pack is loaded.  Prevents crashes from writing to uninitialized
  // h->pack memory (Android crash when Speak tab selects klatt_style
  // before language init).
  std::string pendingPitchMode;
  double pendingInflectionScale = 0.0;
  bool hasPendingInflectionScale = false;

  // Generic data query cache (ABI v5+).
  tgsb_data::DataCache dataCache;
};

static Handle* asHandle(nvspFrontend_handle_t h) {
  return reinterpret_cast<Handle*>(h);
}

static void setError(Handle* h, const std::string& msg) {
  if (!h) return;
  h->lastError = msg;
}

// Helper to format a double with minimal precision (avoid "2.000000")
// Defined here (outside extern "C") to avoid C4190 warning about std::string.
static std::string formatDouble(double val, int precision = 2) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.*f", precision, val);
  // Trim trailing zeros after decimal point
  std::string s = buf;
  if (s.find('.') != std::string::npos) {
    size_t lastNonZero = s.find_last_not_of('0');
    if (lastNonZero != std::string::npos && s[lastNonZero] == '.') {
      // Keep at least one decimal (e.g., "2.0" not "2.")
      s = s.substr(0, lastNonZero + 2);
    } else if (lastNonZero != std::string::npos) {
      s = s.substr(0, lastNonZero + 1);
    }
  }
  return s;
}

} // namespace nvsp_frontend

extern "C" {

NVSP_FRONTEND_API nvspFrontend_handle_t nvspFrontend_create(const char* packDirUtf8) {
  using namespace nvsp_frontend;
  try {
    auto* h = new Handle();
    h->packDir = packDirUtf8 ? std::string(packDirUtf8) : std::string();
    h->lastError.clear();
    return reinterpret_cast<nvspFrontend_handle_t>(h);
  } catch (...) {
    return nullptr;
  }
}

NVSP_FRONTEND_API void nvspFrontend_destroy(nvspFrontend_handle_t handle) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  delete h;
}

NVSP_FRONTEND_API void nvspFrontend_setOverrideDirectory(
    nvspFrontend_handle_t handle, const char* overrideDirUtf8) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return;
  std::lock_guard<std::mutex> lock(h->mu);
  h->overrideDir = (overrideDirUtf8 && overrideDirUtf8[0])
      ? std::string(overrideDirUtf8) : std::string();
}

NVSP_FRONTEND_API int nvspFrontend_setLanguage(nvspFrontend_handle_t handle, const char* langTagUtf8) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return 0;

  std::lock_guard<std::mutex> lock(h->mu);

  h->lastError.clear();
  const std::string lang = langTagUtf8 ? std::string(langTagUtf8) : std::string();

  PackSet pack;
  std::string err;
  if (!loadPackSet(h->packDir, lang, pack, err, h->overrideDir)) {
    setError(h, err.empty() ? "Failed to load pack set" : err);
    return 0;
  }

  // Preserve voice profile name across language changes.
  // loadPackSet produces a fresh PackSet with empty voiceProfileName,
  // but the caller expects the active profile to survive.
  std::string savedProfile = h->pack.lang.voiceProfileName;

  h->pack = std::move(pack);
  h->packLoaded = true;

  // Restore the voice profile that was active before the language change.
  if (!savedProfile.empty()) {
    h->pack.lang.voiceProfileName = savedProfile;
  }

  // Apply any settings that were set before the language was loaded.
  if (!h->pendingPitchMode.empty()) {
    h->pack.lang.legacyPitchMode = h->pendingPitchMode;
    h->pendingPitchMode.clear();
  }
  if (h->hasPendingInflectionScale) {
    h->pack.lang.legacyPitchInflectionScale = h->pendingInflectionScale;
    h->hasPendingInflectionScale = false;
  }

  // Treat language change as the start of a new stream, so we don't
  // insert a segment boundary gap before the first chunk in the new language.
  h->streamHasSpeech = false;
  h->lastEndsVowelLike = false;
  h->langTag = normalizeLangTag(lang);

  // Invalidate the data query cache — new language means new settings.
  h->dataCache.invalidate();

  return 1;
}

NVSP_FRONTEND_API int nvspFrontend_queueIPA(
  nvspFrontend_handle_t handle,
  const char* ipaUtf8,
  double speed,
  double basePitch,
  double inflection,
  const char* clauseTypeUtf8,
  int userIndexBase,
  nvspFrontend_FrameCallback cb,
  void* userData
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return 0;

  std::lock_guard<std::mutex> lock(h->mu);
  h->lastError.clear();

  if (!h->packLoaded) {
    // Default to "default" language if the caller didn't call setLanguage.
    PackSet pack;
    std::string err;
    if (!loadPackSet(h->packDir, "default", pack, err, h->overrideDir)) {
      setError(h, err.empty() ? "No language loaded and default load failed" : err);
      return 0;
    }
    h->pack = std::move(pack);
    h->packLoaded = true;
    h->langTag = "default";
  }

  if (!ipaUtf8) ipaUtf8 = "";

  char clauseType = '.';
  if (clauseTypeUtf8 && clauseTypeUtf8[0]) {
    clauseType = clauseTypeUtf8[0];
  }

  std::vector<Token> tokens;
  std::string err;
  if (!convertIpaToTokens(h->pack, ipaUtf8, speed, basePitch, inflection, clauseType, tokens, err)) {
    setError(h, err.empty() ? "IPA conversion failed" : err);
    return 0;
  }

  // Determine whether this chunk starts/ends with a vowel-like phoneme.
  // We ignore silence/preStopGap tokens for this purpose.
  const Token* firstReal = nullptr;
  const Token* lastReal = nullptr;
  for (const Token& t : tokens) {
    if (!t.def || t.silence) continue;
    if (!firstReal) firstReal = &t;
    lastReal = &t;
  }

  auto isVowelLike = [](const Token& t) -> bool {
    if (!t.def) return false;
    const std::uint32_t f = t.def->flags;
    return (f & kIsVowel) || (f & kIsSemivowel);
  };

  auto isLiquidLike = [](const Token& t) -> bool {
    if (!t.def) return false;
    const std::uint32_t f = t.def->flags;
    return (f & kIsLiquid) || (f & kIsTap) || (f & kIsTrill);
  };

  const bool startsVowelLike = firstReal && isVowelLike(*firstReal);
  const bool startsLiquidLike = firstReal && isLiquidLike(*firstReal);
  const bool endsVowelLike = lastReal && isVowelLike(*lastReal);
  const bool hasRealPhoneme = (firstReal != nullptr);

  // Optional: insert a short silence between consecutive queueIPA calls.
  // This helps when callers stitch UI speech from multiple chunks.
  //
  // However, a boundary pause can create an audible "hole" in vowel-to-vowel
  // transitions (e.g. when a diphthong is split across chunks). To keep
  // diphthongs smooth while preserving consonant clarity, we suppress the
  // boundary gap when the previous chunk ended with a vowel/semivowel and
  // the next chunk starts with a vowel/semivowel.
  if (cb && h->streamHasSpeech && hasRealPhoneme) {
    const double gapMs = h->pack.lang.segmentBoundaryGapMs;
    const double fadeMs = h->pack.lang.segmentBoundaryFadeMs;
    if (gapMs > 0.0 || fadeMs > 0.0) {
      bool skip = false;
      if (h->pack.lang.segmentBoundarySkipVowelToVowel &&
          h->lastEndsVowelLike && startsVowelLike) {
        skip = true;
      }
      if (!skip && h->pack.lang.segmentBoundarySkipVowelToLiquid &&
          h->lastEndsVowelLike && startsLiquidLike) {
        skip = true;
      }
      if (!skip) {
        const double spd = (speed > 0.0) ? speed : 1.0;
        cb(userData, nullptr, gapMs / spd, fadeMs / spd, userIndexBase);
      }
    }
  }

  emitFrames(h->pack, tokens, userIndexBase, speed, &h->trajectoryState, cb, userData);
  if (hasRealPhoneme) {
    h->streamHasSpeech = true;
    h->lastEndsVowelLike = endsVowelLike;
  }
  return 1;
}

NVSP_FRONTEND_API int nvspFrontend_setVoiceProfile(nvspFrontend_handle_t handle, const char* profileNameUtf8) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return 0;

  std::lock_guard<std::mutex> lock(h->mu);
  h->lastError.clear();

  // Set the voice profile name in the language pack settings.
  // This will be used during the next queueIPA call.
  h->pack.lang.voiceProfileName = profileNameUtf8 ? std::string(profileNameUtf8) : std::string();
  return 1;
}

NVSP_FRONTEND_API const char* nvspFrontend_getVoiceProfile(nvspFrontend_handle_t handle) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return "";

  std::lock_guard<std::mutex> lock(h->mu);
  return h->pack.lang.voiceProfileName.c_str();
}

NVSP_FRONTEND_API const char* nvspFrontend_getPackWarnings(nvspFrontend_handle_t handle) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return "";

  std::lock_guard<std::mutex> lock(h->mu);
  return h->pack.loadWarnings.c_str();
}

NVSP_FRONTEND_API const char* nvspFrontend_getLastError(nvspFrontend_handle_t handle) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return "invalid handle";
  std::lock_guard<std::mutex> lock(h->mu);
  return h->lastError.c_str();
}

NVSP_FRONTEND_API int nvspFrontend_getABIVersion(void) {
  return NVSP_FRONTEND_ABI_VERSION;
}

NVSP_FRONTEND_API void nvspFrontend_setFrameExDefaults(
  nvspFrontend_handle_t handle,
  double creakiness,
  double breathiness,
  double jitter,
  double shimmer,
  double sharpness
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return;

  std::lock_guard<std::mutex> lock(h->mu);
  h->frameExCreakiness = creakiness;
  h->frameExBreathiness = breathiness;
  h->frameExJitter = jitter;
  h->frameExShimmer = shimmer;
  h->frameExSharpness = sharpness;
}

NVSP_FRONTEND_API int nvspFrontend_getFrameExDefaults(
  nvspFrontend_handle_t handle,
  nvspFrontend_FrameEx* outDefaults
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !outDefaults) return 0;

  std::lock_guard<std::mutex> lock(h->mu);
  outDefaults->creakiness = h->frameExCreakiness;
  outDefaults->breathiness = h->frameExBreathiness;
  outDefaults->jitter = h->frameExJitter;
  outDefaults->shimmer = h->frameExShimmer;
  outDefaults->sharpness = h->frameExSharpness;
  // Formant end targets: NAN means "no ramping" - per-phoneme only
  outDefaults->endCf1 = NAN;
  outDefaults->endCf2 = NAN;
  outDefaults->endCf3 = NAN;
  outDefaults->endPf1 = NAN;
  outDefaults->endPf2 = NAN;
  outDefaults->endPf3 = NAN;
  return 1;
}

// Shared implementation for queueIPA_Ex and queueIPA_ExWithText.
// textUtf8 may be NULL or "" to skip text parsing.
static int queueIPA_ExImpl(
  nvsp_frontend::Handle* h,
  const char* textUtf8,
  const char* ipaUtf8,
  double speed,
  double basePitch,
  double inflection,
  const char* clauseTypeUtf8,
  int userIndexBase,
  nvspFrontend_FrameExCallback cb,
  void* userData
) {
  using namespace nvsp_frontend;

  h->lastError.clear();

  if (!h->packLoaded) {
    PackSet pack;
    std::string err;
    if (!loadPackSet(h->packDir, "default", pack, err, h->overrideDir)) {
      setError(h, err.empty() ? "No language loaded and default load failed" : err);
      return 0;
    }
    h->pack = std::move(pack);
    h->packLoaded = true;
    h->langTag = "default";
  }

  if (!ipaUtf8) ipaUtf8 = "";

  // Unicode normalization: NFKC + strip invisible characters.
  // Prevents dictionary mismatches from NFD text (iOS/macOS copy-paste)
  // and invisible formatting chars from messaging apps (Telegram, WhatsApp).
  std::string normalizedText;
  if (textUtf8 && textUtf8[0]) {
    normalizedText = normalizeText(textUtf8);
    textUtf8 = normalizedText.c_str();
  }

  // Run text parser if text is available and there's work to do
  // (stress dict for stress correction, OR compound map for IPA merge).
  std::string parsedIpa;
  const char* finalIpa = ipaUtf8;
  if (textUtf8 && textUtf8[0] &&
      (!h->pack.stressDict.empty() || !h->pack.compoundMap.empty())) {
    parsedIpa = runTextParser(textUtf8, ipaUtf8, h->pack.stressDict,
                               h->pack.compoundMap,
                               h->pack.lang.legalOnsets,
                               h->pack.lang.numberExpansion);
    finalIpa = parsedIpa.c_str();
  }

  char clauseType = '.';
  if (clauseTypeUtf8 && clauseTypeUtf8[0]) {
    clauseType = clauseTypeUtf8[0];
  }

  std::vector<Token> tokens;
  std::string err;
  if (!convertIpaToTokens(h->pack, finalIpa, speed, basePitch, inflection, clauseType, tokens, err)) {
    setError(h, err.empty() ? "IPA conversion failed" : err);
    return 0;
  }

  // Determine whether this chunk starts/ends with a vowel-like phoneme.
  const Token* firstReal = nullptr;
  const Token* lastReal = nullptr;
  for (const Token& t : tokens) {
    if (!t.def || t.silence) continue;
    if (!firstReal) firstReal = &t;
    lastReal = &t;
  }

  auto isVowelLike = [](const Token& t) -> bool {
    if (!t.def) return false;
    const std::uint32_t f = t.def->flags;
    return (f & kIsVowel) || (f & kIsSemivowel);
  };

  auto isLiquidLike = [](const Token& t) -> bool {
    if (!t.def) return false;
    const std::uint32_t f = t.def->flags;
    return (f & kIsLiquid) || (f & kIsTap) || (f & kIsTrill);
  };

  const bool startsVowelLike = firstReal && isVowelLike(*firstReal);
  const bool startsLiquidLike = firstReal && isLiquidLike(*firstReal);
  const bool endsVowelLike = lastReal && isVowelLike(*lastReal);
  const bool hasRealPhoneme = (firstReal != nullptr);

  // Optional: insert a short silence between consecutive queueIPA calls.
  if (cb && h->streamHasSpeech && hasRealPhoneme) {
    const double gapMs = h->pack.lang.segmentBoundaryGapMs;
    const double fadeMs = h->pack.lang.segmentBoundaryFadeMs;
    if (gapMs > 0.0 || fadeMs > 0.0) {
      bool skip = false;
      if (h->pack.lang.segmentBoundarySkipVowelToVowel &&
          h->lastEndsVowelLike && startsVowelLike) {
        skip = true;
      }
      if (!skip && h->pack.lang.segmentBoundarySkipVowelToLiquid &&
          h->lastEndsVowelLike && startsLiquidLike) {
        skip = true;
      }
      if (!skip) {
        const double spd = (speed > 0.0) ? speed : 1.0;
        cb(userData, nullptr, nullptr, gapMs / spd, fadeMs / spd, userIndexBase);
      }
    }
  }

  // Build FrameEx defaults struct to pass to emitFramesEx
  nvspFrontend_FrameEx frameExDefaults;
  frameExDefaults.creakiness = h->frameExCreakiness;
  frameExDefaults.breathiness = h->frameExBreathiness;
  frameExDefaults.jitter = h->frameExJitter;
  frameExDefaults.shimmer = h->frameExShimmer;
  frameExDefaults.sharpness = h->frameExSharpness;
  frameExDefaults.endCf1 = NAN;
  frameExDefaults.endCf2 = NAN;
  frameExDefaults.endCf3 = NAN;
  frameExDefaults.endPf1 = NAN;
  frameExDefaults.endPf2 = NAN;
  frameExDefaults.endPf3 = NAN;

  emitFramesEx(h->pack, tokens, userIndexBase, speed, frameExDefaults, &h->trajectoryState, cb, userData);

  if (hasRealPhoneme) {
    h->streamHasSpeech = true;
    h->lastEndsVowelLike = endsVowelLike;
  }
  return 1;
}

NVSP_FRONTEND_API int nvspFrontend_queueIPA_Ex(
  nvspFrontend_handle_t handle,
  const char* ipaUtf8,
  double speed,
  double basePitch,
  double inflection,
  const char* clauseTypeUtf8,
  int userIndexBase,
  nvspFrontend_FrameExCallback cb,
  void* userData
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return 0;
  std::lock_guard<std::mutex> lock(h->mu);
  return queueIPA_ExImpl(h, "", ipaUtf8, speed, basePitch, inflection,
                         clauseTypeUtf8, userIndexBase, cb, userData);
}

NVSP_FRONTEND_API int nvspFrontend_queueIPA_ExWithText(
  nvspFrontend_handle_t handle,
  const char* textUtf8,
  const char* ipaUtf8,
  double speed,
  double basePitch,
  double inflection,
  const char* clauseTypeUtf8,
  int userIndexBase,
  nvspFrontend_FrameExCallback cb,
  void* userData
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return 0;
  std::lock_guard<std::mutex> lock(h->mu);
  return queueIPA_ExImpl(h, textUtf8, ipaUtf8, speed, basePitch, inflection,
                         clauseTypeUtf8, userIndexBase, cb, userData);
}

NVSP_FRONTEND_API int nvspFrontend_getVoicingTone(
  nvspFrontend_handle_t handle,
  nvspFrontend_VoicingTone* outTone
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !outTone) return 0;

  std::lock_guard<std::mutex> lock(h->mu);

  // Initialize with the same defaults as SPEECHPLAYER_VOICINGTONE_DEFAULTS
  // so that voice profiles only need to override fields they explicitly set.
  // Previously these were zeroed, which meant any profile with a partial
  // voicingTone: block would disable high-shelf EQ, pre-emphasis, etc.
  outTone->voicingPeakPos = 0.91;
  outTone->voicedPreEmphA = 0.92;
  outTone->voicedPreEmphMix = 0.35;
  outTone->highShelfGainDb = 5.5;
  outTone->highShelfFcHz = 2000.0;
  outTone->highShelfQ = 0.7;
  outTone->voicedTiltDbPerOct = 0.0;
  outTone->noiseGlottalModDepth = 0.0;
  outTone->pitchSyncF1DeltaHz = 0.0;
  outTone->pitchSyncB1DeltaHz = 0.0;
  outTone->speedQuotient = 2.0;
  outTone->aspirationTiltDbPerOct = 0.0;
  outTone->cascadeBwScale = 1.0;
  outTone->tremorDepth = 0.0;
  outTone->nasalBwScale = 1.0;
  outTone->f4FreqScale = 1.0;
  outTone->nasalGainScale = 1.0;

  // Check if we have a voice profile with voicing tone
  const std::string& profileName = h->pack.lang.voiceProfileName;
  if (profileName.empty()) return 0;

  if (!h->pack.voiceProfiles) return 0;
  const VoiceProfile* profile = h->pack.voiceProfiles->getProfile(profileName);
  if (!profile) return 0;
  if (!profile->hasVoicingTone) return 0;

  // Copy the voicing tone values
  const VoicingTone& vt = profile->voicingTone;
  if (vt.voicingPeakPos_set) outTone->voicingPeakPos = vt.voicingPeakPos;
  if (vt.voicedPreEmphA_set) outTone->voicedPreEmphA = vt.voicedPreEmphA;
  if (vt.voicedPreEmphMix_set) outTone->voicedPreEmphMix = vt.voicedPreEmphMix;
  if (vt.highShelfGainDb_set) outTone->highShelfGainDb = vt.highShelfGainDb;
  if (vt.highShelfFcHz_set) outTone->highShelfFcHz = vt.highShelfFcHz;
  if (vt.highShelfQ_set) outTone->highShelfQ = vt.highShelfQ;
  if (vt.voicedTiltDbPerOct_set) outTone->voicedTiltDbPerOct = vt.voicedTiltDbPerOct;
  if (vt.noiseGlottalModDepth_set) outTone->noiseGlottalModDepth = vt.noiseGlottalModDepth;
  if (vt.pitchSyncF1DeltaHz_set) outTone->pitchSyncF1DeltaHz = vt.pitchSyncF1DeltaHz;
  if (vt.pitchSyncB1DeltaHz_set) outTone->pitchSyncB1DeltaHz = vt.pitchSyncB1DeltaHz;
  if (vt.speedQuotient_set) outTone->speedQuotient = vt.speedQuotient;
  if (vt.aspirationTiltDbPerOct_set) outTone->aspirationTiltDbPerOct = vt.aspirationTiltDbPerOct;
  if (vt.cascadeBwScale_set) outTone->cascadeBwScale = vt.cascadeBwScale;
  if (vt.tremorDepth_set) outTone->tremorDepth = vt.tremorDepth;
  if (vt.nasalBwScale_set) outTone->nasalBwScale = vt.nasalBwScale;
  if (vt.f4FreqScale_set) outTone->f4FreqScale = vt.f4FreqScale;
  if (vt.nasalGainScale_set) outTone->nasalGainScale = vt.nasalGainScale;

  return 1;  // Profile has explicit voicing tone
}

NVSP_FRONTEND_API const char* nvspFrontend_getVoiceProfileNames(nvspFrontend_handle_t handle) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return "";

  std::lock_guard<std::mutex> lock(h->mu);

  // Build newline-separated list of profile names
  h->profileNamesBuffer.clear();
  
  if (h->pack.voiceProfiles) {
    for (const auto& kv : h->pack.voiceProfiles->profiles) {
      h->profileNamesBuffer += kv.first;
      h->profileNamesBuffer += '\n';
    }
  }

  return h->profileNamesBuffer.c_str();
}

NVSP_FRONTEND_API int nvspFrontend_saveVoiceProfileSliders(
  nvspFrontend_handle_t handle,
  const char* profileNameUtf8,
  const nvspFrontend_VoiceProfileSliders* sliders
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !profileNameUtf8 || !sliders) {
    if (h) setError(h, "Invalid parameters");
    return 0;
  }

  std::lock_guard<std::mutex> lock(h->mu);

  std::string profileName = profileNameUtf8;
  if (profileName.empty()) {
    setError(h, "Profile name cannot be empty");
    return 0;
  }

  // Build path to phonemes.yaml
  std::string phonemesPath = h->packDir + "/phonemes.yaml";
  
  // Read current file content
  std::ifstream inFile(phonemesPath);
  if (!inFile.is_open()) {
    setError(h, "Cannot open phonemes.yaml for reading");
    return 0;
  }
  
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(inFile, line)) {
    // Remove trailing \r if present (Windows line endings)
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  inFile.close();

  // The 12 slider keys we write (7 VoicingTone + 5 FrameEx)
  // Order matters for nice YAML output
  struct SliderDef {
    const char* key;
    double value;
    int precision;
  };
  std::vector<SliderDef> sliderDefs = {
    {"voicedTiltDbPerOct", sliders->voicedTiltDbPerOct, 2},
    {"noiseGlottalModDepth", sliders->noiseGlottalModDepth, 2},
    {"pitchSyncF1DeltaHz", sliders->pitchSyncF1DeltaHz, 1},
    {"pitchSyncB1DeltaHz", sliders->pitchSyncB1DeltaHz, 1},
    {"speedQuotient", sliders->speedQuotient, 2},
    {"aspirationTiltDbPerOct", sliders->aspirationTiltDbPerOct, 2},
    {"cascadeBwScale", sliders->cascadeBwScale, 2},
    {"tremorDepth", sliders->tremorDepth, 2},
    {"creakiness", sliders->creakiness, 2},
    {"breathiness", sliders->breathiness, 2},
    {"jitter", sliders->jitter, 2},
    {"shimmer", sliders->shimmer, 2},
    {"sharpness", sliders->sharpness, 2},
  };

  // Track which slider keys we've written (to avoid duplicates)
  std::set<std::string> writtenKeys;

  // State machine for parsing
  bool inVoiceProfiles = false;
  bool inTargetProfile = false;
  bool inVoicingTone = false;
  bool foundProfile = false;
  bool foundVoicingTone = false;
  int voiceProfilesIndent = -1;
  int profileIndent = -1;
  int voicingToneIndent = -1;
  int voicingToneContentIndent = -1;

  std::vector<std::string> newLines;

  auto getIndent = [](const std::string& s) -> int {
    int indent = 0;
    for (char c : s) {
      if (c == ' ') indent++;
      else break;
    }
    return indent;
  };

  auto makeIndent = [](int n) -> std::string {
    return std::string(static_cast<size_t>(n), ' ');
  };

  // Helper to write all slider values
  auto writeAllSliders = [&](int indent) {
    for (const auto& sd : sliderDefs) {
      if (writtenKeys.find(sd.key) == writtenKeys.end()) {
        newLines.push_back(makeIndent(indent) + sd.key + ": " + formatDouble(sd.value, sd.precision));
        writtenKeys.insert(sd.key);
      }
    }
  };

  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& curLine = lines[i];
    bool skipLine = false;  // Set true to skip adding curLine to output
    
    std::string stripped = curLine;
    // Trim leading/trailing whitespace for comparison
    size_t start = stripped.find_first_not_of(" \t");
    if (start != std::string::npos) {
      stripped = stripped.substr(start);
    } else {
      stripped = "";
    }
    
    int indent = getIndent(curLine);

    // Check for voiceProfiles: at root level
    if (!curLine.empty() && curLine[0] != ' ' && curLine[0] != '\t') {
      if (stripped.find("voiceProfiles:") == 0) {
        inVoiceProfiles = true;
        voiceProfilesIndent = 0;
        profileIndent = -1;
        inTargetProfile = false;
        inVoicingTone = false;
      } else if (inVoiceProfiles) {
        // Left voiceProfiles section - if we were in target profile, close it
        if (inTargetProfile && !foundVoicingTone) {
          // Need to add voicingTone block before leaving
          int vtIndent = (profileIndent >= 0 ? profileIndent : 2) + 2;
          newLines.push_back(makeIndent(vtIndent) + "voicingTone:");
          writeAllSliders(vtIndent + 2);
          foundVoicingTone = true;
        } else if (inVoicingTone) {
          // Write any remaining slider values
          writeAllSliders(voicingToneContentIndent >= 0 ? voicingToneContentIndent : voicingToneIndent + 2);
          inVoicingTone = false;
        }
        inVoiceProfiles = false;
        inTargetProfile = false;
      }
    }

    if (inVoiceProfiles) {
      // Detect profile indent level
      if (profileIndent < 0 && !stripped.empty() && stripped.back() == ':' && indent > voiceProfilesIndent) {
        profileIndent = indent;
      }

      // Check if this is the target profile line
      if (indent == profileIndent && !stripped.empty()) {
        std::string potentialName = stripped;
        size_t colonPos = potentialName.find(':');
        if (colonPos != std::string::npos) {
          potentialName = potentialName.substr(0, colonPos);
        }
        
        if (potentialName == profileName) {
          inTargetProfile = true;
          foundProfile = true;
          inVoicingTone = false;
          foundVoicingTone = false;
          voicingToneIndent = -1;
          writtenKeys.clear();
        } else if (inTargetProfile) {
          // Moving to a different profile
          if (!foundVoicingTone) {
            // Add voicingTone block before the new profile
            int vtIndent = profileIndent + 2;
            newLines.push_back(makeIndent(vtIndent) + "voicingTone:");
            writeAllSliders(vtIndent + 2);
            foundVoicingTone = true;
          } else if (inVoicingTone) {
            // Write remaining sliders before leaving
            writeAllSliders(voicingToneContentIndent >= 0 ? voicingToneContentIndent : voicingToneIndent + 2);
          }
          inTargetProfile = false;
          inVoicingTone = false;
        }
      }

      // Inside target profile
      if (inTargetProfile && indent > profileIndent) {
        // Check for voicingTone:
        if (stripped.find("voicingTone:") == 0 && !inVoicingTone) {
          inVoicingTone = true;
          foundVoicingTone = true;
          voicingToneIndent = indent;
          voicingToneContentIndent = -1;
          newLines.push_back(curLine);
          continue;
        }

        // Check for sibling sections (classScales, phonemeOverrides)
        if ((stripped.find("classScales:") == 0 || stripped.find("phonemeOverrides:") == 0) 
            && indent == profileIndent + 2) {
          if (inVoicingTone) {
            // Write remaining sliders before sibling section
            writeAllSliders(voicingToneContentIndent >= 0 ? voicingToneContentIndent : voicingToneIndent + 2);
            inVoicingTone = false;
          } else if (!foundVoicingTone) {
            // Add voicingTone block before sibling
            int vtIndent = profileIndent + 2;
            newLines.push_back(makeIndent(vtIndent) + "voicingTone:");
            writeAllSliders(vtIndent + 2);
            foundVoicingTone = true;
          }
        }

        // Inside voicingTone block
        if (inVoicingTone && indent > voicingToneIndent) {
          if (voicingToneContentIndent < 0) {
            voicingToneContentIndent = indent;
          }

          // Check if this line is one of our slider keys
          size_t colonPos = stripped.find(':');
          if (colonPos != std::string::npos) {
            std::string key = stripped.substr(0, colonPos);
            
            // Check if it's one of our slider keys and replace if so
            for (const auto& sd : sliderDefs) {
              if (key == sd.key) {
                // Replace with new value (skip original line)
                if (writtenKeys.find(key) == writtenKeys.end()) {
                  newLines.push_back(makeIndent(indent) + sd.key + ": " + formatDouble(sd.value, sd.precision));
                  writtenKeys.insert(key);
                }
                skipLine = true;
                break;
              }
            }
            
            // Not a slider key - preserve it (hidden params like voicingPeakPos)
            // But first check if we've left voicingTone (indent decreased)
          }
        }
        
        // Check if we've left voicingTone block (indent back to profile level or voicingTone level)
        if (inVoicingTone && indent <= voicingToneIndent) {
          // Write remaining sliders
          writeAllSliders(voicingToneContentIndent >= 0 ? voicingToneContentIndent : voicingToneIndent + 2);
          inVoicingTone = false;
        }
      }
    }

    if (!skipLine) {
      newLines.push_back(curLine);
    }
  }

  // Handle end of file cases
  if (inVoicingTone) {
    writeAllSliders(voicingToneContentIndent >= 0 ? voicingToneContentIndent : voicingToneIndent + 2);
  } else if (inTargetProfile && !foundVoicingTone) {
    int vtIndent = (profileIndent >= 0 ? profileIndent : 2) + 2;
    newLines.push_back(makeIndent(vtIndent) + "voicingTone:");
    writeAllSliders(vtIndent + 2);
  }

  // If profile wasn't found at all, add it at the end of voiceProfiles section
  if (!foundProfile) {
    // Find where voiceProfiles section ends (or end of file)
    // For simplicity, append at end of file under voiceProfiles
    bool hasVoiceProfiles = false;
    for (const auto& l : lines) {
      if (l.find("voiceProfiles:") == 0) {
        hasVoiceProfiles = true;
        break;
      }
    }
    
    if (!hasVoiceProfiles) {
      // Add voiceProfiles section
      newLines.push_back("");
      newLines.push_back("voiceProfiles:");
    }
    
    // Add the new profile
    newLines.push_back("  " + profileName + ":");
    newLines.push_back("    voicingTone:");
    for (const auto& sd : sliderDefs) {
      newLines.push_back("      " + std::string(sd.key) + ": " + formatDouble(sd.value, sd.precision));
    }
  }

  // Write back to file
  std::ofstream outFile(phonemesPath);
  if (!outFile.is_open()) {
    setError(h, "Cannot open phonemes.yaml for writing");
    return 0;
  }

  for (size_t i = 0; i < newLines.size(); ++i) {
    outFile << newLines[i];
    if (i + 1 < newLines.size()) {
      outFile << '\n';
    }
  }
  outFile.close();

  return 1;
}

NVSP_FRONTEND_API int nvspFrontend_setPitchMode(
  nvspFrontend_handle_t handle,
  const char* modeUtf8
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !modeUtf8) return 0;

  std::string mode = modeUtf8;
  if (mode != "espeak_style" && mode != "legacy" &&
      mode != "fujisaki_style" && mode != "impulse_style" &&
      mode != "klatt_style") {
    setError(h, "Unknown pitch mode: " + mode);
    return 0;
  }

  std::lock_guard<std::mutex> lock(h->mu);
  if (!h->packLoaded) {
    // Store for deferred apply — setLanguage will load the pack later.
    // Without this guard, accessing h->pack before a language is loaded
    // writes to uninitialized memory and crashes (Android #19 report).
    h->pendingPitchMode = mode;
    return 1;
  }
  h->pack.lang.legacyPitchMode = mode;
  return 1;
}

NVSP_FRONTEND_API void nvspFrontend_setLegacyPitchInflectionScale(
  nvspFrontend_handle_t handle,
  double scale
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return;

  std::lock_guard<std::mutex> lock(h->mu);
  if (!h->packLoaded) {
    h->pendingInflectionScale = scale;
    h->hasPendingInflectionScale = true;
    return;
  }
  h->pack.lang.legacyPitchInflectionScale = scale;
}

NVSP_FRONTEND_API char* nvspFrontend_prepareText(
  nvspFrontend_handle_t handle,
  const char* textUtf8
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !textUtf8 || !textUtf8[0]) return nullptr;

  std::lock_guard<std::mutex> lock(h->mu);
  if (!h->packLoaded) return nullptr;

  const std::string original(textUtf8);
  std::string input = normalizeText(original);
  std::string result = prepareTextForEspeak(input, h->pack.compoundMap,
                                             h->pack.pronDict, h->langTag,
                                             h->pack.lang.yearSplittingEnabled,
                                             h->pack.lang.numberExpansion.ohDigit);

  if (result == original) return nullptr;  // no changes

  char* out = static_cast<char*>(std::malloc(result.size() + 1));
  if (!out) return nullptr;
  std::memcpy(out, result.c_str(), result.size() + 1);
  return out;
}

NVSP_FRONTEND_API void nvspFrontend_freeString(char* str) {
  std::free(str);
}

NVSP_FRONTEND_API char* nvspFrontend_exportData(
    nvspFrontend_handle_t handle,
    int domain,
    const char* langTagUtf8,
    const char* overridesJsonUtf8
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return nullptr;

  std::lock_guard<std::mutex> lock(h->mu);

  // Parse overrides JSON into vector of (key, value) pairs.
  std::vector<std::pair<std::string, std::string>> overrides;
  if (overridesJsonUtf8 && overridesJsonUtf8[0]) {
    // Simple JSON object parser: {"key": "value", ...}
    std::string json(overridesJsonUtf8);
    // Find opening brace.
    auto brace = json.find('{');
    if (brace == std::string::npos) return nullptr;
    size_t pos = brace + 1;
    while (pos < json.size()) {
      // Skip whitespace.
      while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' ||
             json[pos] == '\r' || json[pos] == '\t' || json[pos] == ',')) pos++;
      if (pos >= json.size() || json[pos] == '}') break;
      // Parse key (quoted string).
      if (json[pos] != '"') break;
      pos++;
      std::string key;
      while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
          pos++;
          switch (json[pos]) {
            case 'n': key += '\n'; break;
            case 't': key += '\t'; break;
            default: key += json[pos]; break;
          }
        } else {
          key += json[pos];
        }
        pos++;
      }
      if (pos < json.size()) pos++; // skip closing quote
      // Skip colon.
      while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) pos++;
      // Parse value (quoted string).
      if (pos >= json.size() || json[pos] != '"') break;
      pos++;
      std::string val;
      while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
          pos++;
          switch (json[pos]) {
            case 'n': val += '\n'; break;
            case 't': val += '\t'; break;
            default: val += json[pos]; break;
          }
        } else {
          val += json[pos];
        }
        pos++;
      }
      if (pos < json.size()) pos++; // skip closing quote
      overrides.emplace_back(std::move(key), std::move(val));
    }
  }

  // Determine base file path.
  std::string basePath;
  bool isPhonemes = false;

  // Resolve packsRoot the same way pack.cpp does (check both direct and nested).
  auto findRoot = [](const std::string& dir) -> std::string {
    if (dir.empty()) return {};
    std::string direct = dir + "/phonemes.yaml";
    { std::ifstream t(direct); if (t.good()) return dir; }
    std::string nested = dir + "/packs/phonemes.yaml";
    { std::ifstream t(nested); if (t.good()) return dir + "/packs"; }
    return {};
  };

  if (domain == NVSP_DATA_PHONEMES) {
    isPhonemes = true;
    // Check override dir first, then pack dir.
    if (!h->overrideDir.empty()) {
      std::string ovRoot = findRoot(h->overrideDir);
      if (!ovRoot.empty()) basePath = ovRoot + "/phonemes.yaml";
    }
    if (basePath.empty()) {
      std::string root = findRoot(h->packDir);
      if (!root.empty()) basePath = root + "/phonemes.yaml";
    }
  } else if (domain == NVSP_DATA_SETTINGS) {
    std::string lang = langTagUtf8 ? std::string(langTagUtf8) : std::string();
    if (lang.empty()) return nullptr;

    // Check override dir first, then pack dir.
    if (!h->overrideDir.empty()) {
      std::string ovRoot = findRoot(h->overrideDir);
      if (!ovRoot.empty()) {
        std::string ovPath = ovRoot + "/lang/" + lang + ".yaml";
        std::ifstream test(ovPath);
        if (test.good()) basePath = ovPath;
      }
    }
    if (basePath.empty()) {
      std::string root = findRoot(h->packDir);
      if (!root.empty()) basePath = root + "/lang/" + lang + ".yaml";
    }
  } else {
    return nullptr;
  }

  // Verify file exists.
  {
    std::ifstream test(basePath);
    if (!test.good()) return nullptr;
  }

  // Call the surgical merge.
  std::string result = yaml_export::exportMergedYaml(basePath, overrides, isPhonemes);
  if (result.empty()) return nullptr;

  // Return malloc'd copy.
  char* out = static_cast<char*>(std::malloc(result.size() + 1));
  if (!out) return nullptr;
  std::memcpy(out, result.data(), result.size());
  out[result.size()] = '\0';
  return out;
}

NVSP_FRONTEND_API int nvspFrontend_applySettingOverrides(
  nvspFrontend_handle_t handle,
  const char* yamlSnippetUtf8
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !yamlSnippetUtf8 || !yamlSnippetUtf8[0]) return 0;

  std::lock_guard<std::mutex> lock(h->mu);
  if (!h->packLoaded) return 0;

  bool ok = applySettingOverrides(h->pack.lang, std::string(yamlSnippetUtf8));
  if (ok) h->dataCache.invalidate();  // Old API changed settings — stale cache.
  return ok ? 1 : 0;
}

NVSP_FRONTEND_API char* nvspFrontend_getAvailableLanguages(nvspFrontend_handle_t handle) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h) return nullptr;

  std::lock_guard<std::mutex> lock(h->mu);

  auto langs = getAvailableLanguages(h->packDir);
  if (langs.empty()) return nullptr;

  std::string result;
  for (const auto& lang : langs) {
    result += lang;
    result += '\n';
  }

  char* out = static_cast<char*>(std::malloc(result.size() + 1));
  if (!out) return nullptr;
  std::memcpy(out, result.c_str(), result.size() + 1);
  return out;
}

// ── Phoneme Preview (direct DSP pump, no pipeline) ────────────────

NVSP_FRONTEND_API int nvspFrontend_previewPhoneme(
  nvspFrontend_handle_t handle,
  const char* phonemeKeyUtf8,
  double pitchHz,
  double durationMs,
  nvspFrontend_FrameExCallback cb,
  void* userData
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !phonemeKeyUtf8 || !phonemeKeyUtf8[0] || !cb) return 0;

  std::lock_guard<std::mutex> lock(h->mu);

  if (!h->packLoaded) return 0;

  // Look up phoneme directly in the loaded pack.
  std::u32string phonU32 = utf8ToU32(std::string(phonemeKeyUtf8));
  auto it = h->pack.phonemes.find(phonU32);
  if (it == h->pack.phonemes.end()) return 0;

  const PhonemeDef& def = it->second;

  // Build frame directly from PhonemeDef field array.
  // FieldId enum maps 1:1 to nvspFrontend_Frame struct layout.
  nvspFrontend_Frame frame;
  std::memset(&frame, 0, sizeof(frame));
  static_assert(sizeof(nvspFrontend_Frame) == kFrameFieldCount * sizeof(double),
    "Frame/FieldId layout mismatch");
  double* fp = reinterpret_cast<double*>(&frame);
  for (int i = 0; i < kFrameFieldCount; ++i) {
    fp[i] = def.field[i];
  }

  // Override pitch with requested value.
  frame.voicePitch = pitchHz;
  frame.endVoicePitch = pitchHz;

  // Set sensible defaults for a steady-state preview.
  if (frame.preFormantGain <= 0.0) frame.preFormantGain = 1.0;
  if (frame.outputGain <= 0.0) frame.outputGain = 1.0;

  // Build frameEx from PhonemeDef's optional fields.
  nvspFrontend_FrameEx frameEx;
  std::memset(&frameEx, 0, sizeof(frameEx));
  // Set NAN for end-formant fields (no ramping).
  frameEx.endCf1 = NAN; frameEx.endCf2 = NAN; frameEx.endCf3 = NAN;
  frameEx.endPf1 = NAN; frameEx.endPf2 = NAN; frameEx.endPf3 = NAN;
  if (def.hasCreakiness)  frameEx.creakiness  = def.creakiness;
  if (def.hasBreathiness) frameEx.breathiness = def.breathiness;
  if (def.hasJitter)      frameEx.jitter      = def.jitter;
  if (def.hasShimmer)     frameEx.shimmer     = def.shimmer;
  if (def.hasSharpness)   frameEx.sharpness   = def.sharpness;

  // Emit a steady-state phoneme frame.
  if (durationMs <= 0.0) durationMs = 300.0;
  double fadeMs = 15.0;
  cb(userData, &frame, &frameEx, durationMs, fadeMs, 0);

  // Trailing silence for clean fade-out.
  cb(userData, nullptr, nullptr, 0.0, fadeMs, 0);

  return 1;
}

// ── Generic Data Query API (ABI v5+) ──────────────────────────────

NVSP_FRONTEND_API int nvspFrontend_getDataCount(
  nvspFrontend_handle_t handle,
  int domain,
  const char* langTagUtf8
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !langTagUtf8) return -1;

  std::lock_guard<std::mutex> lock(h->mu);

  if (domain == NVSP_DATA_SETTINGS) {
    if (!langTagUtf8[0]) return -1;  // settings requires a lang tag
    const std::string lang(langTagUtf8);
    if (!h->dataCache.settingsValid || h->dataCache.langTag != lang) {
      tgsb_data::buildSettingsCache(h->dataCache, h->packDir, lang);
    }
    return static_cast<int>(h->dataCache.settings.size());
  }

  if (domain == NVSP_DATA_PHONEMES) {
    const std::string lang(langTagUtf8);  // "" = all phonemes
    if (!h->dataCache.phonemesValid || h->dataCache.phonemesLangTag != lang) {
      tgsb_data::buildPhonemesCache(h->dataCache, h->packDir, lang);
    }
    return static_cast<int>(h->dataCache.phonemes.size());
  }

  if (domain == NVSP_DATA_DICTIONARY) {
    if (!h->packLoaded) return 0;
    if (!h->dataCache.dictionaryValid) {
      tgsb_data::buildDictionaryCache(h->dataCache, h->pack.pronDict);
    }
    return static_cast<int>(h->dataCache.dictionary.size());
  }

  // Unsupported domain.
  return -1;
}

NVSP_FRONTEND_API char* nvspFrontend_queryData(
  nvspFrontend_handle_t handle,
  int domain,
  const char* langTagUtf8,
  int offset,
  int limit
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !langTagUtf8) return nullptr;

  std::lock_guard<std::mutex> lock(h->mu);

  if (domain == NVSP_DATA_SETTINGS) {
    if (!langTagUtf8[0]) return nullptr;  // settings requires a lang tag
    const std::string lang(langTagUtf8);
    if (!h->dataCache.settingsValid || h->dataCache.langTag != lang) {
      tgsb_data::buildSettingsCache(h->dataCache, h->packDir, lang);
    }

    std::string json = tgsb_data::serializeSettingsJson(h->dataCache, offset, limit);
    if (json.empty()) return nullptr;

    char* out = static_cast<char*>(std::malloc(json.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, json.c_str(), json.size() + 1);
    return out;
  }

  if (domain == NVSP_DATA_PHONEMES) {
    const std::string lang(langTagUtf8);  // "" = all phonemes
    if (!h->dataCache.phonemesValid || h->dataCache.phonemesLangTag != lang) {
      tgsb_data::buildPhonemesCache(h->dataCache, h->packDir, lang);
    }

    std::string json = tgsb_data::serializePhonemesJson(h->dataCache, offset, limit);
    if (json.empty()) return nullptr;

    char* out = static_cast<char*>(std::malloc(json.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, json.c_str(), json.size() + 1);
    return out;
  }

  if (domain == NVSP_DATA_DICTIONARY) {
    if (!h->packLoaded) return nullptr;
    if (!h->dataCache.dictionaryValid) {
      tgsb_data::buildDictionaryCache(h->dataCache, h->pack.pronDict);
    }

    std::string json = tgsb_data::serializeDictionaryJson(h->dataCache, offset, limit);
    if (json.empty()) return nullptr;

    char* out = static_cast<char*>(std::malloc(json.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, json.c_str(), json.size() + 1);
    return out;
  }

  // Unsupported domain.
  return nullptr;
}

NVSP_FRONTEND_API int nvspFrontend_setData(
  nvspFrontend_handle_t handle,
  int domain,
  const char* langTagUtf8,
  const char* keyUtf8,
  const char* valueUtf8
) {
  using namespace nvsp_frontend;
  Handle* h = asHandle(handle);
  if (!h || !langTagUtf8 || !keyUtf8 || !keyUtf8[0]) return 0;
  if (!valueUtf8) valueUtf8 = "";

  std::lock_guard<std::mutex> lock(h->mu);

  if (domain == NVSP_DATA_SETTINGS) {
    if (!langTagUtf8[0]) return 0;  // settings requires a lang tag
    const std::string lang(langTagUtf8);
    const std::string key(keyUtf8);
    const std::string value(valueUtf8);

    // Build a YAML snippet "key: value\n" and apply via existing path.
    // Only effective if langTag matches the currently loaded language.
    if (h->packLoaded && h->langTag == lang) {
      std::string snippet = key + ": " + value + "\n";
      if (!applySettingOverrides(h->pack.lang, snippet)) {
        setError(h, "Failed to apply setting override");
        return 0;
      }
    }

    h->dataCache.invalidate();
    return 1;
  }

  if (domain == NVSP_DATA_PHONEMES) {
    const std::string dotKey(keyUtf8);   // e.g. "ɪ.cf2"
    const std::string value(valueUtf8);

    // Parse "phonemeKey.fieldName" (first dot splits phoneme from field).
    // Handle frameEx: "ɪ.frameEx.breathiness" → phoneme="ɪ", field path.
    auto firstDot = dotKey.find('.');
    if (firstDot == std::string::npos) return 0;

    std::string phonemeKeyUtf8 = dotKey.substr(0, firstDot);
    std::string fieldPath = dotKey.substr(firstDot + 1);  // "cf2" or "frameEx.breathiness"
    if (phonemeKeyUtf8.empty() || fieldPath.empty()) return 0;

    // Apply in-memory override to the loaded pack if applicable.
    if (h->packLoaded) {
      std::u32string phonU32 = utf8ToU32(phonemeKeyUtf8);
      auto it = h->pack.phonemes.find(phonU32);
      if (it == h->pack.phonemes.end()) {
#ifdef __ANDROID__
        // Phoneme key not found in loaded pack.
#endif
        h->dataCache.invalidate();
        return 0;  // phoneme not found — fail explicitly
      }
      {
        PhonemeDef& def = it->second;

        // Check if it's a frameEx sub-field.
        if (fieldPath.substr(0, 8) == "frameEx.") {
          std::string fxField = fieldPath.substr(8);
          double num = parseDouble(value);
          if (fxField == "creakiness")  { def.hasCreakiness = true; def.creakiness = num; }
          else if (fxField == "breathiness") { def.hasBreathiness = true; def.breathiness = num; }
          else if (fxField == "jitter")      { def.hasJitter = true; def.jitter = num; }
          else if (fxField == "shimmer")     { def.hasShimmer = true; def.shimmer = num; }
          else if (fxField == "sharpness")   { def.hasSharpness = true; def.sharpness = num; }
          else if (fxField == "endCf1")      { def.hasEndCf1 = true; def.endCf1 = num; }
          else if (fxField == "endCf2")      { def.hasEndCf2 = true; def.endCf2 = num; }
          else if (fxField == "endCf3")      { def.hasEndCf3 = true; def.endCf3 = num; }
          else if (fxField == "endPf1")      { def.hasEndPf1 = true; def.endPf1 = num; }
          else if (fxField == "endPf2")      { def.hasEndPf2 = true; def.endPf2 = num; }
          else if (fxField == "endPf3")      { def.hasEndPf3 = true; def.endPf3 = num; }
        }
        // Check flag fields.
        else if (!fieldPath.empty() && fieldPath[0] == '_') {
          bool b = (value == "true" || value == "1");
          uint32_t bit = 0;
          if (fieldPath == "_isAffricate")  bit = kIsAfricate;
          else if (fieldPath == "_isLiquid")     bit = kIsLiquid;
          else if (fieldPath == "_isNasal")      bit = kIsNasal;
          else if (fieldPath == "_isSemivowel")  bit = kIsSemivowel;
          else if (fieldPath == "_isStop")       bit = kIsStop;
          else if (fieldPath == "_isTap")        bit = kIsTap;
          else if (fieldPath == "_isTrill")      bit = kIsTrill;
          else if (fieldPath == "_isVoiced")     bit = kIsVoiced;
          else if (fieldPath == "_isVowel")      bit = kIsVowel;
          else if (fieldPath == "_copyAdjacent") bit = kCopyAdjacent;
          if (bit) {
            if (b) def.flags |= bit;
            else   def.flags &= ~bit;
          }
        }
        // Check micro-event fields.
        else if (fieldPath == "burstDurationMs") {
          def.hasBurstDurationMs = true; def.burstDurationMs = parseDouble(value);
        } else if (fieldPath == "burstDecayRate") {
          def.hasBurstDecayRate = true; def.burstDecayRate = parseDouble(value);
        } else if (fieldPath == "burstSpectralTilt") {
          def.hasBurstSpectralTilt = true; def.burstSpectralTilt = parseDouble(value);
        } else if (fieldPath == "voiceBarAmplitude") {
          def.hasVoiceBarAmplitude = true; def.voiceBarAmplitude = parseDouble(value);
        } else if (fieldPath == "voiceBarF1") {
          def.hasVoiceBarF1 = true; def.voiceBarF1 = parseDouble(value);
        } else if (fieldPath == "releaseSpreadMs") {
          def.hasReleaseSpreadMs = true; def.releaseSpreadMs = parseDouble(value);
        } else if (fieldPath == "fricAttackMs") {
          def.hasFricAttackMs = true; def.fricAttackMs = parseDouble(value);
        } else if (fieldPath == "fricDecayMs") {
          def.hasFricDecayMs = true; def.fricDecayMs = parseDouble(value);
        } else if (fieldPath == "durationScale") {
          def.hasDurationScale = true; def.durationScale = parseDouble(value);
        }
        // Frame fields (FieldId-based).
        else {
          FieldId fid;
          if (parseFieldId(fieldPath, fid)) {
            int idx = static_cast<int>(fid);
            if (idx >= 0 && idx < kFrameFieldCount) {
              double newVal = parseDouble(value);
              def.field[idx] = newVal;
              def.setMask |= (1ull << idx);
            }
          }
        }
      }
    }

    h->dataCache.invalidate();
    return 1;
  }

  if (domain == NVSP_DATA_DICTIONARY) {
    const std::string fromText(keyUtf8);
    const std::string value(valueUtf8);

    // Lowercase key for case-insensitive lookup.
    std::string lk = fromText;
    for (auto& c : lk)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (value.empty()) {
      // Delete entry.
      h->pack.pronDict.entries.erase(lk);
    } else {
      // Parse JSON value: {"toText":"...", "fromIpa":"...", "toIpa":"...",
      //                     "category":"...", "masked":false}
      // Simple extraction — find quoted values after known keys.
      auto extractStr = [&](const char* field) -> std::string {
        auto pos = value.find(field);
        if (pos == std::string::npos) return "";
        pos = value.find('"', pos + std::strlen(field));
        if (pos == std::string::npos) return "";
        ++pos;  // skip opening quote
        auto end = value.find('"', pos);
        if (end == std::string::npos) return "";
        return value.substr(pos, end - pos);
      };

      DictEntry e;
      e.fromText = fromText;
      e.toText   = extractStr("\"toText\":");
      e.fromIpa  = extractStr("\"fromIpa\":");
      e.toIpa    = extractStr("\"toIpa\":");
      e.category = extractStr("\"category\":");
      e.source   = "user";
      e.masked   = (value.find("\"masked\":true") != std::string::npos);

      // If toText is empty but masked is set, update mask on existing entry.
      if (e.toText.empty() && e.masked) {
        auto it = h->pack.pronDict.entries.find(lk);
        if (it != h->pack.pronDict.entries.end()) {
          it->second.masked = true;
          h->dataCache.dictionaryValid = false;
          return 1;
        }
        return 0;
      }
      // Unmask: masked=false with empty toText.
      if (e.toText.empty() && !e.masked &&
          value.find("\"masked\":false") != std::string::npos) {
        auto it = h->pack.pronDict.entries.find(lk);
        if (it != h->pack.pronDict.entries.end()) {
          it->second.masked = false;
          h->dataCache.dictionaryValid = false;
          return 1;
        }
        return 0;
      }

      if (e.toText.empty()) return 0;  // toText is required for new entries

      h->pack.pronDict.entries[lk] = std::move(e);
    }

    h->dataCache.dictionaryValid = false;
    return 1;
  }

  // Unsupported domain.
  return 0;
}

} // extern "C"
