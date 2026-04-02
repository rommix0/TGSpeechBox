/*
TGSpeechBox — Diphthong collapse pass.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "diphthong_collapse.h"

#include <algorithm>
#include <cmath>

// Debug logging for diphthong collapse investigation.
// Set to 1 to enable, 0 to disable.
#define DCOLLAPSE_DEBUG_LOG 0
#if DCOLLAPSE_DEBUG_LOG
#include <cstdio>
#include <cstdlib>
static FILE* dcollapseLogFile() {
  static FILE* f = nullptr;
  if (!f) {
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = "/tmp";
    std::string path = std::string(tmp) + "/tgsb_diphthong_collapse.log";
    f = std::fopen(path.c_str(), "a");
  }
  return f;
}
#define DCLOG(...) do { FILE* _f = dcollapseLogFile(); if (_f) { std::fprintf(_f, __VA_ARGS__); std::fflush(_f); } } while(0)
#else
#define DCLOG(...) ((void)0)
#endif

namespace nvsp_frontend::passes {

namespace {

static inline bool tokIsVowel(const Token& t) {
  return t.def && ((t.def->flags & kIsVowel) != 0);
}

static inline bool tokIsVowelOrSemivowel(const Token& t) {
  return t.def && ((t.def->flags & (kIsVowel | kIsSemivowel)) != 0);
}

// Rhotic diphthongs: eSpeak ties vowel+/ɹ/ (e.g. /ɛ͡ɹ/ in "shared").
// Without this, collapse skips the pair because /ɹ/ is a liquid, leaving
// /ɛ/ and /ɹ/ as two short tokens with no micro-frame formant sweep —
// the vowel quality vanishes at high rates.  Same pattern as the Spanish
// semivowel offglide fix.
static inline bool tokIsValidOffglide(const Token& t) {
  return t.def && ((t.def->flags & (kIsVowel | kIsSemivowel | kIsLiquid)) != 0);
}

} // namespace

bool runDiphthongCollapse(
  PassContext& ctx,
  std::vector<Token>& tokens,
  std::string& outError
) {
  (void)outError;

  const auto& lp = ctx.pack.lang;
  if (!lp.diphthongCollapseEnabled) return true;

  const int cf1 = static_cast<int>(FieldId::cf1);
  const int cf2 = static_cast<int>(FieldId::cf2);
  const int cf3 = static_cast<int>(FieldId::cf3);
  const int cb1 = static_cast<int>(FieldId::cb1);
  const int cb2 = static_cast<int>(FieldId::cb2);
  const int cb3 = static_cast<int>(FieldId::cb3);
  const int pb1 = static_cast<int>(FieldId::pb1);
  const int pb2 = static_cast<int>(FieldId::pb2);
  const int pb3 = static_cast<int>(FieldId::pb3);
  const int pf1 = static_cast<int>(FieldId::pf1);
  const int pf2 = static_cast<int>(FieldId::pf2);
  const int pf3 = static_cast<int>(FieldId::pf3);
  const int vp  = static_cast<int>(FieldId::voicePitch);
  const int evp = static_cast<int>(FieldId::endVoicePitch);

  // Scan for tied vowel pairs: A.tiedTo && B.tiedFrom.
  // Onset (A) must be a vowel; offglide (B) can be vowel OR semivowel.
  // Semivowel offglides arise from pack replacements (e.g. Spanish ɪ→ɪ_es
  // where ɪ_es has _isSemivowel: true) and must still collapse for smooth
  // micro-frame glide emission instead of relying on crossfade alone.
  // Iterate by index (not iterator) because we erase token B in place.
  for (size_t i = 0; i + 1 < tokens.size(); /* advanced inside */) {
    Token& a = tokens[i];
    Token& b = tokens[i + 1];

    if (!a.tiedTo || !b.tiedFrom || !tokIsVowel(a) || !tokIsValidOffglide(b)) {
      ++i;
      continue;
    }

    // === Merge B into A ===

    // Duration: combined.
    a.durationMs += b.durationMs;

    // Per-diphthong duration scaling (Gay 1968: F2 rate of change is fixed,
    // so wide diphthongs need proportionally more time than narrow ones).
    // Look up pair-specific scale; fall back to global diphthongDurationScale.
    {
      double pairScale = lp.diphthongDurationScale;
      if (!lp.diphthongPairScales.empty() && a.def && b.def) {
          for (const auto& ps : lp.diphthongPairScales) {
              if (ps.onset == a.def->key && ps.offset == b.def->key) {
                  pairScale = ps.scale;
                  break;
              }
          }
      }
      if (pairScale > 0.0 && pairScale != 1.0) {
          a.durationMs *= pairScale;
      }
    }

    // Rate compensation: at high speeds, undo some of the rate compression
    // so the formant glide has enough time to sweep naturally.
    // Exponent 0 = no compensation (current behaviour), 1.0 = fully
    // rate-invariant (Eloquence-style: glide duration independent of rate).
    if (lp.diphthongRateCompensation > 0.0 && ctx.speed > 1.0) {
      double comp = std::pow(ctx.speed, lp.diphthongRateCompensation);
      a.durationMs *= comp;
    }

    if (a.durationMs < lp.diphthongDurationFloorMs)
      a.durationMs = lp.diphthongDurationFloorMs;

    // Start formants: already in A's field[] (cf1/2/3, pf1/2/3).
    // End formants: take from B's field[] (what B's steady-state would be).
    // Use token-level field values when set, fall back to PhonemeDef.
    auto getField = [](const Token& t, int fid) -> double {
      if ((t.setMask & (1ull << fid)) != 0) return t.field[fid];
      if (t.def) {
        return t.def->field[fid];
      }
      return 0.0;
    };

    a.hasEndCf1 = true;  a.endCf1 = getField(b, cf1);
    a.hasEndCf2 = true;  a.endCf2 = getField(b, cf2);
    a.hasEndCf3 = true;  a.endCf3 = getField(b, cf3);

    // Parallel end targets: use B's parallel formants.
    // These will fall back to endCf in frame_emit if not explicitly set
    // on Token, but setting them here future-proofs for nasal diphthongs.
    a.hasEndPf1 = true;  a.endPf1 = getField(b, pf1);
    a.hasEndPf2 = true;  a.endPf2 = getField(b, pf2);
    a.hasEndPf3 = true;  a.endPf3 = getField(b, pf3);

    // End bandwidths: take B's cb1/2/3 so micro-frames can interpolate
    // bandwidths alongside frequencies.  Without this, onset bandwidths
    // are held constant — producing smeared, wobbly offsets.
    a.hasEndCb1 = true;  a.endCb1 = getField(b, cb1);
    a.hasEndCb2 = true;  a.endCb2 = getField(b, cb2);
    a.hasEndCb3 = true;  a.endCb3 = getField(b, cb3);
    a.hasEndPb1 = true;  a.endPb1 = getField(b, pb1);
    a.hasEndPb2 = true;  a.endPb2 = getField(b, pb2);
    a.hasEndPb3 = true;  a.endPb3 = getField(b, pb3);

    // Pitch: onset from A, offset from B.
    // A's voicePitch stays as-is.  Set endVoicePitch to B's pitch.
    double bPitch = getField(b, vp);
    if (bPitch > 0.0) {
      a.field[evp] = bPitch;
      a.setMask |= (1ull << evp);
    }

    // Flag it
    a.isDiphthongGlide = true;

    DCLOG("COLLAPSE speed=%.2f dur=%.1fms fade=%.1fms "
          "cf1=%.0f->%.0f cf2=%.0f->%.0f cf3=%.0f->%.0f "
          "cb1=%.0f->%.0f cb2=%.0f->%.0f\n",
          ctx.speed,
          a.durationMs, a.fadeMs,
          getField(a, cf1), a.endCf1,
          getField(a, cf2), a.endCf2,
          getField(a, cf3), a.endCf3,
          getField(a, cb1), a.endCb1,
          getField(a, cb2), a.endCb2);

    // Inherit A's syllableIndex, stress, wordStart, syllableStart (already there).
    // fadeMs from A (entry fade into the diphthong).
    // Clear tied flags — this is now a single merged token.
    a.tiedTo = false;
    a.tiedFrom = false;

    // Erase token B
    tokens.erase(tokens.begin() + static_cast<ptrdiff_t>(i + 1));

    // Do NOT double-merge triphthongs.
    // After collapsing [A,B] -> [AB], advance past the merged token.
    // If there was a triphthong [A,B,C] with A.tiedTo, B.tiedTo+tiedFrom,
    // C.tiedFrom, the first merge creates [AB,C].  AB has tiedTo=false,
    // so the next iteration won't merge AB+C.  Correct.
    ++i;
  }

  return true;
}

} // namespace nvsp_frontend::passes
