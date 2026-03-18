/*
 * tgsb_synth.cpp — Synthesis-only bridge (MIT, no eSpeak/GPL).
 *
 * Same pipeline as tgsb_bridge.cpp but takes IPA directly
 * instead of plain text. eSpeak phonemization happens in the
 * XPC service process.
 *
 * License: MIT
 */

#include "tgsb_synth.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "speechPlayer.h"
#include "nvspFrontend.h"

/* ------------------------------------------------------------------ */
/* Voice presets (identical to tgsb_bridge.cpp)                        */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t offset;
    double value;
    int isMultiplier;
} FrameOverride;

typedef struct {
    const char *name;
    const FrameOverride *overrides;
    int numOverrides;
    double voicedTiltDbPerOct;
    int hasVoicedTilt;
} VoicePreset;

#define OFF(field) offsetof(speechPlayer_frame_t, field)

static const FrameOverride kAdamOverrides[] = {
    { OFF(cb1), 1.3, 1 },
    { OFF(pa6), 1.3, 1 },
    { OFF(fricationAmplitude), 0.85, 1 },
};

static const FrameOverride kBenjaminOverrides[] = {
    { OFF(cf1), 1.01, 1 },
    { OFF(cf2), 1.02, 1 },
    { OFF(cf4), 3770.0, 0 },
    { OFF(cf5), 4100.0, 0 },
    { OFF(cf6), 5000.0, 0 },
    { OFF(cfNP), 0.9, 1 },
    { OFF(cb1), 1.3, 1 },
    { OFF(fricationAmplitude), 0.7, 1 },
    { OFF(pa6), 1.3, 1 },
};

static const FrameOverride kCalebOverrides[] = {
    { OFF(aspirationAmplitude), 1.0, 0 },
    { OFF(voiceAmplitude), 0.0, 0 },
};

static const FrameOverride kDavidOverrides[] = {
    { OFF(voicePitch), 0.75, 1 },
    { OFF(endVoicePitch), 0.75, 1 },
    { OFF(cf1), 0.90, 1 },
    { OFF(cf2), 0.93, 1 },
    { OFF(cf3), 0.95, 1 },
};

static const FrameOverride kRobertOverrides[] = {
    { OFF(voicePitch), 1.10, 1 },
    { OFF(endVoicePitch), 1.10, 1 },
    { OFF(cf1), 1.02, 1 }, { OFF(cf2), 1.06, 1 }, { OFF(cf3), 1.08, 1 },
    { OFF(cf4), 1.08, 1 }, { OFF(cf5), 1.10, 1 }, { OFF(cf6), 1.05, 1 },
    { OFF(cb1), 0.65, 1 }, { OFF(cb2), 0.68, 1 }, { OFF(cb3), 0.72, 1 },
    { OFF(cb4), 0.75, 1 }, { OFF(cb5), 0.78, 1 }, { OFF(cb6), 0.80, 1 },
    { OFF(glottalOpenQuotient), 0.30, 0 },
    { OFF(voiceTurbulenceAmplitude), 0.20, 1 },
    { OFF(fricationAmplitude), 0.75, 1 },
    { OFF(parallelBypass), 0.70, 1 },
    { OFF(pa3), 1.08, 1 }, { OFF(pa4), 1.15, 1 },
    { OFF(pa5), 1.20, 1 }, { OFF(pa6), 1.25, 1 },
    { OFF(pb1), 0.72, 1 }, { OFF(pb2), 0.75, 1 }, { OFF(pb3), 0.78, 1 },
    { OFF(pb4), 0.80, 1 }, { OFF(pb5), 0.82, 1 }, { OFF(pb6), 0.85, 1 },
    { OFF(pf3), 1.06, 1 }, { OFF(pf4), 1.08, 1 },
    { OFF(pf5), 1.10, 1 }, { OFF(pf6), 1.00, 1 },
};

#define PRESET(name, arr, tilt, hasTilt) \
    { name, arr, sizeof(arr)/sizeof(arr[0]), tilt, hasTilt }

static const VoicePreset kPresets[] = {
    PRESET("adam",     kAdamOverrides,     0.0,  0),
    PRESET("benjamin", kBenjaminOverrides, 0.0,  0),
    PRESET("caleb",    kCalebOverrides,    0.0,  0),
    PRESET("david",    kDavidOverrides,    0.0,  0),
    PRESET("robert",   kRobertOverrides,  -6.0,  1),
};
static const int kNumPresets = sizeof(kPresets) / sizeof(kPresets[0]);

static void applyOverrides(speechPlayer_frame_t *f,
                           const FrameOverride *ov, int count) {
    for (int i = 0; i < count; i++) {
        double *field = (double *)((char *)f + ov[i].offset);
        if (ov[i].isMultiplier)
            *field *= ov[i].value;
        else
            *field = ov[i].value;
    }
}

/* ------------------------------------------------------------------ */
/* Synth struct                                                        */
/* ------------------------------------------------------------------ */

struct TgsbSynth {
    speechPlayer_handle_t player;
    nvspFrontend_handle_t frontend;
    int sampleRate;
    volatile int stopRequested;
    int voiceIndex;
};

typedef struct {
    TgsbSynth *synth;
    int frameCount;
} FrameCtx;

static void onFrame(
    void *userData,
    const nvspFrontend_Frame *frameOrNull,
    const nvspFrontend_FrameEx *frameExOrNull,
    double durationMs,
    double fadeMs,
    int userIndex
) {
    FrameCtx *ctx = (FrameCtx *)userData;
    if (!ctx || !ctx->synth || !ctx->synth->player) return;

    int sr = ctx->synth->sampleRate;
    unsigned int minSamples = durationMs > 0.0
        ? (unsigned int)(durationMs * sr / 1000.0 + 0.5) : 0;
    unsigned int fadeSamples = fadeMs > 0.0
        ? (unsigned int)(fadeMs * sr / 1000.0 + 0.5) : 0;

    if (frameOrNull) {
        speechPlayer_frame_t f;
        memcpy(&f, frameOrNull, sizeof(f));

        const VoicePreset *vp = &kPresets[ctx->synth->voiceIndex];
        applyOverrides(&f, vp->overrides, vp->numOverrides);

        if (frameExOrNull) {
            speechPlayer_queueFrameEx(ctx->synth->player, &f,
                (const speechPlayer_frameEx_t *)frameExOrNull,
                (unsigned int)sizeof(nvspFrontend_FrameEx),
                minSamples, fadeSamples, userIndex, 0);
        } else {
            speechPlayer_queueFrame(ctx->synth->player, &f,
                minSamples, fadeSamples, userIndex, 0);
        }
    } else {
        speechPlayer_queueFrame(ctx->synth->player, NULL,
            minSamples, fadeSamples, userIndex, 0);
    }
    ctx->frameCount++;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

extern "C" {

TgsbSynth *tgsb_synth_create(const char *packDir, int sampleRate)
{
    speechPlayer_handle_t player = speechPlayer_initialize(sampleRate);
    nvspFrontend_handle_t fe = nvspFrontend_create(packDir);
    if (!fe) {
        speechPlayer_terminate(player);
        return NULL;
    }
    nvspFrontend_setLanguage(fe, "en-us");

    speechPlayer_voicingTone_t tone = speechPlayer_getDefaultVoicingTone();
    speechPlayer_setVoicingTone(player, &tone);

    TgsbSynth *synth = (TgsbSynth *)calloc(1, sizeof(TgsbSynth));
    synth->player = player;
    synth->frontend = fe;
    synth->sampleRate = sampleRate;
    synth->stopRequested = 0;
    synth->voiceIndex = 0;

    return synth;
}

void tgsb_synth_destroy(TgsbSynth *synth)
{
    if (!synth) return;
    if (synth->frontend) nvspFrontend_destroy(synth->frontend);
    if (synth->player) speechPlayer_terminate(synth->player);
    free(synth);
}

int tgsb_synth_set_language(TgsbSynth *synth, const char *tgsbLang)
{
    if (!synth) return 0;
    return nvspFrontend_setLanguage(synth->frontend, tgsbLang);
}

int tgsb_synth_set_voice(TgsbSynth *synth, const char *voiceName)
{
    if (!synth) return 0;

    for (int i = 0; i < kNumPresets; i++) {
        if (strcmp(kPresets[i].name, voiceName) == 0) {
            synth->voiceIndex = i;

            speechPlayer_voicingTone_t tone =
                speechPlayer_getDefaultVoicingTone();
            if (kPresets[i].hasVoicedTilt)
                tone.voicedTiltDbPerOct = kPresets[i].voicedTiltDbPerOct;
            speechPlayer_setVoicingTone(synth->player, &tone);
            return 1;
        }
    }
    return 0;
}

void tgsb_synth_queue_ipa(TgsbSynth *synth,
                           const char *ipa,
                           double speed,
                           double pitch)
{
    if (!synth || !synth->player || !synth->frontend) return;
    if (!ipa || !*ipa) return;

    synth->stopRequested = 0;

    /* Purge stale frames */
    speechPlayer_queueFrame(synth->player, NULL, 0, 0, -1, true);

    if (speed < 0.1) speed = 0.1;

    /* Automatic speed split: cap synthesis at 2.0x, frame-advance the rest */
    const double kSynthCap = 2.0;
    double timeStretch = 1.0;
    if (speed > kSynthCap) {
        timeStretch = speed / kSynthCap;
        speed = kSynthCap;
    }
    speechPlayer_setTimeStretch(synth->player, timeStretch);

    if (pitch < 40.0) pitch = 40.0;
    if (pitch > 500.0) pitch = 500.0;

    /* IPA clauses are newline-separated from the XPC service */
    FrameCtx ctx;
    ctx.synth = synth;
    ctx.frameCount = 0;

    /* Process each clause */
    const char *p = ipa;
    while (*p && !synth->stopRequested) {
        /* Find end of this clause */
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len > 0) {
            /* Each clause is prefixed with its type character
             * (e.g. ".ipa_here" or "?ipa_here") from the phonemizer.
             * Extract it and advance past the prefix. */
            char clauseType = '.';
            const char *ipaStart = p;
            if (len >= 2) {
                char first = *p;
                if (first == '.' || first == ',' || first == '?' || first == '!') {
                    clauseType = first;
                    ipaStart = p + 1;
                    len -= 1;
                }
            }

            char *clause = (char *)malloc(len + 1);
            if (clause) {
                memcpy(clause, ipaStart, len);
                clause[len] = '\0';

                char clauseStr[2] = { clauseType, '\0' };
                nvspFrontend_queueIPA_Ex(
                    synth->frontend, clause,
                    speed, pitch, 0.5, clauseStr, 0,
                    onFrame, &ctx
                );

                free(clause);
            }
        }

        p += len;
        if (*p == '\n') p++;
    }
}

int tgsb_synth_pull_audio(TgsbSynth *synth,
                           int16_t *outBuffer,
                           int maxSamples)
{
    if (!synth || !synth->player || synth->stopRequested) return 0;
    if (maxSamples <= 0) return 0;
    if (maxSamples > 4096) maxSamples = 4096;

    sample buf[4096];
    int n = speechPlayer_synthesize(synth->player,
                                    (unsigned int)maxSamples, buf);
    if (n <= 0) return 0;

    static const double kGain = 1.8;
    for (int i = 0; i < n; i++) {
        double s = buf[i].value * kGain;
        if (s > 32767.0) s = 32767.0;
        if (s < -32767.0) s = -32767.0;
        outBuffer[i] = (int16_t)s;
    }

    return n;
}

void tgsb_synth_stop(TgsbSynth *synth)
{
    if (synth) synth->stopRequested = 1;
}

int tgsb_synth_get_num_voices(void)
{
    return kNumPresets;
}

const char *tgsb_synth_get_voice_name(int index)
{
    if (index < 0 || index >= kNumPresets) return NULL;
    return kPresets[index].name;
}

} /* extern "C" */
