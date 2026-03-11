/*
TGSpeechBox — Token-to-FrameEx conversion and emission.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

// frame_emit.cpp - TGSpeechBox — Frame emission for frontend
//
// Extracted from ipa_engine.cpp to reduce file size.
// Contains emitFrames() and emitFramesEx() implementations.

#include "ipa_engine.h"
#include "passes/pass_common.h"
#include "../utils.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <type_traits>

// Debug logging for frame emission investigation.
// Set to 1 to enable, 0 to disable.
#define FEMIT_DEBUG_LOG 0
#if FEMIT_DEBUG_LOG
#include <cstdio>
#include <cstdlib>
static FILE* femitLogFile() {
  static FILE* f = nullptr;
  if (!f) {
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = "/tmp";
    std::string path = std::string(tmp) + "/tgsb_frame_emit.log";
    f = std::fopen(path.c_str(), "a");
  }
  return f;
}
#define FELOG(...) do { FILE* _f = femitLogFile(); if (_f) { std::fprintf(_f, __VA_ARGS__); std::fflush(_f); } } while(0)
#else
#define FELOG(...) ((void)0)
#endif

namespace nvsp_frontend {

// Helper to clamp a value to [0, 1]
static inline double clamp01(double v) {
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

// Helper to clamp sharpness multiplier to reasonable range
static inline double clampSharpness(double v) {
  if (v < 0.1) return 0.1;
  if (v > 5.0) return 5.0;
  return v;
}

// Adaptive onset hold for diphthong glides.
//
// The base onsetHoldExponent (e.g. 1.3) works well for narrow sweeps
// like FACE /eɪ/ (~300 Hz F2 delta), but crushes the offglide on wide
// sweeps like PRICE /ɑɪ/ (~600+ Hz F2 delta).  This function scales
// the exponent down for wide sweeps and enforces a minimum offglide
// duration so the diphthong identity isn't lost perceptually.
//
// References: Gay 1968, Lindblom & Studdert-Kennedy 1967 — ~40-50 ms
// of audible formant motion needed for diphthong offglide perception.
static double adaptiveOnsetHold(
    double baseHold,
    double startCf1, double endCf1,
    double startCf2, double endCf2,
    double totalDurMs,
    const Token* nextToken)  // nullptr if no following token
{
  double hold = baseHold;
  if (hold <= 1.0) return hold;

  // 1. Sweep-width scaling: wider sweeps need earlier motion onset.
  //    F2 is the primary perceptual cue for diphthong identity.
  const double sweepF2 = fabs(endCf2 - startCf2);
  const double sweepF1 = fabs(endCf1 - startCf1);
  const double sweepMax = std::max(sweepF2, sweepF1);

  // Narrow (<300 Hz): full hold.  Wide (>600 Hz): hold → floor.
  // Floor of 1.12 ensures wide diphthongs still linger briefly at onset
  // so the open vowel quality registers before the sweep begins — without
  // this, PRICE /aɪ/ sounds "chesty" at rates above ~70 because the F1/F2
  // sweep starts immediately and the onset never settles.
  constexpr double kMinHold = 1.12;
  if (sweepMax > 300.0) {
    double widthFrac = std::min((sweepMax - 300.0) / 300.0, 1.0);
    hold = hold + (kMinHold - hold) * widthFrac;
  }

  // 2. Following-context check: stops, glottal stops, and silence give
  //    zero coarticulatory runway — the offglide must complete in-token.
  bool nextIsAbrupt = false;
  if (!nextToken || nextToken->silence || nextToken->preStopGap) {
    nextIsAbrupt = true;
  } else if (nextToken->def &&
             (nextToken->def->flags & kIsStop)) {
    nextIsAbrupt = true;
  }

  // 3. Minimum offglide duration: ~40 ms of formant motion needed for
  //    the ear to register the diphthong.  If the hold eats too much
  //    time, reduce it so the sweep portion meets the floor.
  const double minOffglideMs = 40.0;
  if (totalDurMs > minOffglideMs && hold > 1.0 &&
      (nextIsAbrupt || sweepMax > 400.0)) {
    // With pow(frac, hold), meaningful sweep starts around frac=0.15^(1/hold).
    double sweepStart = pow(0.15, 1.0 / std::max(hold, 1.001));
    double effectiveSweepMs = totalDurMs * (1.0 - sweepStart);
    if (effectiveSweepMs < minOffglideMs) {
      // Back-solve: what exponent gives us minOffglideMs of sweep?
      double targetStart = 1.0 - (minOffglideMs / totalDurMs);
      if (targetStart > 0.01 && targetStart < 0.99) {
        hold = log(0.15) / log(targetStart);
        if (hold < 1.0) hold = 1.0;
      }
    }
  }

  return hold;
}

void emitFrames(
  const PackSet& pack,
  const std::vector<Token>& tokens,
  int userIndexBase,
  double speed,
  TrajectoryState* trajectoryState,
  nvspFrontend_FrameCallback cb,
  void* userData
) {
  if (!cb) return;

  // We intentionally treat nvspFrontend_Frame as a dense sequence of doubles.
  // Enforce that assumption at compile time so future edits fail loudly.
  static_assert(sizeof(nvspFrontend_Frame) == sizeof(double) * kFrameFieldCount,
                "nvspFrontend_Frame must remain exactly kFrameFieldCount doubles with no padding");
  static_assert(std::is_standard_layout<nvspFrontend_Frame>::value,
                "nvspFrontend_Frame must remain standard-layout");
  static_assert(std::is_trivially_copyable<nvspFrontend_Frame>::value,
                "nvspFrontend_Frame must remain trivially copyable");

  const bool trillEnabled = (pack.lang.trillModulationMs > 0.0);

  const int vp = static_cast<int>(FieldId::voicePitch);
  const int evp = static_cast<int>(FieldId::endVoicePitch);
  const int va = static_cast<int>(FieldId::voiceAmplitude);
  const int fa = static_cast<int>(FieldId::fricationAmplitude);

  // Trill modulation constants.
  //
  // We implement the trill as an amplitude modulation on voiceAmplitude using a
  // sequence of short frames (micro-frames). This keeps the speechPlayer.dll ABI
  // stable (no extra fields) while avoiding pack-level hacks such as duplicating
  // phoneme tokens.
  //
  // These constants were chosen to produce an audible trill without introducing
  // clicks or an overly "tremolo" sound. Packs can tune the trill duration and
  // micro-frame fade via settings, but not the depth (kept fixed for simplicity).
  constexpr double kTrillCloseFactor = 0.22;   // voiceAmplitude multiplier during closure
  constexpr double kTrillCloseFrac = 0.28;     // fraction of cycle spent in closure
  constexpr double kTrillFricFloor = 0.12;     // minimum fricationAmplitude during closure (if frication is present)
  // Minimum phase duration for the trill micro-frames. Keep this small so
  // very fast modulation settings (e.g. 2ms cycles) still behave as expected.
  constexpr double kMinPhaseMs = 0.25;


  // ============================================
  // TRAJECTORY LIMITING STATE (per-handle)
  // ============================================
  // Reset state at start of each utterance
  const LanguagePack& lang = pack.lang;
  trajectoryState->hasPrevFrame = false;
  trajectoryState->hasPrevBase = false;
  trajectoryState->hasPrevFrameEx = false;

  // Track previous frame values for voiced closure continuation
  bool hadPrevFrame = false;
  // Track whether previous real token was a stop/affricate/aspiration.
  // Used to skip fricative attack ramp in post-stop clusters (/ks/, /ts/, etc.)
  bool prevTokenWasStop = false;
  bool prevTokenWasTap = false;
  const double wbDipMs = lang.wordBoundaryDipMs;

  for (const Token& t : tokens) {
    // ============================================
    // VOICE BAR EMISSION (voiced stop closures)
    // ============================================
    // Build voice bar from the previous real frame (which has pitch, GOQ, outputGain,
    // vibrato — everything PhonemeDef lacks) then override just the voice-bar-specific
    // fields. Falls back to NULL frame if no previous base is available.
    if (t.voicedClosure && hadPrevFrame) {
      double vbFadeMs = t.fadeMs;
      if (vbFadeMs < 8.0) vbFadeMs = 8.0;

      if (trajectoryState->hasPrevBase) {
        double vb[kFrameFieldCount];
        std::memcpy(vb, trajectoryState->prevBase, sizeof(vb));

        double vbAmp = (t.def && t.def->hasVoiceBarAmplitude) ? t.def->voiceBarAmplitude : 0.3;
        double vbF1 = (t.def && t.def->hasVoiceBarF1) ? t.def->voiceBarF1 : 150.0;

        vb[va] = vbAmp;
        vb[fa] = 0.0;
        vb[static_cast<int>(FieldId::aspirationAmplitude)] = 0.0;
        vb[static_cast<int>(FieldId::cf1)] = vbF1;
        vb[static_cast<int>(FieldId::pf1)] = vbF1;
        vb[static_cast<int>(FieldId::preFormantGain)] = vbAmp;

        // Pre-position F2/F3 at the stop's own formants instead of the
        // previous vowel's.  This lets the cascade transition during the
        // voice bar so the burst fires into matching resonator state,
        // eliminating the interference pop at stop onsets.
        if (t.def) {
          auto vbField = [&](FieldId id) -> double {
            int idx = static_cast<int>(id);
            return (t.def->setMask & (1ULL << idx)) ? t.def->field[idx] : 0.0;
          };
          double sCf2 = vbField(FieldId::cf2);
          double sCf3 = vbField(FieldId::cf3);
          double sPf2 = vbField(FieldId::pf2);
          double sPf3 = vbField(FieldId::pf3);
          if (sCf2 > 0.0) vb[static_cast<int>(FieldId::cf2)] = sCf2;
          if (sCf3 > 0.0) vb[static_cast<int>(FieldId::cf3)] = sCf3;
          if (sPf2 > 0.0) vb[static_cast<int>(FieldId::pf2)] = sPf2;
          if (sPf3 > 0.0) vb[static_cast<int>(FieldId::pf3)] = sPf3;
        }

        nvspFrontend_Frame vbFrame;
        std::memcpy(&vbFrame, vb, sizeof(vbFrame));
        cb(userData, &vbFrame, t.durationMs, vbFadeMs, userIndexBase);
      } else {
        cb(userData, nullptr, t.durationMs, vbFadeMs, userIndexBase);
      }
      continue;
    }

    // ============================================
    // CODA NOISE TAPER (fricative→stop closure)
    // ============================================
    // Two micro-frames that crossfade the noise SOURCE from parallel-dominant
    // (sibilant) to cascade-dominant (aspirated).  This rotates the spectral
    // character across the closure so the burst fires into warm cascade
    // resonators instead of cold ones.  Same micro-frame pattern as burst
    // and release-spread code.
    //
    //   early taper:  parallel medium, cascade barely waking  → sibilant tail
    //   late taper:   parallel quiet,  cascade rising         → becoming aspirated
    //   /t/ burst:    cascade already warm with formant structure
    if (t.preStopGap && t.codaFricStopBlend && !t.voicedClosure && hadPrevFrame) {
      if (trajectoryState->hasPrevBase) {
        const double totalDur = t.durationMs;
        const double earlyDur = totalDur * 0.45;
        const double lateDur = totalDur - earlyDur;
        const double prevFric = trajectoryState->prevBase[fa];
        const int aspIdx = static_cast<int>(FieldId::aspirationAmplitude);
        const int pgIdx = static_cast<int>(FieldId::preFormantGain);

        // --- Early taper: sibilant tail (parallel still dominant) ---
        double early[kFrameFieldCount];
        std::memcpy(early, trajectoryState->prevBase, sizeof(early));
        early[va] = 0.0;  // voiceless closure
        early[fa] = prevFric * lang.codaNoiseTaperEarlyFricScale;
        early[aspIdx] = lang.codaNoiseTaperEarlyAspAmp;
        early[pgIdx] = lang.codaNoiseTaperPreGain;

        nvspFrontend_Frame earlyFrame;
        std::memcpy(&earlyFrame, early, sizeof(earlyFrame));
        cb(userData, &earlyFrame, earlyDur, t.fadeMs, userIndexBase);
        hadPrevFrame = true;

        // --- Late taper: aspirated transition (cascade now dominant) ---
        double late[kFrameFieldCount];
        std::memcpy(late, trajectoryState->prevBase, sizeof(late));
        late[va] = 0.0;
        late[fa] = prevFric * lang.codaNoiseTaperLateFricScale;
        late[aspIdx] = lang.codaNoiseTaperLateAspAmp;
        late[pgIdx] = lang.codaNoiseTaperPreGain;

        // Blend formants toward the stop's place of articulation.
        // The gap carries the stop's PhonemeDef (t.def), so we can read
        // its formant targets and lerp the late taper partway there.
        // This smooths the spectral handoff — especially for cross-place
        // clusters like /sk/ or /sp/ where the locus differs significantly.
        if (t.def) {
          const int cf2i = static_cast<int>(FieldId::cf2);
          const int cf3i = static_cast<int>(FieldId::cf3);
          const int pf2i = static_cast<int>(FieldId::pf2);
          const int pf3i = static_cast<int>(FieldId::pf3);
          constexpr double blend = 0.40;
          if (t.def->setMask & (1ull << cf2i))
            late[cf2i] += (t.def->field[cf2i] - late[cf2i]) * blend;
          if (t.def->setMask & (1ull << cf3i))
            late[cf3i] += (t.def->field[cf3i] - late[cf3i]) * blend;
          if (t.def->setMask & (1ull << pf2i))
            late[pf2i] += (t.def->field[pf2i] - late[pf2i]) * blend;
          if (t.def->setMask & (1ull << pf3i))
            late[pf3i] += (t.def->field[pf3i] - late[pf3i]) * blend;
        }

        nvspFrontend_Frame lateFrame;
        std::memcpy(&lateFrame, late, sizeof(lateFrame));
        double lateFade = std::max(earlyDur * 0.5, lateDur * 0.4);
        cb(userData, &lateFrame, lateDur, lateFade, userIndexBase);

        continue;
      }
    }

    if (t.silence || !t.def) {
      cb(userData, nullptr, t.durationMs, t.fadeMs, userIndexBase);
      continue;
    }

    // Build a dense array of doubles and memcpy into the frame.
    // This avoids UB from treating a struct as an array via pointer arithmetic.
    double base[kFrameFieldCount] = {};
    const std::uint64_t mask = t.setMask;
    for (int f = 0; f < kFrameFieldCount; ++f) {
      if ((mask & (1ull << f)) == 0) continue;
      base[f] = t.field[f];
    }

    // Save full base for voice bar emission on the next voiced closure.
    std::memcpy(trajectoryState->prevBase, base, sizeof(base));
    trajectoryState->hasPrevBase = true;

    // Rate-adaptive bandwidth widening: widen cascade bandwidths at high speeds
    // so resonators settle faster and express formant identity sooner.
    // Applied AFTER prevBase save so voice bar inherits natural bandwidths.
    {
      const double hrT = pack.lang.highRateThreshold;
      const double bwF = pack.lang.highRateBandwidthWideningFactor;
      if (hrT > 0.0 && bwF > 1.0 && speed > hrT) {
        const double ceiling = hrT * 1.8;
        const double ramp = std::min((speed - hrT) / (ceiling - hrT), 1.0);
        const double bwScale = 1.0 + ramp * (bwF - 1.0);
        base[static_cast<int>(FieldId::cb1)] *= bwScale;
        base[static_cast<int>(FieldId::cb2)] *= bwScale;
        base[static_cast<int>(FieldId::cb3)] *= bwScale;
      }
    }

    // DIPHTHONG GLIDE (legacy path -- no FrameEx, no endCf guidance)
    if (t.isDiphthongGlide && t.durationMs > 0.0) {
      const double totalDur = t.durationMs;
      double intervalMs = pack.lang.diphthongMicroFrameIntervalMs;
      const double pitch0Leg = base[vp];
      if (pitch0Leg > 0.0) {
        // Scale interval proportionally to pitch period so each micro-frame
        // covers roughly the same number of glottal cycles regardless of F0.
        intervalMs *= (100.0 / pitch0Leg);
        // Floor: each micro-frame must span at least 2 full pitch periods
        // so cascade resonators settle before formants shift again.
        // Without this, declining F0 (statement intonation) produces
        // shimmer because micro-frames are shorter than one glottal cycle.
        double minInterval = 2000.0 / pitch0Leg;
        if (intervalMs < minInterval) intervalMs = minInterval;
        if (intervalMs < 3.0) intervalMs = 3.0;
      }
      int N = (intervalMs > 0.0)
            ? std::max(5, static_cast<int>(std::round(totalDur / intervalMs)))
            : 3;

      const double startCf1 = base[static_cast<int>(FieldId::cf1)];
      const double startCf2 = base[static_cast<int>(FieldId::cf2)];
      const double startCf3 = base[static_cast<int>(FieldId::cf3)];
      const double startPf1 = base[static_cast<int>(FieldId::pf1)];
      const double startPf2 = base[static_cast<int>(FieldId::pf2)];
      const double startPf3 = base[static_cast<int>(FieldId::pf3)];

      // End targets: use token-level endCf (set by diphthong_collapse pass).
      const double endCf1 = t.hasEndCf1 ? t.endCf1 : startCf1;
      const double endCf2 = t.hasEndCf2 ? t.endCf2 : startCf2;
      const double endCf3 = t.hasEndCf3 ? t.endCf3 : startCf3;
      const double endPf1V = t.hasEndPf1 ? t.endPf1 : endCf1;
      const double endPf2V = t.hasEndPf2 ? t.endPf2 : endCf2;
      const double endPf3V = t.hasEndPf3 ? t.endPf3 : endCf3;

      // End bandwidths: use offset vowel's values if set, else hold onset.
      const double startCb1 = base[static_cast<int>(FieldId::cb1)];
      const double startCb2 = base[static_cast<int>(FieldId::cb2)];
      const double startCb3 = base[static_cast<int>(FieldId::cb3)];
      const double startPb1 = base[static_cast<int>(FieldId::pb1)];
      const double startPb2 = base[static_cast<int>(FieldId::pb2)];
      const double startPb3 = base[static_cast<int>(FieldId::pb3)];
      const double endCb1 = t.hasEndCb1 ? t.endCb1 : startCb1;
      const double endCb2 = t.hasEndCb2 ? t.endCb2 : startCb2;
      const double endCb3 = t.hasEndCb3 ? t.endCb3 : startCb3;
      const double endPb1 = t.hasEndPb1 ? t.endPb1 : startPb1;
      const double endPb2 = t.hasEndPb2 ? t.endPb2 : startPb2;
      const double endPb3 = t.hasEndPb3 ? t.endPb3 : startPb3;

      const double startPitch = base[vp];
      const double endPitch = base[evp];
      const double pitchDelta = endPitch - startPitch;
      const double dipFactor = pack.lang.diphthongAmplitudeDipFactor;
      const double baseSegDur = totalDur / N;

      // Adaptive onset hold (same logic as Ex path).
      const Token* nextTok = (&t < &tokens.back()) ? (&t + 1) : nullptr;
      const double onsetHold = adaptiveOnsetHold(
          pack.lang.diphthongOnsetHoldExponent,
          startCf1, endCf1, startCf2, endCf2,
          totalDur, nextTok);

      // Onset settle: give the first micro-frame extra time so the
      // resonator can settle from the preceding consonant before the
      // formant sweep begins.  Borrowed from remaining segments.
      double seg0Dur = baseSegDur;
      double otherSegDur = baseSegDur;
      const double settleMs = pack.lang.diphthongOnsetSettleMs;
      if (settleMs > 0.0 && N > 1) {
        seg0Dur = std::min(baseSegDur + settleMs, totalDur * 0.5);
        otherSegDur = (totalDur - seg0Dur) / (N - 1);
      }

      for (int seg = 0; seg < N; ++seg) {
        double frac = (N > 1) ? (static_cast<double>(seg) / (N - 1)) : 0.0;
        if (onsetHold > 1.0) frac = pow(frac, onsetHold);
        double s = cosineSmooth(frac);

        double mf[kFrameFieldCount];
        std::memcpy(mf, base, sizeof(mf));

        mf[static_cast<int>(FieldId::cf1)] = calculateFreqAtFadePosition(startCf1, endCf1, s);
        mf[static_cast<int>(FieldId::cf2)] = calculateFreqAtFadePosition(startCf2, endCf2, s);
        mf[static_cast<int>(FieldId::cf3)] = calculateFreqAtFadePosition(startCf3, endCf3, s);
        mf[static_cast<int>(FieldId::pf1)] = calculateFreqAtFadePosition(startPf1, endPf1V, s);
        mf[static_cast<int>(FieldId::pf2)] = calculateFreqAtFadePosition(startPf2, endPf2V, s);
        mf[static_cast<int>(FieldId::pf3)] = calculateFreqAtFadePosition(startPf3, endPf3V, s);
        mf[static_cast<int>(FieldId::cb1)] = calculateFreqAtFadePosition(startCb1, endCb1, s);
        mf[static_cast<int>(FieldId::cb2)] = calculateFreqAtFadePosition(startCb2, endCb2, s);
        mf[static_cast<int>(FieldId::cb3)] = calculateFreqAtFadePosition(startCb3, endCb3, s);
        mf[static_cast<int>(FieldId::pb1)] = calculateFreqAtFadePosition(startPb1, endPb1, s);
        mf[static_cast<int>(FieldId::pb2)] = calculateFreqAtFadePosition(startPb2, endPb2, s);
        mf[static_cast<int>(FieldId::pb3)] = calculateFreqAtFadePosition(startPb3, endPb3, s);

        double t0 = (N > 1) ? (static_cast<double>(seg) / N) : 0.0;
        double t1 = (N > 1) ? (static_cast<double>(seg + 1) / N) : 1.0;
        mf[vp]  = startPitch + pitchDelta * t0;
        mf[evp] = startPitch + pitchDelta * t1;

        if (dipFactor > 0.0) {
          double ampScale = 1.0 - dipFactor * sin(M_PI * frac);
          mf[va] *= ampScale;
        }

        nvspFrontend_Frame frame;
        std::memcpy(&frame, mf, sizeof(frame));

        const double thisDur = (seg == 0) ? seg0Dur : otherSegDur;
        double fadeIn = (seg == 0) ? t.fadeMs : thisDur;
        // Cap entry fade after obstruents (see Ex path comment).
        if (seg == 0 && trajectoryState->hasPrevFrame &&
            trajectoryState->prevVoiceAmp < 0.05) {
          fadeIn = std::min(fadeIn, 4.0);
        }
        if (fadeIn > thisDur) fadeIn = thisDur;

        cb(userData, &frame, thisDur, fadeIn, userIndexBase);
      }

      trajectoryState->prevCf2 = calculateFreqAtFadePosition(startCf2, endCf2, 1.0);
      trajectoryState->prevCf3 = calculateFreqAtFadePosition(startCf3, endCf3, 1.0);
      trajectoryState->prevPf2 = calculateFreqAtFadePosition(startPf2, endPf2V, 1.0);
      trajectoryState->prevPf3 = calculateFreqAtFadePosition(startPf3, endPf3V, 1.0);
      trajectoryState->prevVoiceAmp = base[va];
      trajectoryState->prevFricAmp = base[fa];
      trajectoryState->hasPrevFrame = true;
      trajectoryState->prevWasNasal = false;
      prevTokenWasTap = false;
      prevTokenWasStop = false;
      continue;
    }

    // Optional trill modulation (only when `_isTrill` is true for the phoneme).
    if (trillEnabled && tokenIsTrill(t) && t.durationMs > 0.0) {
      double totalDur = t.durationMs;

      // Trill flutter speed is hardcoded to a natural-sounding ~35Hz.
      // The pack setting (trillModulationMs) controls the *total duration* via calculateTimes().
      constexpr double kFixedTrillCycleMs = 28.0;

      double cycleMs = kFixedTrillCycleMs;

      // For short trills, compress the cycle so we still get at least one closure dip.
      if (cycleMs > totalDur) cycleMs = totalDur;

      // Split the cycle into an "open" and "closure" phase.
      double closeMs = cycleMs * kTrillCloseFrac;
      double openMs = cycleMs - closeMs;

      // Keep both phases non-trivial (prevents zero-length frames).
      if (openMs < kMinPhaseMs) {
        openMs = kMinPhaseMs;
        closeMs = std::max(kMinPhaseMs, cycleMs - openMs);
      }
      if (closeMs < kMinPhaseMs) {
        closeMs = kMinPhaseMs;
        openMs = std::max(kMinPhaseMs, cycleMs - closeMs);
      }

      // Fade between micro-frames. If not configured, choose a small default
      // relative to the cycle.
      double microFadeMs = pack.lang.trillModulationFadeMs;
      if (microFadeMs <= 0.0) {
        microFadeMs = std::min(2.0, cycleMs * 0.12);
      }

      const bool hasVoiceAmp = ((mask & (1ull << va)) != 0);
      const bool hasFricAmp = ((mask & (1ull << fa)) != 0);
      const double baseVoiceAmp = base[va];
      const double baseFricAmp = base[fa];

      const double startPitch = base[vp];
      const double endPitch = base[evp];
      const double pitchDelta = endPitch - startPitch;

      double remaining = totalDur;
      double pos = 0.0;
      bool highPhase = true;
      bool firstPhase = true;

      while (remaining > 1e-9) {
        double phaseDur = highPhase ? openMs : closeMs;
        if (phaseDur > remaining) phaseDur = remaining;

        // Interpolate pitch over the original token's duration so pitch remains continuous.
        double t0 = (totalDur > 0.0) ? (pos / totalDur) : 0.0;
        double t1 = (totalDur > 0.0) ? ((pos + phaseDur) / totalDur) : 1.0;

        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));

        seg[vp] = startPitch + pitchDelta * t0;
        seg[evp] = startPitch + pitchDelta * t1;

        if (!highPhase) {
          if (hasVoiceAmp) {
            seg[va] = baseVoiceAmp * kTrillCloseFactor;
          }
          // Add a small noise burst on closure to make the trill more perceptible,
          // but only if the phoneme already has a frication path.
          if (hasFricAmp && baseFricAmp > 0.0) {
            seg[fa] = std::max(baseFricAmp, kTrillFricFloor);
          }
        }

        nvspFrontend_Frame frame;
        std::memcpy(&frame, seg, sizeof(frame));

        // In speechPlayer.dll, the fade duration belongs to the *incoming* frame
        // (it's the crossfade from the previous frame to this one). Preserve the
        // token's original fade on entry to the trill, then use microFadeMs for
        // the internal micro-frame boundaries.
        double fadeIn = firstPhase ? t.fadeMs : microFadeMs;
        if (fadeIn <= 0.0) fadeIn = microFadeMs;

        // Prevent fade dominating very short micro-frames.
        if (fadeIn > phaseDur) fadeIn = phaseDur;

        cb(userData, &frame, phaseDur, fadeIn, userIndexBase);
        hadPrevFrame = true;

        remaining -= phaseDur;
        pos += phaseDur;
        highPhase = !highPhase;
        firstPhase = false;

        // If the remaining duration is too small to fit another phase, let the loop
        // handle it naturally by truncating phaseDur above.
      }

      prevTokenWasStop = false;
      continue;
    }

    // ============================================
    // STOP BURST MICRO-FRAME EMISSION
    // ============================================
    // Stops and affricates get 2 micro-frames: a short burst followed by
    // a decayed residual. This replaces the single flat frame with a
    // time-varying amplitude envelope that models real stop releases.
    // Only fires on the main stop token, not gap/closure/aspiration satellites.
    {
      const bool isStop = t.def && ((t.def->flags & kIsStop) != 0);
      const bool isAffricate = t.def && ((t.def->flags & kIsAfricate) != 0);

      if ((isStop || isAffricate) && !t.silence && !t.preStopGap &&
          !t.postStopAspiration && !t.voicedClosure && t.durationMs > 1.0) {

        // Word-boundary silence gap before word-initial stops (same as Ex path)
        double stopMainDur = t.durationMs;
        double stopMainFade = t.fadeMs;
        if (wbDipMs > 0.0 && t.wordStart && hadPrevFrame) {
          double dip[kFrameFieldCount];
          std::memcpy(dip, base, sizeof(dip));
          dip[va] = 0.0;
          dip[fa] = 0.0;
          dip[static_cast<int>(FieldId::aspirationAmplitude)] = 0.0;
          dip[static_cast<int>(FieldId::preFormantGain)] = 0.0;

          nvspFrontend_Frame dipFrame;
          std::memcpy(&dipFrame, dip, sizeof(dipFrame));
          cb(userData, &dipFrame, wbDipMs, 1.0, userIndexBase);
          hadPrevFrame = true;
          stopMainFade = 1.0;
        }

        Place place = getPlace(t.def->key);

        // Place-based defaults (from Cho & Ladefoged 1999, Stevens 1998)
        double burstMs = 7.0;
        double decayRate = 0.5;
        double spectralTilt = 0.0;

        switch (place) {
          case Place::Labial:   burstMs = 5.0;  decayRate = 0.6;  spectralTilt = 0.1;   break;
          case Place::Alveolar: burstMs = 7.0;  decayRate = 0.5;  spectralTilt = 0.0;   break;
          case Place::Velar:    burstMs = 11.0; decayRate = 0.4;  spectralTilt = -0.15; break;
          case Place::Palatal:  burstMs = 9.0;  decayRate = 0.45; spectralTilt = -0.1;  break;
          default: break;
        }

        // Phoneme-level overrides
        if (t.def->hasBurstDurationMs) burstMs = t.def->burstDurationMs;
        if (t.def->hasBurstDecayRate) decayRate = t.def->burstDecayRate;
        if (t.def->hasBurstSpectralTilt) spectralTilt = t.def->burstSpectralTilt;

        // Coda blend: the burst continues the taper, not a fresh onset.
        // Use longer burst for smoother decay — the taper already did the attack.
        if (t.codaFricStopBlend && !isAffricate) {
          burstMs = std::max(burstMs, stopMainDur * 0.6);
          decayRate = 0.7;  // faster frication decay (taper is doing the work)
        }

        // Clamp burst to 75% of token duration so it always fires,
        // even at high speech rates. Preserves place differentiation.
        double maxBurst = stopMainDur * 0.75;
        if (burstMs > maxBurst) burstMs = maxBurst;

        {
          // Pitch interpolation: slice the token's pitch ramp proportionally
          const double startPitch = base[vp];
          const double pitchDelta = base[evp] - startPitch;
          const double totalDur = stopMainDur;
          const double burstFrac = burstMs / totalDur;

          // --- Burst micro-frame ---
          double seg1[kFrameFieldCount];
          std::memcpy(seg1, base, sizeof(seg1));
          seg1[vp] = startPitch;
          seg1[evp] = startPitch + pitchDelta * burstFrac;

          const int pa3i = static_cast<int>(FieldId::pa3);
          const int pa4i = static_cast<int>(FieldId::pa4);
          const int pa5i = static_cast<int>(FieldId::pa5);
          const int pa6i = static_cast<int>(FieldId::pa6);

          if (spectralTilt < 0.0) {
            seg1[pa5i] = std::min(1.0, seg1[pa5i] * (1.0 - spectralTilt));
            seg1[pa6i] = std::min(1.0, seg1[pa6i] * (1.0 - spectralTilt * 0.7));
          } else if (spectralTilt > 0.0) {
            seg1[pa3i] = std::min(1.0, seg1[pa3i] * (1.0 + spectralTilt));
            seg1[pa4i] = std::min(1.0, seg1[pa4i] * (1.0 + spectralTilt * 0.7));
          }

          nvspFrontend_Frame burstFrame;
          std::memcpy(&burstFrame, seg1, sizeof(burstFrame));
          cb(userData, &burstFrame, burstMs, stopMainFade, userIndexBase);
          hadPrevFrame = true;

          // --- Decay micro-frame ---
          double seg2[kFrameFieldCount];
          std::memcpy(seg2, base, sizeof(seg2));
          seg2[vp] = startPitch + pitchDelta * burstFrac;
          seg2[evp] = startPitch + pitchDelta;  // end of token

          const int faIdx = static_cast<int>(FieldId::fricationAmplitude);
          // Stops: decay frication (burst is the only noise source, it should die).
          // Affricates: keep frication at full — the whole point is sustained frication
          // after the burst transient. Only the spectral tilt boost decays.
          if (!isAffricate) {
            seg2[faIdx] *= (1.0 - decayRate);
          }

          nvspFrontend_Frame decayFrame;
          std::memcpy(&decayFrame, seg2, sizeof(decayFrame));
          double decayDur = stopMainDur - burstMs;
          double decayFade = std::min(burstMs * 0.5, decayDur);
          cb(userData, &decayFrame, decayDur, decayFade, userIndexBase);

          // Update trajectory state
          trajectoryState->prevCf2 = burstFrame.cf2;
          trajectoryState->prevCf3 = burstFrame.cf3;
          trajectoryState->prevPf2 = burstFrame.pf2;
          trajectoryState->prevPf3 = burstFrame.pf3;
          trajectoryState->prevVoiceAmp = base[va];
          trajectoryState->prevFricAmp = base[fa];
          trajectoryState->hasPrevFrame = true;
          trajectoryState->prevWasNasal = false;

          prevTokenWasStop = true;
          continue;
        }
      }
    }

    // ============================================
    // FRICATIVE ATTACK/DECAY MICRO-FRAME EMISSION
    // ============================================
    // Fricatives get 3 micro-frames: attack (ramp up), sustain, decay (ramp down).
    // This replaces the flat fricationAmplitude with a shaped envelope.
    // Only fires on non-stop fricatives (stops already handled above).
    {
      const bool isStop = t.def && ((t.def->flags & kIsStop) != 0);
      const bool isAffricate = t.def && ((t.def->flags & kIsAfricate) != 0);
      const double fricAmp = base[fa];

      if (!isStop && !isAffricate && !t.silence && !t.preStopGap &&
          !t.postStopAspiration && !t.voicedClosure && fricAmp > 0.0) {

        double attackMs = t.def->hasFricAttackMs ? t.def->fricAttackMs : 3.0;
        double decayMs = t.def->hasFricDecayMs ? t.def->fricDecayMs : 4.0;

        // Post-stop fricatives (/kʃ/ in "action", /ks/ in "box"):
        // Use a shortened attack ramp so the stop burst has headroom to
        // register as a separate articulation before the fricative takes
        // over.  Without this, the fricative slams in at full amplitude
        // and masks the preceding burst (especially problematic for /kʃ/
        // where the spectral overlap in 1.5-3 kHz is heavy).
        if (prevTokenWasStop) {
          attackMs = std::min(attackMs, 2.5);
        }

        // Only emit micro-frames if token is long enough for attack + decay + 2ms sustain
        if (attackMs + decayMs + 2.0 < t.durationMs) {
          const int faIdx = static_cast<int>(FieldId::fricationAmplitude);

          // Pitch interpolation across 3 micro-frames
          const double startPitch = base[vp];
          const double pitchDelta = base[evp] - startPitch;
          const double totalDur = t.durationMs;
          const double sustainDur = totalDur - attackMs - decayMs;
          const double attackFrac = attackMs / totalDur;
          const double sustainEndFrac = (attackMs + sustainDur) / totalDur;

          // --- Attack micro-frame: ramp from 10% to full ---
          double seg1[kFrameFieldCount];
          std::memcpy(seg1, base, sizeof(seg1));
          seg1[faIdx] = fricAmp * 0.1;
          seg1[vp] = startPitch;
          seg1[evp] = startPitch + pitchDelta * attackFrac;

          nvspFrontend_Frame attackFrame;
          std::memcpy(&attackFrame, seg1, sizeof(attackFrame));
          cb(userData, &attackFrame, attackMs, t.fadeMs, userIndexBase);
          hadPrevFrame = true;

          // --- Sustain micro-frame: full amplitude ---
          double seg2[kFrameFieldCount];
          std::memcpy(seg2, base, sizeof(seg2));
          seg2[vp] = startPitch + pitchDelta * attackFrac;
          seg2[evp] = startPitch + pitchDelta * sustainEndFrac;

          nvspFrontend_Frame sustainFrame;
          std::memcpy(&sustainFrame, seg2, sizeof(sustainFrame));
          cb(userData, &sustainFrame, sustainDur, attackMs, userIndexBase);

          // --- Decay micro-frame: ramp from full to 30% ---
          double seg3[kFrameFieldCount];
          std::memcpy(seg3, base, sizeof(seg3));
          seg3[faIdx] = fricAmp * 0.3;
          seg3[vp] = startPitch + pitchDelta * sustainEndFrac;
          seg3[evp] = startPitch + pitchDelta;

          nvspFrontend_Frame decayFrame;
          std::memcpy(&decayFrame, seg3, sizeof(decayFrame));
          cb(userData, &decayFrame, decayMs, decayMs * 0.5, userIndexBase);

          // Update trajectory state
          trajectoryState->prevCf2 = sustainFrame.cf2;
          trajectoryState->prevCf3 = sustainFrame.cf3;
          trajectoryState->prevPf2 = sustainFrame.pf2;
          trajectoryState->prevPf3 = sustainFrame.pf3;
          trajectoryState->prevVoiceAmp = base[va];
          trajectoryState->prevFricAmp = base[fa];
          trajectoryState->hasPrevFrame = true;
          trajectoryState->prevWasNasal = false;

          prevTokenWasStop = false;
          continue;
        }
        // else: token too short, fall through to normal emission
      }
    }

    // ============================================
    // RELEASE SPREAD (postStopAspiration tokens)
    // ============================================
    // Instead of instant aspiration onset, ramp in gradually over releaseSpreadMs.
    // Emit 2 micro-frames: a quiet ramp-in followed by full aspiration.
    // Coda blend: skip the ramp-in — we're continuing the taper, not starting fresh.
    if (t.postStopAspiration && t.def && t.durationMs > 1.0) {
      if (t.codaFricStopBlend) {
        // Single frame with moderate aspiration that decays naturally.
        // No ramp-in dip — the taper→burst already provided continuity.
        const int aspIdx = static_cast<int>(FieldId::aspirationAmplitude);
        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));
        seg[aspIdx] *= 0.60;  // start at 60% — decays into silence

        nvspFrontend_Frame aspFrame;
        std::memcpy(&aspFrame, seg, sizeof(aspFrame));
        cb(userData, &aspFrame, t.durationMs, t.durationMs * 0.5, userIndexBase);

        trajectoryState->prevCf2 = aspFrame.cf2;
        trajectoryState->prevCf3 = aspFrame.cf3;
        trajectoryState->prevPf2 = aspFrame.pf2;
        trajectoryState->prevPf3 = aspFrame.pf3;
        trajectoryState->prevVoiceAmp = base[va];
        trajectoryState->prevFricAmp = base[fa];
        trajectoryState->hasPrevFrame = true;
        trajectoryState->prevWasNasal = false;

        prevTokenWasStop = false;
        continue;
      }

      // Look up releaseSpreadMs from the aspiration phoneme's def (or default 4ms)
      double spreadMs = t.def->hasReleaseSpreadMs ? t.def->releaseSpreadMs : 4.0;

      if (spreadMs > 0.0 && spreadMs < t.durationMs) {
        const int faIdx = static_cast<int>(FieldId::fricationAmplitude);
        const int aspIdx = static_cast<int>(FieldId::aspirationAmplitude);

        // Pitch interpolation across 2 micro-frames
        const double startPitch = base[vp];
        const double pitchDelta = base[evp] - startPitch;
        const double totalDur = t.durationMs;
        const double spreadFrac = (totalDur > 0.0) ? (spreadMs / totalDur) : 0.0;

        // --- Ramp-in micro-frame: low aspiration/frication ---
        double seg1[kFrameFieldCount];
        std::memcpy(seg1, base, sizeof(seg1));
        seg1[faIdx] *= 0.15;
        seg1[aspIdx] *= 0.15;
        seg1[vp] = startPitch;
        seg1[evp] = startPitch + pitchDelta * spreadFrac;

        nvspFrontend_Frame rampFrame;
        std::memcpy(&rampFrame, seg1, sizeof(rampFrame));
        cb(userData, &rampFrame, spreadMs, t.fadeMs, userIndexBase);
        hadPrevFrame = true;

        // --- Full aspiration micro-frame ---
        double seg2[kFrameFieldCount];
        std::memcpy(seg2, base, sizeof(seg2));
        seg2[vp] = startPitch + pitchDelta * spreadFrac;
        seg2[evp] = startPitch + pitchDelta;

        nvspFrontend_Frame fullFrame;
        std::memcpy(&fullFrame, seg2, sizeof(fullFrame));
        double fullDur = t.durationMs - spreadMs;
        cb(userData, &fullFrame, fullDur, spreadMs * 0.5, userIndexBase);

        trajectoryState->prevCf2 = fullFrame.cf2;
        trajectoryState->prevCf3 = fullFrame.cf3;
        trajectoryState->prevPf2 = fullFrame.pf2;
        trajectoryState->prevPf3 = fullFrame.pf3;
        trajectoryState->prevVoiceAmp = base[va];
        trajectoryState->prevFricAmp = base[fa];
        trajectoryState->hasPrevFrame = true;
        trajectoryState->prevWasNasal = false;

        prevTokenWasStop = true;  // aspiration is stop-related
        continue;
      }
      // else: spread fills entire token or zero, fall through to normal emission
    }

    // TAP MICRO-EVENT (v1 path — see emitFramesEx for full rationale).
    const bool isTap = t.def && ((t.def->flags & kIsTap) != 0);
    // Micro-event notch only makes sense when the tap is long enough for
    // 3 phases to ring up properly.  Below ~8 ms the recovery phase is
    // so short it creates a burst-like transient ("fourgy" for "forty").
    // Fall through to normal single-frame emission for very short taps.
    if (isTap && t.durationMs >= 8.0) {
      const double totalDur = t.durationMs;
      const double notchFloorMs = 1.5;
      double notchDur = std::max(totalDur * 0.50, notchFloorMs);
      if (notchDur > totalDur * 0.80) notchDur = totalDur * 0.80;
      const double remainDur = totalDur - notchDur;
      // Asymmetric split: short onset, longer recovery.  Sharp entry into
      // the tap; recovery bridges back to the following vowel.
      const double onsetDur = remainDur * 0.35;
      const double recovDur = remainDur - onsetDur;

      const int vaIdx = static_cast<int>(FieldId::voiceAmplitude);
      const double notchAmp = base[vaIdx] * 0.50;

      const double startPitch = base[vp];
      const double pitchDelta = base[evp] - startPitch;
      const double onsetFrac = (totalDur > 0.0) ? (onsetDur / totalDur) : 0.0;
      const double notchEndFrac = (totalDur > 0.0) ? ((onsetDur + notchDur) / totalDur) : 0.0;
      // Short micro-fade for all phases (see Ex path comment).
      const double microFade = std::max(0.5, 1.5 / std::max(0.5, speed));

      { double seg[kFrameFieldCount]; std::memcpy(seg, base, sizeof(seg));
        seg[vp] = startPitch; seg[evp] = startPitch + pitchDelta * onsetFrac;
        nvspFrontend_Frame f; std::memcpy(&f, seg, sizeof(f));
        cb(userData, &f, onsetDur, microFade, userIndexBase); hadPrevFrame = true; }

      { double seg[kFrameFieldCount]; std::memcpy(seg, base, sizeof(seg));
        seg[vaIdx] = notchAmp;
        seg[vp] = startPitch + pitchDelta * onsetFrac; seg[evp] = startPitch + pitchDelta * notchEndFrac;
        nvspFrontend_Frame f; std::memcpy(&f, seg, sizeof(f));
        cb(userData, &f, notchDur, microFade, userIndexBase); }

      { double seg[kFrameFieldCount]; std::memcpy(seg, base, sizeof(seg));
        seg[vp] = startPitch + pitchDelta * notchEndFrac; seg[evp] = startPitch + pitchDelta;
        nvspFrontend_Frame f; std::memcpy(&f, seg, sizeof(f));
        cb(userData, &f, recovDur, microFade, userIndexBase); }

      trajectoryState->prevVoiceAmp = base[va];
      trajectoryState->prevFricAmp = base[fa];
      trajectoryState->hasPrevFrame = true;
      trajectoryState->prevWasNasal = false;
      prevTokenWasTap = true;
      prevTokenWasStop = false;
      continue;
    }

    // ============================================
    // WORD-BOUNDARY AMPLITUDE DIP
    // ============================================
    // Emit a brief reduced-amplitude micro-frame at every word start.
    // Uniform cue: V#V, C#V, V#C all get the same subtle boundary marker.
    // For C#C the consonant's own closure already provides the cue, but the
    // extra dip just reinforces it harmlessly.
    double mainDur = t.durationMs;
    double mainFade = t.fadeMs;

    if (wbDipMs > 0.0 && t.wordStart && hadPrevFrame && mainDur > wbDipMs + 1.0) {
      double dipDepth = lang.wordBoundaryDipDepth;

      // After a stop, deepen the dip — the stop's release can smear into
      // the next word's onset, especially at high rates.
      if (prevTokenWasStop) {
        dipDepth *= 0.5;
      }

      const double dipDur = wbDipMs;

      double dip[kFrameFieldCount];
      std::memcpy(dip, base, sizeof(dip));
      dip[va] *= dipDepth;
      dip[fa] *= dipDepth;
      dip[static_cast<int>(FieldId::aspirationAmplitude)] *= dipDepth;

      nvspFrontend_Frame dipFrame;
      std::memcpy(&dipFrame, dip, sizeof(dipFrame));
      cb(userData, &dipFrame, dipDur, mainFade, userIndexBase);
      hadPrevFrame = true;

      mainDur -= dipDur;
      mainFade = dipDur; // crossfade from dip back to full amplitude
    }

    nvspFrontend_Frame frame;
    std::memcpy(&frame, base, sizeof(frame));

    const bool isNasal = t.def && ((t.def->flags & kIsNasal) != 0);
    trajectoryState->prevVoiceAmp = base[va];
    trajectoryState->prevFricAmp = base[fa];
    trajectoryState->hasPrevFrame = true;
    trajectoryState->prevWasNasal = isNasal;

    double emitFade = mainFade;
    if (prevTokenWasTap && mainDur > 0.0) {
      emitFade = std::min(emitFade, mainDur * 0.50);
    }

    // Pitch-dependent fade floor: at low F0, each glottal cycle is longer
    // so crossfades span fewer cycles and individual frame boundaries become
    // audible (choppy at low pitch). Enforce a minimum of 2 glottal cycles.
    if (base[vp] > 0.0) {
      double minFadeMs = 2000.0 / base[vp]; // 2 cycles at current pitch
      if (emitFade < minFadeMs && mainDur > minFadeMs) {
        emitFade = minFadeMs;
      }
    }

    cb(userData, &frame, mainDur, emitFade, userIndexBase);
    hadPrevFrame = true;

    prevTokenWasTap = false;
    prevTokenWasStop = t.def && (
      ((t.def->flags & kIsStop) != 0) ||
      ((t.def->flags & kIsAfricate) != 0) ||
      t.postStopAspiration);
  }
}

void emitFramesEx(
  const PackSet& pack,
  const std::vector<Token>& tokens,
  int userIndexBase,
  double speed,
  const nvspFrontend_FrameEx& frameExDefaults,
  TrajectoryState* trajectoryState,
  nvspFrontend_FrameExCallback cb,
  void* userData
) {
  if (!cb) return;

  // Same static asserts as emitFrames
  static_assert(sizeof(nvspFrontend_Frame) == sizeof(double) * kFrameFieldCount,
                "nvspFrontend_Frame must remain exactly kFrameFieldCount doubles with no padding");
  static_assert(std::is_standard_layout<nvspFrontend_Frame>::value,
                "nvspFrontend_Frame must remain standard-layout");
  static_assert(std::is_trivially_copyable<nvspFrontend_Frame>::value,
                "nvspFrontend_Frame must remain trivially copyable");

  const bool trillEnabled = (pack.lang.trillModulationMs > 0.0);

  const int vp = static_cast<int>(FieldId::voicePitch);
  const int evp = static_cast<int>(FieldId::endVoicePitch);
  const int va = static_cast<int>(FieldId::voiceAmplitude);
  const int fa = static_cast<int>(FieldId::fricationAmplitude);

  // Trill modulation constants (same as emitFrames)
  constexpr double kTrillCloseFactor = 0.22;
  constexpr double kTrillCloseFrac = 0.28;
  constexpr double kTrillFricFloor = 0.12;
  constexpr double kMinPhaseMs = 0.25;

  // Trajectory limiting state (per-handle, reset at utterance start)
  const LanguagePack& lang = pack.lang;
  trajectoryState->hasPrevFrame = false;
  trajectoryState->hasPrevBase = false;
  trajectoryState->hasPrevFrameEx = false;

  // Track whether we've emitted at least one real frame
  bool hadPrevFrame = false;
  bool prevTokenWasStop = false;
  bool prevTokenWasTap = false;
  const double wbDipMs = lang.wordBoundaryDipMs;

  for (const Token& t : tokens) {
    // ============================================
    // VOICE BAR EMISSION (voiced stop closures) — Ex path
    // ============================================
    if (t.voicedClosure && hadPrevFrame) {
      double vbFadeMs = t.fadeMs;
      if (vbFadeMs < 8.0) vbFadeMs = 8.0;

      if (trajectoryState->hasPrevBase) {
        double vb[kFrameFieldCount];
        std::memcpy(vb, trajectoryState->prevBase, sizeof(vb));

        double vbAmp = (t.def && t.def->hasVoiceBarAmplitude) ? t.def->voiceBarAmplitude : 0.3;
        double vbF1 = (t.def && t.def->hasVoiceBarF1) ? t.def->voiceBarF1 : 150.0;

        vb[va] = vbAmp;
        vb[fa] = 0.0;
        vb[static_cast<int>(FieldId::aspirationAmplitude)] = 0.0;
        vb[static_cast<int>(FieldId::cf1)] = vbF1;
        vb[static_cast<int>(FieldId::pf1)] = vbF1;
        vb[static_cast<int>(FieldId::preFormantGain)] = vbAmp;

        // Pre-position F2/F3 at the stop's own formants (see emitFrames path).
        if (t.def) {
          auto vbField = [&](FieldId id) -> double {
            int idx = static_cast<int>(id);
            return (t.def->setMask & (1ULL << idx)) ? t.def->field[idx] : 0.0;
          };
          double sCf2 = vbField(FieldId::cf2);
          double sCf3 = vbField(FieldId::cf3);
          double sPf2 = vbField(FieldId::pf2);
          double sPf3 = vbField(FieldId::pf3);
          if (sCf2 > 0.0) vb[static_cast<int>(FieldId::cf2)] = sCf2;
          if (sCf3 > 0.0) vb[static_cast<int>(FieldId::cf3)] = sCf3;
          if (sPf2 > 0.0) vb[static_cast<int>(FieldId::pf2)] = sPf2;
          if (sPf3 > 0.0) vb[static_cast<int>(FieldId::pf3)] = sPf3;
        }

        nvspFrontend_Frame vbFrame;
        std::memcpy(&vbFrame, vb, sizeof(vbFrame));

        nvspFrontend_FrameEx vbFrameEx = trajectoryState->hasPrevFrameEx
            ? trajectoryState->prevFrameEx
            : frameExDefaults;
        vbFrameEx.transAmplitudeMode = 1.0;  // equal-power crossfade
        // Keep Fujisaki model running (don't reset IIR state) but don't re-fire commands.
        vbFrameEx.fujisakiPhraseAmp = 0.0;
        vbFrameEx.fujisakiAccentAmp = 0.0;
        vbFrameEx.fujisakiReset = 0.0;
        cb(userData, &vbFrame, &vbFrameEx, t.durationMs, vbFadeMs, userIndexBase);
      } else {
        cb(userData, nullptr, nullptr, t.durationMs, vbFadeMs, userIndexBase);
      }
      continue;
    }

    // ============================================
    // CODA NOISE TAPER (fricative→stop closure) — Ex path
    // ============================================
    if (t.preStopGap && t.codaFricStopBlend && !t.voicedClosure && hadPrevFrame) {
      if (trajectoryState->hasPrevBase) {
        const double totalDur = t.durationMs;
        const double earlyDur = totalDur * 0.45;
        const double lateDur = totalDur - earlyDur;
        const double prevFric = trajectoryState->prevBase[fa];
        const int aspIdx = static_cast<int>(FieldId::aspirationAmplitude);
        const int pgIdx = static_cast<int>(FieldId::preFormantGain);

        nvspFrontend_FrameEx taperFrameEx = trajectoryState->hasPrevFrameEx
            ? trajectoryState->prevFrameEx
            : frameExDefaults;
        taperFrameEx.fujisakiPhraseAmp = 0.0;
        taperFrameEx.fujisakiAccentAmp = 0.0;

        // --- Early taper: sibilant tail ---
        double early[kFrameFieldCount];
        std::memcpy(early, trajectoryState->prevBase, sizeof(early));
        early[va] = 0.0;
        early[fa] = prevFric * lang.codaNoiseTaperEarlyFricScale;
        early[aspIdx] = lang.codaNoiseTaperEarlyAspAmp;
        early[pgIdx] = lang.codaNoiseTaperPreGain;

        nvspFrontend_Frame earlyFrame;
        std::memcpy(&earlyFrame, early, sizeof(earlyFrame));
        cb(userData, &earlyFrame, &taperFrameEx, earlyDur, t.fadeMs, userIndexBase);
        hadPrevFrame = true;

        // --- Late taper: aspirated transition ---
        double late[kFrameFieldCount];
        std::memcpy(late, trajectoryState->prevBase, sizeof(late));
        late[va] = 0.0;
        late[fa] = prevFric * lang.codaNoiseTaperLateFricScale;
        late[aspIdx] = lang.codaNoiseTaperLateAspAmp;
        late[pgIdx] = lang.codaNoiseTaperPreGain;

        // Blend formants toward the stop's place (same as emitFrames path)
        if (t.def) {
          const int cf2i = static_cast<int>(FieldId::cf2);
          const int cf3i = static_cast<int>(FieldId::cf3);
          const int pf2i = static_cast<int>(FieldId::pf2);
          const int pf3i = static_cast<int>(FieldId::pf3);
          constexpr double blend = 0.40;
          if (t.def->setMask & (1ull << cf2i))
            late[cf2i] += (t.def->field[cf2i] - late[cf2i]) * blend;
          if (t.def->setMask & (1ull << cf3i))
            late[cf3i] += (t.def->field[cf3i] - late[cf3i]) * blend;
          if (t.def->setMask & (1ull << pf2i))
            late[pf2i] += (t.def->field[pf2i] - late[pf2i]) * blend;
          if (t.def->setMask & (1ull << pf3i))
            late[pf3i] += (t.def->field[pf3i] - late[pf3i]) * blend;
        }

        nvspFrontend_Frame lateFrame;
        std::memcpy(&lateFrame, late, sizeof(lateFrame));
        double lateFade = std::max(earlyDur * 0.5, lateDur * 0.4);
        cb(userData, &lateFrame, &taperFrameEx, lateDur, lateFade, userIndexBase);

        continue;
      }
    }

    if (t.silence || !t.def) {
      cb(userData, nullptr, nullptr, t.durationMs, t.fadeMs, userIndexBase);
      continue;
    }

    // Build base frame (same as emitFrames)
    double base[kFrameFieldCount] = {};
    const std::uint64_t mask = t.setMask;
    for (int f = 0; f < kFrameFieldCount; ++f) {
      if ((mask & (1ull << f)) == 0) continue;
      base[f] = t.field[f];
    }

    // Save full base for voice bar emission on the next voiced closure.
    std::memcpy(trajectoryState->prevBase, base, sizeof(base));
    trajectoryState->hasPrevBase = true;

    // Rate-adaptive bandwidth widening (same logic as emitFrames path).
    {
      const double hrT = pack.lang.highRateThreshold;
      const double bwF = pack.lang.highRateBandwidthWideningFactor;
      if (hrT > 0.0 && bwF > 1.0 && speed > hrT) {
        const double ceiling = hrT * 1.8;
        const double ramp = std::min((speed - hrT) / (ceiling - hrT), 1.0);
        const double bwScale = 1.0 + ramp * (bwF - 1.0);
        base[static_cast<int>(FieldId::cb1)] *= bwScale;
        base[static_cast<int>(FieldId::cb2)] *= bwScale;
        base[static_cast<int>(FieldId::cb3)] *= bwScale;
      }
    }

    // Build FrameEx by mixing user defaults with per-phoneme values.
    // The mixing formula:
    //   - creakiness, breathiness, jitter, shimmer: additive, clamped to [0,1]
    //   - sharpness: multiplicative (phoneme * user), clamped to reasonable range
    //   - endCf1/2/3, endPf1/2/3: direct values (Hz), NAN if not set
    // Per-phoneme values override only if explicitly set (has* flags).
    nvspFrontend_FrameEx frameEx;
    
    // Get per-phoneme values (0 / 1.0 neutral if not set)
    double phonemeCreakiness = (t.def && t.def->hasCreakiness) ? t.def->creakiness : 0.0;
    double phonemeBreathiness = (t.def && t.def->hasBreathiness) ? t.def->breathiness : 0.0;
    double phonemeJitter = (t.def && t.def->hasJitter) ? t.def->jitter : 0.0;
    double phonemeShimmer = (t.def && t.def->hasShimmer) ? t.def->shimmer : 0.0;
    double phonemeSharpness = (t.def && t.def->hasSharpness) ? t.def->sharpness : 1.0;
    
    // Phoneme can only BOOST sharpness, never dull it - this ensures the user's
    // configured sharpness is never reduced by per-phoneme values. A phoneme
    // wanting "less sharp" would actually make it less distinct from neighbors.
    if (phonemeSharpness < 1.0) phonemeSharpness = 1.0;
    
    frameEx.creakiness = clamp01(phonemeCreakiness + frameExDefaults.creakiness);
    double tokenBreathiness = t.hasTokenBreathiness ? t.tokenBreathiness : 0.0;
    frameEx.breathiness = clamp01(phonemeBreathiness + tokenBreathiness + frameExDefaults.breathiness);
    frameEx.jitter = clamp01(phonemeJitter + frameExDefaults.jitter);
    frameEx.shimmer = clamp01(phonemeShimmer + frameExDefaults.shimmer);

    double userSharpness = (frameExDefaults.sharpness > 0.0) ? frameExDefaults.sharpness : 1.0;
    frameEx.sharpness = clampSharpness(phonemeSharpness * userSharpness);    
    // Formant end targets: token-level (from coarticulation) takes priority,
    // then phoneme-level, otherwise NAN (no ramping).
    // This enables DECTalk-style within-frame formant ramping for CV transitions.
    frameEx.endCf1 = t.hasEndCf1 ? t.endCf1 : 
                     (t.def && t.def->hasEndCf1) ? t.def->endCf1 : NAN;
    frameEx.endCf2 = t.hasEndCf2 ? t.endCf2 : 
                     (t.def && t.def->hasEndCf2) ? t.def->endCf2 : NAN;
    frameEx.endCf3 = t.hasEndCf3 ? t.endCf3 : 
                     (t.def && t.def->hasEndCf3) ? t.def->endCf3 : NAN;
    frameEx.endPf1 = t.hasEndPf1 ? t.endPf1 :   // Token-level parallel (diphthong/nasal)
                     t.hasEndCf1 ? t.endCf1 :    // Fall back to cascade (coarticulation)
                     (t.def && t.def->hasEndPf1) ? t.def->endPf1 : NAN;
    frameEx.endPf2 = t.hasEndPf2 ? t.endPf2 :
                     t.hasEndCf2 ? t.endCf2 :
                     (t.def && t.def->hasEndPf2) ? t.def->endPf2 : NAN;
    frameEx.endPf3 = t.hasEndPf3 ? t.endPf3 :
                     t.hasEndCf3 ? t.endCf3 :
                     (t.def && t.def->hasEndPf3) ? t.def->endPf3 : NAN;

    // Per-parameter transition speed scales (set by boundary_smoothing pass).
    frameEx.transF1Scale = t.transF1Scale;
    frameEx.transF2Scale = t.transF2Scale;
    frameEx.transF3Scale = t.transF3Scale;
    frameEx.transNasalScale = t.transNasalScale;

    // Detect source transitions for equal-power amplitude crossfade.
    // When voicing source type changes (voiced→voiceless or vice versa),
    // linear crossfade creates an energy dip. Equal-power fixes this.
    // We check the FRAME values (not token flags) because that's what
    // the DSP actually interpolates between.
    {
      double curVA = base[va];   // voiceAmplitude of this frame
      double curFA = base[fa];   // fricationAmplitude of this frame
      bool curVoiced   = (curVA > 0.05);
      bool curFricated = (curFA > 0.05);
      if (trajectoryState->hasPrevFrame) {
        bool prevVoiced   = (trajectoryState->prevVoiceAmp > 0.05);
        bool prevFricated = (trajectoryState->prevFricAmp > 0.05);
        bool sourceChange = (prevVoiced != curVoiced) ||
                            (prevFricated != curFricated);
        frameEx.transAmplitudeMode = sourceChange ? 1.0 : 0.0;
      } else {
        frameEx.transAmplitudeMode = 0.0;
      }
    }

    // Fujisaki pitch model parameters (set by applyPitchFujisaki)
    // These pass phrase/accent commands to the DSP for natural prosody contours.
    frameEx.fujisakiEnabled = t.fujisakiEnabled ? 1.0 : 0.0;
    frameEx.fujisakiReset = t.fujisakiReset ? 1.0 : 0.0;
    frameEx.fujisakiPhraseAmp = t.fujisakiPhraseAmp;
    frameEx.fujisakiPhraseLen = lang.fujisakiPhraseLen;  // 0 = DSP default
    frameEx.fujisakiAccentAmp = t.fujisakiAccentAmp;
    // Per-token accent duration: scale by monosyllable shortening factor.
    frameEx.fujisakiAccentDur = lang.fujisakiAccentDur * t.fujisakiAccentDurScale;
    frameEx.fujisakiAccentLen = lang.fujisakiAccentLen;  // 0 = DSP default

    // Save frameEx for voice bar emission (keeps Fujisaki model alive during closures).
    trajectoryState->prevFrameEx = frameEx;
    trajectoryState->hasPrevFrameEx = true;

    // ============================================
    // DIPHTHONG GLIDE MICRO-FRAME EMISSION
    // ============================================
    // Collapsed diphthongs emit N cosine-smoothed micro-frames that sweep
    // formants from onset targets (token's base field[]) to offset targets
    // (endCf1/2/3).  Each micro-frame's endCf/endPf points to the NEXT
    // frame's formant values, so the DSP's per-sample exponential smoothing
    // glides between nearby waypoints.  This avoids:
    //   - phasing (audio crossfade between distant formant configs)
    //   - bandwidth smearing (sweeping across full range kills resonator Q)
    // The last micro-frame gets endCf=NAN (hold at offset target).
    if (t.isDiphthongGlide && t.durationMs > 0.0) {
      const double totalDur = t.durationMs;

      // Number of micro-frames scales with duration.
      // Minimum 5: with N=3, onset hold concentrates waypoints near the
      // onset, leaving only one big jump to the offset — losing the glide.
      // N=5 guarantees enough interior points for a smooth sweep curve.
      // At higher pitch, fewer harmonics per formant bandwidth makes
      // crossfade phasing more audible — tighten the interval.
      double intervalMs = lang.diphthongMicroFrameIntervalMs;
      const double pitch0 = base[vp];
      const double pitchEnd = base[evp];
      // Use the LOWEST pitch across the diphthong for interval floor.
      // Sentence-final declination can drop pitch from 109→76 Hz;
      // onset pitch alone would allow frames too short for the
      // resonator to settle at the offglide's lower pitch.
      double pitchForFloor = pitch0;
      if (pitchEnd > 0.0 && pitchEnd < pitchForFloor)
        pitchForFloor = pitchEnd;
      if (pitchForFloor > 0.0) {
        // Scale interval proportionally to pitch period (see non-Ex path).
        intervalMs *= (100.0 / pitchForFloor);
        // Floor: at least 2 pitch periods per micro-frame for resonator settling.
        double minInterval = 2000.0 / pitchForFloor;
        if (intervalMs < minInterval) intervalMs = minInterval;
        if (intervalMs < 3.0) intervalMs = 3.0;
      }
      // Start and end formant targets (Hz).
      const double startCf1 = base[static_cast<int>(FieldId::cf1)];
      const double startCf2 = base[static_cast<int>(FieldId::cf2)];
      const double startCf3 = base[static_cast<int>(FieldId::cf3)];
      const double startPf1 = base[static_cast<int>(FieldId::pf1)];
      const double startPf2 = base[static_cast<int>(FieldId::pf2)];
      const double startPf3 = base[static_cast<int>(FieldId::pf3)];

      // Start bandwidths (from onset vowel's base frame).
      const double startCb1 = base[static_cast<int>(FieldId::cb1)];
      const double startCb2 = base[static_cast<int>(FieldId::cb2)];
      const double startCb3 = base[static_cast<int>(FieldId::cb3)];
      const double startPb1 = base[static_cast<int>(FieldId::pb1)];
      const double startPb2 = base[static_cast<int>(FieldId::pb2)];
      const double startPb3 = base[static_cast<int>(FieldId::pb3)];

      const double endCascF1 = nvsp_isnan(frameEx.endCf1) ? startCf1 : frameEx.endCf1;
      const double endCascF2 = nvsp_isnan(frameEx.endCf2) ? startCf2 : frameEx.endCf2;
      const double endCascF3 = nvsp_isnan(frameEx.endCf3) ? startCf3 : frameEx.endCf3;

      const double endParF1 = (t.hasEndPf1 && !nvsp_isnan(frameEx.endPf1))
                              ? frameEx.endPf1 : endCascF1;
      const double endParF2 = (t.hasEndPf2 && !nvsp_isnan(frameEx.endPf2))
                              ? frameEx.endPf2 : endCascF2;
      const double endParF3 = (t.hasEndPf3 && !nvsp_isnan(frameEx.endPf3))
                              ? frameEx.endPf3 : endCascF3;

      // End bandwidths: use offset vowel's values if set, else hold onset.
      const double endCascB1 = t.hasEndCb1 ? t.endCb1 : startCb1;
      const double endCascB2 = t.hasEndCb2 ? t.endCb2 : startCb2;
      const double endCascB3 = t.hasEndCb3 ? t.endCb3 : startCb3;
      const double endParB1 = t.hasEndPb1 ? t.endPb1 : startPb1;
      const double endParB2 = t.hasEndPb2 ? t.endPb2 : startPb2;
      const double endParB3 = t.hasEndPb3 ? t.endPb3 : startPb3;

      // Number of micro-frames scales with duration so each step stays
      // a consistent length regardless of speech rate.  At fast rates
      // N stays at minimum 5 (the staircase sweet spot for shimmer).
      // At slow rates N scales up — more steps with smaller formant
      // jumps.  This mirrors MacInTalk's fixed 5ms frame rate: slow
      // speech just gets more frames, not bigger jumps.
      static constexpr int kMaxFrames = 64;
      int N = std::clamp(static_cast<int>(std::round(totalDur / intervalMs)), 5, kMaxFrames);

      const double startPitch = base[vp];
      const double endPitch = base[evp];
      const double pitchDelta = endPitch - startPitch;
      const double dipFactor = lang.diphthongAmplitudeDipFactor;
      const double baseSegDur = totalDur / N;

      // Adaptive onset hold: scale exponent based on sweep width and
      // following context.  Wide diphthongs (PRICE) get reduced hold;
      // narrow ones (FACE) keep the full base exponent.
      const Token* nextTok = (&t < &tokens.back()) ? (&t + 1) : nullptr;
      const double onsetHold = adaptiveOnsetHold(
          lang.diphthongOnsetHoldExponent,
          startCf1, endCascF1, startCf2, endCascF2,
          totalDur, nextTok);

      FELOG("DIPH-EX speed=%.2f dur=%.1fms N=%d intv=%.1fms hold=%.2f "
            "cf1=%.0f->%.0f cf2=%.0f->%.0f cf3=%.0f->%.0f "
            "pitch=%.1f->%.1f fade=%.1fms\n",
            speed, totalDur, N, intervalMs, onsetHold,
            startCf1, endCascF1, startCf2, endCascF2,
            startCf3, endCascF3,
            startPitch, endPitch, t.fadeMs);

      // Onset settle: give the first micro-frame extra time so the
      // resonator can settle from the preceding consonant before the
      // formant sweep begins.  Borrowed from remaining segments.
      double seg0Dur = baseSegDur;
      double otherSegDur = baseSegDur;
      const double settleMs = lang.diphthongOnsetSettleMs;
      if (settleMs > 0.0 && N > 1) {
        seg0Dur = std::min(baseSegDur + settleMs, totalDur * 0.5);
        otherSegDur = (totalDur - seg0Dur) / (N - 1);
      }

      // Pre-compute all N waypoints so each frame can reference the next.
      // 6 formants per waypoint: cf1,cf2,cf3,pf1,pf2,pf3
      double wpCf1[kMaxFrames], wpCf2[kMaxFrames], wpCf3[kMaxFrames];
      double wpPf1[kMaxFrames], wpPf2[kMaxFrames], wpPf3[kMaxFrames];
      double wpCb1[kMaxFrames], wpCb2[kMaxFrames], wpCb3[kMaxFrames];
      double wpPb1[kMaxFrames], wpPb2[kMaxFrames], wpPb3[kMaxFrames];

      for (int seg = 0; seg < N; ++seg) {
        double frac = (N > 1) ? (static_cast<double>(seg) / (N - 1)) : 0.0;
        if (onsetHold > 1.0) frac = pow(frac, onsetHold);
        double s = cosineSmooth(frac);

        wpCf1[seg] = calculateFreqAtFadePosition(startCf1, endCascF1, s);
        wpCf2[seg] = calculateFreqAtFadePosition(startCf2, endCascF2, s);
        wpCf3[seg] = calculateFreqAtFadePosition(startCf3, endCascF3, s);
        wpPf1[seg] = calculateFreqAtFadePosition(startPf1, endParF1, s);
        wpPf2[seg] = calculateFreqAtFadePosition(startPf2, endParF2, s);
        wpPf3[seg] = calculateFreqAtFadePosition(startPf3, endParF3, s);
        wpCb1[seg] = calculateFreqAtFadePosition(startCb1, endCascB1, s);
        wpCb2[seg] = calculateFreqAtFadePosition(startCb2, endCascB2, s);
        wpCb3[seg] = calculateFreqAtFadePosition(startCb3, endCascB3, s);
        wpPb1[seg] = calculateFreqAtFadePosition(startPb1, endParB1, s);
        wpPb2[seg] = calculateFreqAtFadePosition(startPb2, endParB2, s);
        wpPb3[seg] = calculateFreqAtFadePosition(startPb3, endParB3, s);

        FELOG("  wp[%d] frac=%.3f s=%.3f cf1=%.0f cf2=%.0f cf3=%.0f\n",
              seg, (N > 1) ? (static_cast<double>(seg) / (N - 1)) : 0.0,
              s, wpCf1[seg], wpCf2[seg], wpCf3[seg]);
      }

      // Emit micro-frames with endCf pointing to the next waypoint.
      for (int seg = 0; seg < N; ++seg) {
        double mf[kFrameFieldCount];
        std::memcpy(mf, base, sizeof(mf));

        mf[static_cast<int>(FieldId::cf1)] = wpCf1[seg];
        mf[static_cast<int>(FieldId::cf2)] = wpCf2[seg];
        mf[static_cast<int>(FieldId::cf3)] = wpCf3[seg];
        mf[static_cast<int>(FieldId::pf1)] = wpPf1[seg];
        mf[static_cast<int>(FieldId::pf2)] = wpPf2[seg];
        mf[static_cast<int>(FieldId::pf3)] = wpPf3[seg];
        mf[static_cast<int>(FieldId::cb1)] = wpCb1[seg];
        mf[static_cast<int>(FieldId::cb2)] = wpCb2[seg];
        mf[static_cast<int>(FieldId::cb3)] = wpCb3[seg];
        mf[static_cast<int>(FieldId::pb1)] = wpPb1[seg];
        mf[static_cast<int>(FieldId::pb2)] = wpPb2[seg];
        mf[static_cast<int>(FieldId::pb3)] = wpPb3[seg];

        // Pitch: linear ramp sliced proportionally.
        double t0 = (N > 1) ? (static_cast<double>(seg) / N) : 0.0;
        double t1 = (N > 1) ? (static_cast<double>(seg + 1) / N) : 1.0;
        mf[vp]  = startPitch + pitchDelta * t0;
        mf[evp] = startPitch + pitchDelta * t1;

        // Slight amplitude dip at midpoint (articulatory gesture)
        double frac = (N > 1) ? (static_cast<double>(seg) / (N - 1)) : 0.0;
        if (dipFactor > 0.0) {
          double ampScale = 1.0 - dipFactor * sin(M_PI * frac);
          mf[va] *= ampScale;
        }

        nvspFrontend_Frame frame;
        std::memcpy(&frame, mf, sizeof(frame));

        // MacinTalk staircase: ALL micro-frames hold at their waypoint
        // (endCf=NAN disables per-sample exponential smoothing).
        // Formants snap during the brief crossfade then stay put,
        // minimizing time spent near harmonic-formant crossings.
        nvspFrontend_FrameEx mfEx = frameEx;
        mfEx.endCf1 = NAN;
        mfEx.endCf2 = NAN;
        mfEx.endCf3 = NAN;
        mfEx.endPf1 = NAN;
        mfEx.endPf2 = NAN;
        mfEx.endPf3 = NAN;

        // Fade: first micro-frame uses token's entry fade.
        // Internal micro-frames use a short snap fade (3ms) so formants
        // jump quickly to the new waypoint then hold — staircase style.
        //
        // After obstruents, onset shimmer is addressed by bandwidth widening
        // below (not by fade capping, which caused clicks).
        const double thisDur = (seg == 0) ? seg0Dur : otherSegDur;
        double fadeIn = (seg == 0) ? t.fadeMs : 3.0;
        if (fadeIn > thisDur) fadeIn = thisDur;

        // After obstruents, apply decaying bandwidth widening across the
        // first few micro-frames, scaled by the diphthong's F2 sweep width.
        // Wide sweeps (MOUTH /aʊ/, 550-750 Hz) need more help settling;
        // narrow sweeps (FACE /eɪ/, ~200 Hz) need little or none.
        // MacInTalk widens BW1 by 250 Hz during burst release; we taper
        // across seg0→seg1→seg2 so bandwidths return gradually (no pop).
        const bool afterObstruent = (trajectoryState->hasPrevFrame &&
            trajectoryState->prevVoiceAmp < 0.05);
        if (afterObstruent && seg < 3) {
          const double sweepF2 = fabs(endCascF2 - startCf2);
          // Scale: 0 below 400 Hz, full at 700+ Hz.
          const double sweepScale = std::clamp(
              (sweepF2 - 400.0) / 300.0, 0.0, 1.0);
          if (sweepScale > 0.0) {
            const double decay = 1.0 / (1 << seg);  // 1.0, 0.5, 0.25
            const double s = sweepScale * decay;
            mf[static_cast<int>(FieldId::cb1)] += 200.0 * s;
            mf[static_cast<int>(FieldId::cb2)] += 60.0 * s;
            mf[static_cast<int>(FieldId::cb3)] += 40.0 * s;
            std::memcpy(&frame, mf, sizeof(frame));
          }
        }

        // Don't re-fire Fujisaki commands on internal micro-frames.
        if (seg > 0) {
          mfEx.fujisakiPhraseAmp = 0.0;
          mfEx.fujisakiAccentAmp = 0.0;
          mfEx.fujisakiReset = 0.0;
        }

        FELOG("  emit[%d] dur=%.1fms fade=%.1fms cf1=%.0f cf2=%.0f endCf1=%.0f endCf2=%.0f\n",
              seg, thisDur, fadeIn,
              mf[static_cast<int>(FieldId::cf1)],
              mf[static_cast<int>(FieldId::cf2)],
              nvsp_isnan(mfEx.endCf1) ? -1.0 : mfEx.endCf1,
              nvsp_isnan(mfEx.endCf2) ? -1.0 : mfEx.endCf2);

        cb(userData, &frame, &mfEx, thisDur, fadeIn, userIndexBase);
        hadPrevFrame = true;
      }

      // Update trajectory state with the final micro-frame's values
      // so the next token's trajectory limiting sees the offset formants.
      trajectoryState->prevCf2 = wpCf2[N - 1];
      trajectoryState->prevCf3 = wpCf3[N - 1];
      trajectoryState->prevPf2 = wpPf2[N - 1];
      trajectoryState->prevPf3 = wpPf3[N - 1];
      trajectoryState->prevVoiceAmp = base[va];
      trajectoryState->prevFricAmp = base[fa];
      trajectoryState->hasPrevFrame = true;
      trajectoryState->prevWasNasal = false;

      prevTokenWasTap = false;
      prevTokenWasStop = false;
      continue;
    }

    // Handle trill modulation (simplified version - emits micro-frames)
    if (trillEnabled && tokenIsTrill(t) && t.durationMs > 0.0) {
      double totalDur = t.durationMs;
      constexpr double kFixedTrillCycleMs = 28.0;
      double cycleMs = kFixedTrillCycleMs;
      if (cycleMs > totalDur) cycleMs = totalDur;

      double closeMs = cycleMs * kTrillCloseFrac;
      double openMs = cycleMs - closeMs;

      if (openMs < kMinPhaseMs) {
        openMs = kMinPhaseMs;
        closeMs = std::max(kMinPhaseMs, cycleMs - openMs);
      }
      if (closeMs < kMinPhaseMs) {
        closeMs = kMinPhaseMs;
        openMs = std::max(kMinPhaseMs, cycleMs - closeMs);
      }

      double microFadeMs = pack.lang.trillModulationFadeMs;
      if (microFadeMs <= 0.0) {
        microFadeMs = std::min(2.0, cycleMs * 0.12);
      }

      const bool hasVoiceAmp = ((mask & (1ull << va)) != 0);
      const bool hasFricAmp = ((mask & (1ull << fa)) != 0);
      const double baseVoiceAmp = base[va];
      const double baseFricAmp = base[fa];

      const double startPitch = base[vp];
      const double endPitch = base[evp];
      const double pitchDelta = endPitch - startPitch;

      double remaining = totalDur;
      double pos = 0.0;
      bool highPhase = true;
      bool firstPhase = true;

      while (remaining > 1e-9) {
        double phaseDur = highPhase ? openMs : closeMs;
        if (phaseDur > remaining) phaseDur = remaining;

        double t0 = (totalDur > 0.0) ? (pos / totalDur) : 0.0;
        double t1 = (totalDur > 0.0) ? ((pos + phaseDur) / totalDur) : 1.0;

        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));

        seg[vp] = startPitch + pitchDelta * t0;
        seg[evp] = startPitch + pitchDelta * t1;

        if (!highPhase) {
          if (hasVoiceAmp) {
            seg[va] = baseVoiceAmp * kTrillCloseFactor;
          }
          if (hasFricAmp && baseFricAmp > 0.0) {
            seg[fa] = std::max(baseFricAmp, kTrillFricFloor);
          }
        }

        nvspFrontend_Frame frame;
        std::memcpy(&frame, seg, sizeof(frame));

        double fadeIn = firstPhase ? t.fadeMs : microFadeMs;
        if (fadeIn <= 0.0) fadeIn = microFadeMs;
        if (fadeIn > phaseDur) fadeIn = phaseDur;

        cb(userData, &frame, &frameEx, phaseDur, fadeIn, userIndexBase);
        hadPrevFrame = true;

        remaining -= phaseDur;
        pos += phaseDur;
        highPhase = !highPhase;
        firstPhase = false;
      }

      prevTokenWasStop = false;
      continue;
    }

    // ============================================
    // STOP BURST MICRO-FRAME EMISSION (Ex path)
    // ============================================
    {
      const bool isStop = t.def && ((t.def->flags & kIsStop) != 0);
      const bool isAffricate = t.def && ((t.def->flags & kIsAfricate) != 0);

      if ((isStop || isAffricate) && !t.silence && !t.preStopGap &&
          !t.postStopAspiration && !t.voicedClosure && t.durationMs > 1.0) {

        // Word-boundary silence gap before word-initial stops.
        // Stops at high rate can be as short as 4ms — can't steal duration.
        // Instead, INSERT an extra near-silent micro-frame (additive).
        // This gives the ear a boundary cue without shrinking the burst.
        double stopMainDur = t.durationMs;
        double stopMainFade = t.fadeMs;
        if (wbDipMs > 0.0 && t.wordStart && hadPrevFrame) {
          double dip[kFrameFieldCount];
          std::memcpy(dip, base, sizeof(dip));
          dip[va] = 0.0;
          dip[fa] = 0.0;
          dip[static_cast<int>(FieldId::aspirationAmplitude)] = 0.0;
          dip[static_cast<int>(FieldId::preFormantGain)] = 0.0;

          nvspFrontend_Frame dipFrame;
          std::memcpy(&dipFrame, dip, sizeof(dipFrame));
          cb(userData, &dipFrame, &frameEx, wbDipMs, 1.0, userIndexBase);
          hadPrevFrame = true;
          stopMainFade = 1.0; // short crossfade into burst

          FELOG("WB-GAP-STOP wordStart=%d prevWasStop=%d gapMs=%.1f dur=%.1f\n",
                (int)t.wordStart, (int)prevTokenWasStop, wbDipMs, stopMainDur);
        } else if (isStop || isAffricate) {
          FELOG("NO-WB-GAP-STOP wordStart=%d hadPrev=%d wbDipMs=%.1f dur=%.1f\n",
                (int)t.wordStart, (int)hadPrevFrame, wbDipMs, stopMainDur);
        }

        Place place = getPlace(t.def->key);

        double burstMs = 7.0;
        double decayRate = 0.5;
        double spectralTilt = 0.0;

        switch (place) {
          case Place::Labial:   burstMs = 5.0;  decayRate = 0.6;  spectralTilt = 0.1;   break;
          case Place::Alveolar: burstMs = 7.0;  decayRate = 0.5;  spectralTilt = 0.0;   break;
          case Place::Velar:    burstMs = 11.0; decayRate = 0.4;  spectralTilt = -0.15; break;
          case Place::Palatal:  burstMs = 9.0;  decayRate = 0.45; spectralTilt = -0.1;  break;
          default: break;
        }

        if (t.def->hasBurstDurationMs) burstMs = t.def->burstDurationMs;
        if (t.def->hasBurstDecayRate) decayRate = t.def->burstDecayRate;
        if (t.def->hasBurstSpectralTilt) spectralTilt = t.def->burstSpectralTilt;

        // Coda blend: longer burst, faster decay (same as emitFrames path)
        if (t.codaFricStopBlend && !isAffricate) {
          burstMs = std::max(burstMs, stopMainDur * 0.6);
          decayRate = 0.7;
        }

        // Clamp burst to 75% of token duration so it always fires
        double maxBurst = stopMainDur * 0.75;
        if (burstMs > maxBurst) burstMs = maxBurst;

        {
          // Pitch interpolation across 2 micro-frames
          const double startPitch = base[vp];
          const double pitchDelta = base[evp] - startPitch;
          const double totalDur = stopMainDur;
          const double burstFrac = burstMs / totalDur;

          double seg1[kFrameFieldCount];
          std::memcpy(seg1, base, sizeof(seg1));
          seg1[vp] = startPitch;
          seg1[evp] = startPitch + pitchDelta * burstFrac;

          const int pa3i = static_cast<int>(FieldId::pa3);
          const int pa4i = static_cast<int>(FieldId::pa4);
          const int pa5i = static_cast<int>(FieldId::pa5);
          const int pa6i = static_cast<int>(FieldId::pa6);

          if (spectralTilt < 0.0) {
            seg1[pa5i] = std::min(1.0, seg1[pa5i] * (1.0 - spectralTilt));
            seg1[pa6i] = std::min(1.0, seg1[pa6i] * (1.0 - spectralTilt * 0.7));
          } else if (spectralTilt > 0.0) {
            seg1[pa3i] = std::min(1.0, seg1[pa3i] * (1.0 + spectralTilt));
            seg1[pa4i] = std::min(1.0, seg1[pa4i] * (1.0 + spectralTilt * 0.7));
          }

          nvspFrontend_Frame burstFrame;
          std::memcpy(&burstFrame, seg1, sizeof(burstFrame));
          cb(userData, &burstFrame, &frameEx, burstMs, stopMainFade, userIndexBase);
          hadPrevFrame = true;

          double seg2[kFrameFieldCount];
          std::memcpy(seg2, base, sizeof(seg2));
          seg2[vp] = startPitch + pitchDelta * burstFrac;
          seg2[evp] = startPitch + pitchDelta;

          const int faIdx = static_cast<int>(FieldId::fricationAmplitude);
          if (!isAffricate) {
            seg2[faIdx] *= (1.0 - decayRate);
          }

          nvspFrontend_Frame decayFrame;
          std::memcpy(&decayFrame, seg2, sizeof(decayFrame));
          double decayDur = stopMainDur - burstMs;
          double decayFade = std::min(burstMs * 0.5, decayDur);
          cb(userData, &decayFrame, &frameEx, decayDur, decayFade, userIndexBase);

          trajectoryState->prevCf2 = burstFrame.cf2;
          trajectoryState->prevCf3 = burstFrame.cf3;
          trajectoryState->prevPf2 = burstFrame.pf2;
          trajectoryState->prevPf3 = burstFrame.pf3;
          trajectoryState->prevVoiceAmp = base[va];
          trajectoryState->prevFricAmp = base[fa];
          trajectoryState->hasPrevFrame = true;
          trajectoryState->prevWasNasal = false;

          prevTokenWasStop = true;
          continue;
        }
      }
    }

    // ============================================
    // FRICATIVE ATTACK/DECAY MICRO-FRAME EMISSION (Ex path)
    // ============================================
    {
      const bool isStop2 = t.def && ((t.def->flags & kIsStop) != 0);
      const bool isAffricate2 = t.def && ((t.def->flags & kIsAfricate) != 0);
      const double fricAmp = base[fa];

      if (!isStop2 && !isAffricate2 && !t.silence && !t.preStopGap &&
          !t.postStopAspiration && !t.voicedClosure && fricAmp > 0.0) {

        double attackMs = t.def->hasFricAttackMs ? t.def->fricAttackMs : 3.0;
        double decayMs = t.def->hasFricDecayMs ? t.def->fricDecayMs : 4.0;

        // Skip attack ramp in post-stop clusters (/ks/, /ts/, etc.)
        if (!prevTokenWasStop && attackMs + decayMs + 2.0 < t.durationMs) {
          const int faIdx = static_cast<int>(FieldId::fricationAmplitude);

          // Pitch interpolation across 3 micro-frames
          const double startPitch = base[vp];
          const double pitchDelta = base[evp] - startPitch;
          const double totalDur = t.durationMs;
          const double sustainDur = totalDur - attackMs - decayMs;
          const double attackFrac = attackMs / totalDur;
          const double sustainEndFrac = (attackMs + sustainDur) / totalDur;

          // --- Attack micro-frame: ramp from 10% to full ---
          double seg1[kFrameFieldCount];
          std::memcpy(seg1, base, sizeof(seg1));
          seg1[faIdx] = fricAmp * 0.1;
          seg1[vp] = startPitch;
          seg1[evp] = startPitch + pitchDelta * attackFrac;

          nvspFrontend_Frame attackFrame;
          std::memcpy(&attackFrame, seg1, sizeof(attackFrame));
          cb(userData, &attackFrame, &frameEx, attackMs, t.fadeMs, userIndexBase);
          hadPrevFrame = true;

          // --- Sustain micro-frame: full amplitude ---
          double seg2s[kFrameFieldCount];
          std::memcpy(seg2s, base, sizeof(seg2s));
          seg2s[vp] = startPitch + pitchDelta * attackFrac;
          seg2s[evp] = startPitch + pitchDelta * sustainEndFrac;

          nvspFrontend_Frame sustainFrame;
          std::memcpy(&sustainFrame, seg2s, sizeof(sustainFrame));
          cb(userData, &sustainFrame, &frameEx, sustainDur, attackMs, userIndexBase);

          // --- Decay micro-frame: ramp from full to 30% ---
          double seg3[kFrameFieldCount];
          std::memcpy(seg3, base, sizeof(seg3));
          seg3[faIdx] = fricAmp * 0.3;
          seg3[vp] = startPitch + pitchDelta * sustainEndFrac;
          seg3[evp] = startPitch + pitchDelta;

          nvspFrontend_Frame decayFrame;
          std::memcpy(&decayFrame, seg3, sizeof(decayFrame));
          cb(userData, &decayFrame, &frameEx, decayMs, decayMs * 0.5, userIndexBase);

          trajectoryState->prevCf2 = sustainFrame.cf2;
          trajectoryState->prevCf3 = sustainFrame.cf3;
          trajectoryState->prevPf2 = sustainFrame.pf2;
          trajectoryState->prevPf3 = sustainFrame.pf3;
          trajectoryState->prevVoiceAmp = base[va];
          trajectoryState->prevFricAmp = base[fa];
          trajectoryState->hasPrevFrame = true;
          trajectoryState->prevWasNasal = false;

          prevTokenWasStop = false;
          continue;
        }
      }
    }

    // ============================================
    // RELEASE SPREAD (postStopAspiration tokens) — Ex path
    // ============================================
    if (t.postStopAspiration && t.def && t.durationMs > 1.0) {
      if (t.codaFricStopBlend) {
        // Coda blend: single decaying aspiration frame, no ramp-in dip.
        const int aspIdx = static_cast<int>(FieldId::aspirationAmplitude);
        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));
        seg[aspIdx] *= 0.60;

        nvspFrontend_Frame aspFrame;
        std::memcpy(&aspFrame, seg, sizeof(aspFrame));
        cb(userData, &aspFrame, &frameEx, t.durationMs, t.durationMs * 0.5, userIndexBase);

        trajectoryState->prevCf2 = aspFrame.cf2;
        trajectoryState->prevCf3 = aspFrame.cf3;
        trajectoryState->prevPf2 = aspFrame.pf2;
        trajectoryState->prevPf3 = aspFrame.pf3;
        trajectoryState->prevVoiceAmp = base[va];
        trajectoryState->prevFricAmp = base[fa];
        trajectoryState->hasPrevFrame = true;
        trajectoryState->prevWasNasal = false;

        prevTokenWasStop = false;
        continue;
      }

      double spreadMs = t.def->hasReleaseSpreadMs ? t.def->releaseSpreadMs : 4.0;

      if (spreadMs > 0.0 && spreadMs < t.durationMs) {
        const int faIdx = static_cast<int>(FieldId::fricationAmplitude);
        const int aspIdx = static_cast<int>(FieldId::aspirationAmplitude);

        // Pitch interpolation across 2 micro-frames
        const double startPitch = base[vp];
        const double pitchDelta = base[evp] - startPitch;
        const double totalDur = t.durationMs;
        const double spreadFrac = (totalDur > 0.0) ? (spreadMs / totalDur) : 0.0;

        double seg1[kFrameFieldCount];
        std::memcpy(seg1, base, sizeof(seg1));
        seg1[faIdx] *= 0.15;
        seg1[aspIdx] *= 0.15;
        seg1[vp] = startPitch;
        seg1[evp] = startPitch + pitchDelta * spreadFrac;

        nvspFrontend_Frame rampFrame;
        std::memcpy(&rampFrame, seg1, sizeof(rampFrame));
        cb(userData, &rampFrame, &frameEx, spreadMs, t.fadeMs, userIndexBase);
        hadPrevFrame = true;

        double seg2[kFrameFieldCount];
        std::memcpy(seg2, base, sizeof(seg2));
        seg2[vp] = startPitch + pitchDelta * spreadFrac;
        seg2[evp] = startPitch + pitchDelta;

        nvspFrontend_Frame fullFrame;
        std::memcpy(&fullFrame, seg2, sizeof(fullFrame));
        double fullDur = t.durationMs - spreadMs;
        cb(userData, &fullFrame, &frameEx, fullDur, spreadMs * 0.5, userIndexBase);

        trajectoryState->prevCf2 = fullFrame.cf2;
        trajectoryState->prevCf3 = fullFrame.cf3;
        trajectoryState->prevPf2 = fullFrame.pf2;
        trajectoryState->prevPf3 = fullFrame.pf3;
        trajectoryState->prevVoiceAmp = base[va];
        trajectoryState->prevFricAmp = base[fa];
        trajectoryState->hasPrevFrame = true;
        trajectoryState->prevWasNasal = false;

        prevTokenWasStop = true;  // aspiration is stop-related
        continue;
      }
    }

    // ============================================
    // TAP MICRO-EVENT EMISSION (coarticulated amplitude notch)
    // =========================================================
    // A tap is almost entirely coarticulation — the tongue tip briefly flicks
    // the alveolar ridge without the jaw closing.  The primary cue is a brief
    // amplitude dip, but formants must also sweep smoothly from the preceding
    // vowel through the alveolar target and back, never dwelling at the tap's
    // static formant values.  Using static tap formants for all 3 phases
    // created an abrupt formant jump that sounded velar/clenched.
    //
    // New approach: onset starts at 70% previous vowel + 30% tap formants,
    // notch reaches 100% tap target (brief), recovery blends back out.
    // The DSP crossfade to the following token handles the exit naturally.
    const bool isTap = t.def && ((t.def->flags & kIsTap) != 0);
    // See non-Ex path comment: skip micro-event notch for very short taps.
    if (isTap && t.durationMs >= 8.0) {
      const double totalDur = t.durationMs;

      // Phase proportions: onset 30%, notch 40%, recovery 30%.
      // More onset/recovery time than before for smoother coarticulation.
      const double notchFloorMs = 1.5;
      double notchDur = std::max(totalDur * 0.40, notchFloorMs);
      if (notchDur > totalDur * 0.70) notchDur = totalDur * 0.70;
      const double remainDur = totalDur - notchDur;
      const double onsetDur = remainDur * 0.45;
      const double recovDur = remainDur - onsetDur;

      // Amplitude dip: reduce voiceAmplitude in the notch.
      // Gentler dip (60%) — the formant sweep now carries identity,
      // so the amplitude doesn't need to do all the work.
      const int vaIdx = static_cast<int>(FieldId::voiceAmplitude);
      const double origAmp = base[vaIdx];
      const double notchAmp = origAmp * 0.60;

      // Formant indices for coarticulation blending.
      const int cf1 = static_cast<int>(FieldId::cf1);
      const int cf2 = static_cast<int>(FieldId::cf2);
      const int cf3 = static_cast<int>(FieldId::cf3);

      // Pitch interpolation
      const double startPitch = base[vp];
      const double pitchDelta = base[evp] - startPitch;
      const double onsetFrac = (totalDur > 0.0) ? (onsetDur / totalDur) : 0.0;
      const double notchEndFrac = (totalDur > 0.0) ? ((onsetDur + notchDur) / totalDur) : 0.0;

      const double microFade = std::max(0.5, 1.5 / std::max(0.5, speed));

      // Phase 1: onset — formants blend FROM previous vowel TOWARD tap.
      // 70% previous + 30% tap = gentle approach, not an abrupt jump.
      {
        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));
        if (trajectoryState->hasPrevBase) {
          const double prevW = 0.70;
          const double tapW  = 0.30;
          seg[cf1] = trajectoryState->prevBase[cf1] * prevW + base[cf1] * tapW;
          seg[cf2] = trajectoryState->prevBase[cf2] * prevW + base[cf2] * tapW;
          seg[cf3] = trajectoryState->prevBase[cf3] * prevW + base[cf3] * tapW;
        }
        seg[vp] = startPitch;
        seg[evp] = startPitch + pitchDelta * onsetFrac;

        nvspFrontend_Frame f;
        std::memcpy(&f, seg, sizeof(f));
        cb(userData, &f, &frameEx, onsetDur, microFade, userIndexBase);
        hadPrevFrame = true;
      }

      // Phase 2: notch — amplitude dipped, tap formants at full target.
      // This is the brief moment of alveolar contact.
      {
        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));
        seg[vaIdx] = notchAmp;
        seg[vp] = startPitch + pitchDelta * onsetFrac;
        seg[evp] = startPitch + pitchDelta * notchEndFrac;

        nvspFrontend_Frame f;
        std::memcpy(&f, seg, sizeof(f));
        cb(userData, &f, &frameEx, notchDur, microFade, userIndexBase);
      }

      // Phase 3: recovery — formants blend AWAY from tap back toward
      // neutral.  Uses 50/50 blend; the DSP crossfade to the next token
      // completes the trajectory to the following vowel.
      {
        double seg[kFrameFieldCount];
        std::memcpy(seg, base, sizeof(seg));
        if (trajectoryState->hasPrevBase) {
          // Blend toward previous vowel (proxy for surrounding vowel space).
          const double prevW = 0.50;
          const double tapW  = 0.50;
          seg[cf1] = trajectoryState->prevBase[cf1] * prevW + base[cf1] * tapW;
          seg[cf2] = trajectoryState->prevBase[cf2] * prevW + base[cf2] * tapW;
          seg[cf3] = trajectoryState->prevBase[cf3] * prevW + base[cf3] * tapW;
        }
        seg[vp] = startPitch + pitchDelta * notchEndFrac;
        seg[evp] = startPitch + pitchDelta;

        nvspFrontend_Frame f;
        std::memcpy(&f, seg, sizeof(f));
        cb(userData, &f, &frameEx, recovDur, microFade, userIndexBase);
      }

      trajectoryState->prevVoiceAmp = base[va];
      trajectoryState->prevFricAmp = base[fa];
      trajectoryState->hasPrevFrame = true;
      trajectoryState->prevWasNasal = false;

      prevTokenWasTap = true;
      prevTokenWasStop = false;
      continue;
    }

    // ============================================
    // WORD-BOUNDARY AMPLITUDE DIP — Ex path
    // ============================================
    // Mirror of the emitFrames word-boundary dip.
    // Emit a brief reduced-amplitude micro-frame at word starts so the
    // listener can parse word boundaries, especially at high speech rates.
    // Context-aware: deeper dip after stops (e.g. "dialog type") where
    // the stop's own release blends into the next word's onset.
    double mainDur = t.durationMs;
    double mainFade = t.fadeMs;

    if (wbDipMs > 0.0 && t.wordStart && hadPrevFrame && mainDur > wbDipMs + 1.0) {
      double dipDepth = lang.wordBoundaryDipDepth;

      // After a stop, deepen the dip — the stop's release can smear into
      // the next word's onset, especially at high rates.
      if (prevTokenWasStop) {
        dipDepth *= 0.5; // e.g. 0.50 * 0.5 = 0.25 → 75% amplitude cut
      }

      FELOG("WB-DIP-NORM wordStart=%d prevWasStop=%d dipDepth=%.2f dipMs=%.1f dur=%.1f\n",
            (int)t.wordStart, (int)prevTokenWasStop, dipDepth, wbDipMs, mainDur);

      const double dipDur = wbDipMs;

      double dip[kFrameFieldCount];
      std::memcpy(dip, base, sizeof(dip));
      dip[va] *= dipDepth;
      dip[fa] *= dipDepth;
      dip[static_cast<int>(FieldId::aspirationAmplitude)] *= dipDepth;

      nvspFrontend_Frame dipFrame;
      std::memcpy(&dipFrame, dip, sizeof(dipFrame));
      cb(userData, &dipFrame, &frameEx, dipDur, mainFade, userIndexBase);
      hadPrevFrame = true;

      mainDur -= dipDur;
      mainFade = dipDur; // crossfade from dip back to full amplitude
    }

    // Normal frame emission
    nvspFrontend_Frame frame;
    std::memcpy(&frame, base, sizeof(frame));

    // Trajectory limiting is handled by the trajectory_limit pass (fade-based,
    // Trans-F aware).  We only track state needed by micro-event emission here.
    const bool isNasal = t.def && ((t.def->flags & kIsNasal) != 0);
    trajectoryState->prevVoiceAmp = base[va];
    trajectoryState->prevFricAmp = base[fa];
    trajectoryState->hasPrevFrame = true;
    trajectoryState->prevWasNasal = isNasal;

    // Post-tap: cap fade on the token after a tap so the tap's amplitude
    // notch rings briefly before the next sound smears it away.
    double emitFade = mainFade;
    if (prevTokenWasTap && mainDur > 0.0) {
      emitFade = std::min(emitFade, mainDur * 0.50);
    }

    // Pitch-dependent fade floor: at low F0, each glottal cycle is longer
    // so crossfades span fewer cycles and individual frame boundaries become
    // audible (choppy at low pitch). Enforce a minimum of 2 glottal cycles.
    if (base[vp] > 0.0) {
      double minFadeMs = 2000.0 / base[vp]; // 2 cycles at current pitch
      if (emitFade < minFadeMs && mainDur > minFadeMs) {
        emitFade = minFadeMs;
      }
    }

    cb(userData, &frame, &frameEx, mainDur, emitFade, userIndexBase);
    hadPrevFrame = true;

    prevTokenWasTap = false;
    prevTokenWasStop = t.def && (
      ((t.def->flags & kIsStop) != 0) ||
      ((t.def->flags & kIsAfricate) != 0) ||
      t.postStopAspiration);
  }
}

} // namespace nvsp_frontend