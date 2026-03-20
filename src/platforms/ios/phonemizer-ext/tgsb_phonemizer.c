/*
 * tgsb_phonemizer.c — eSpeak phonemizer implementation.
 *
 * License: GPL-3.0
 */

#include "tgsb_phonemizer.h"

#include <espeak-ng/speak_lib.h>
#include <stdlib.h>
#include <string.h>

static int g_initialized = 0;

/* Pad emoji codepoints with spaces so eSpeak treats them as separate
 * words for $textmode dictionary lookup.  Caller must free() result. */
static char *pad_emoji(const char *text)
{
    size_t slen = strlen(text);
    size_t cap = slen * 2 + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t oi = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        /* 4-byte UTF-8: emoji in U+1F000..U+1FFFF */
        if (p[0] == 0xF0 && p[1] >= 0x9F && p[1] <= 0x9F &&
            (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            if (oi > 0 && out[oi-1] != ' ') out[oi++] = ' ';
            out[oi++] = p[0]; out[oi++] = p[1];
            out[oi++] = p[2]; out[oi++] = p[3];
            p += 4;
            /* skip variation selectors */
            while (p[0] == 0xEF && p[1] == 0xB8 && (p[2] == 0x8E || p[2] == 0x8F)) {
                out[oi++] = p[0]; out[oi++] = p[1]; out[oi++] = p[2];
                p += 3;
            }
            if (*p && *p != ' ') out[oi++] = ' ';
            continue;
        }
        /* 3-byte UTF-8: U+2600..U+27BF */
        if (p[0] == 0xE2 && p[1] >= 0x98 && p[1] <= 0x9E &&
            (p[2] & 0xC0) == 0x80) {
            if (oi > 0 && out[oi-1] != ' ') out[oi++] = ' ';
            out[oi++] = p[0]; out[oi++] = p[1]; out[oi++] = p[2];
            p += 3;
            while (p[0] == 0xEF && p[1] == 0xB8 && (p[2] == 0x8E || p[2] == 0x8F)) {
                out[oi++] = p[0]; out[oi++] = p[1]; out[oi++] = p[2];
                p += 3;
            }
            if (*p && *p != ' ') out[oi++] = ' ';
            continue;
        }
        out[oi++] = *p++;
    }
    out[oi] = '\0';
    return out;
}

int tgsb_phonemizer_init(const char *espeakDataPath)
{
    if (g_initialized) return 1;

    int sr = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0,
                               espeakDataPath, 0x8000);
    if (sr <= 0) return 0;

    /* Verify data actually loaded */
    espeak_VOICE voice_spec;
    memset(&voice_spec, 0, sizeof(voice_spec));
    voice_spec.languages = "en-us";
    if (espeak_SetVoiceByProperties(&voice_spec) != EE_OK) {
        espeak_Terminate();
        return 0;
    }

    g_initialized = 1;
    return 1;
}

void tgsb_phonemizer_terminate(void)
{
    if (g_initialized) {
        espeak_Terminate();
        g_initialized = 0;
    }
}

int tgsb_phonemizer_set_language(const char *espeakLang)
{
    if (!g_initialized || !espeakLang) return 0;

    espeak_VOICE voice_spec;
    memset(&voice_spec, 0, sizeof(voice_spec));
    voice_spec.languages = espeakLang;
    return espeak_SetVoiceByProperties(&voice_spec) == EE_OK ? 1 : 0;
}

char *tgsb_phonemizer_phonemize(const char *text)
{
    if (!g_initialized || !text || !*text) return NULL;

    /* Accumulate IPA clauses into a growable buffer */
    size_t cap = 1024;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';

    /* Pad emoji with spaces so eSpeak treats them as separate words */
    char *emojiPadded = pad_emoji(text);
    const char *inputText = emojiPadded ? emojiPadded : text;

    const void *textPtr = inputText;
    while (textPtr && *(const char *)textPtr) {
        const char *clauseStart = (const char *)textPtr;
        const char *ipa = espeak_TextToPhonemes(
            &textPtr, espeakCHARS_UTF8, 0x02 /* IPA */);
        if (!ipa || !*ipa) continue;

        /* Detect clause type by scanning backward through the text
         * that eSpeak consumed, same pattern as tgsb_bridge.cpp.
         * Skip closing quotes/brackets before checking punctuation. */
        const char *clauseEnd = textPtr
            ? (const char *)textPtr : clauseStart + strlen(clauseStart);
        char clauseType = '.';
        for (const char *cp = clauseEnd - 1; cp >= clauseStart; --cp) {
            char c = *cp;
            /* Skip closing quotes and brackets */
            if (c == ')' || c == ']' || c == '"' || c == '\'') continue;
            /* UTF-8 multi-byte: skip right single/double quotes
             * U+2019 = E2 80 99, U+201D = E2 80 9D */
            if (c == (char)0x99 || c == (char)0x9D) {
                if (cp - 2 >= clauseStart &&
                    *(cp-2) == (char)0xE2 && *(cp-1) == (char)0x80) {
                    cp -= 2;
                    continue;
                }
            }
            if (c == '.' || c == ',' || c == '?' || c == '!') {
                clauseType = c;
                break;
            }
            /* Non-quote, non-punct character — stop scanning */
            break;
        }

        size_t ipaLen = strlen(ipa);
        /* +3 for newline separator + clauseType prefix + null */
        while (len + ipaLen + 3 > cap) {
            cap *= 2;
            char *tmp = (char *)realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }

        if (len > 0) {
            buf[len++] = '\n';
        }
        /* Prefix each clause with its type character */
        buf[len++] = clauseType;
        memcpy(buf + len, ipa, ipaLen);
        len += ipaLen;
        buf[len] = '\0';
    }

    free(emojiPadded);
    return buf;
}
