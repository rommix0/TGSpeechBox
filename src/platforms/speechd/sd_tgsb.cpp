/*
 * sd_tgsb — Native Speech Dispatcher module for TGSpeechBox.
 * Copyright 2025-2026 Tamas Geczy.  Licensed under the MIT License.
 *
 * Persistent process: initializes espeak + frontend + player once at startup.
 * Handles SPEAK/STOP/SET/LIST VOICES/QUIT via the SD module text protocol.
 * Audio sent back as HDLC-escaped 705 AUDIO blocks.
 *
 * Architecture matches iOS/Android/SAPI: single process, zero pipes.
 * espeak-ng loaded via dlopen (no build-time dependency).
 */

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include "speechPlayer.h"
#include "nvspFrontend.h"

// ============================================================================
// eSpeak-NG runtime loading (same as tgsbRender.cpp)
// ============================================================================

enum { ESPEAK_AUDIO_OUTPUT_SYNCH = 0x02 };
enum { ESPEAK_CHARS_UTF8 = 1 };
enum { ESPEAK_INITIALIZE_DONT_EXIT = 0x8000 };
enum { ESPEAK_EE_OK = 0 };

struct espeak_VOICE_local {
  const char *name;
  const char *languages;
  const char *identifier;
  unsigned char gender, age, variant, xx1;
  int score;
  void *spare;
};

typedef int  (*fn_espeak_Initialize)(int, int, const char*, int);
typedef int  (*fn_espeak_SetVoiceByProperties)(espeak_VOICE_local*);
typedef const char* (*fn_espeak_TextToPhonemes)(const void**, int, int);
typedef int  (*fn_espeak_Terminate)(void);

struct EspeakLib {
  void* handle = nullptr;
  fn_espeak_Initialize       Initialize = nullptr;
  fn_espeak_SetVoiceByProperties SetVoiceByProperties = nullptr;
  fn_espeak_TextToPhonemes   TextToPhonemes = nullptr;
  fn_espeak_Terminate        Terminate = nullptr;

  bool load(const char* dataPath) {
    handle = dlopen("libespeak-ng.so.1", RTLD_LAZY);
    if (!handle) handle = dlopen("libespeak-ng.so", RTLD_LAZY);
    if (!handle) return false;
    Initialize = (fn_espeak_Initialize)dlsym(handle, "espeak_Initialize");
    SetVoiceByProperties = (fn_espeak_SetVoiceByProperties)dlsym(handle, "espeak_SetVoiceByProperties");
    TextToPhonemes = (fn_espeak_TextToPhonemes)dlsym(handle, "espeak_TextToPhonemes");
    Terminate = (fn_espeak_Terminate)dlsym(handle, "espeak_Terminate");
    if (!Initialize || !SetVoiceByProperties || !TextToPhonemes || !Terminate) {
      dlclose(handle); handle = nullptr; return false;
    }
    int sr = Initialize(ESPEAK_AUDIO_OUTPUT_SYNCH, 0,
                        (dataPath && dataPath[0]) ? dataPath : nullptr,
                        ESPEAK_INITIALIZE_DONT_EXIT);
    if (sr <= 0) { dlclose(handle); handle = nullptr; return false; }
    return true;
  }

  bool setVoice(const char* lang) {
    espeak_VOICE_local spec{};
    spec.languages = lang;
    return SetVoiceByProperties(&spec) == ESPEAK_EE_OK;
  }

  std::string phonemize(const char* text) {
    std::string combined;
    const void *ptr = text;
    while (ptr && *(const char*)ptr) {
      const char *ipa = TextToPhonemes(&ptr, ESPEAK_CHARS_UTF8, 0x02);
      if (!ipa || !*ipa) continue;
      if (!combined.empty()) combined += ' ';
      combined += ipa;
    }
    return combined;
  }

  void close() {
    if (handle) { if (Terminate) Terminate(); dlclose(handle); handle = nullptr; }
  }
};

// ============================================================================
// Built-in voice presets (same table as tgsbRender.cpp)
// ============================================================================

struct BuiltinVoice {
  const char* name;
  double voicePitch_mul, endVoicePitch_mul;
  double cf1_mul, cf2_mul, cf3_mul, cf4_mul, cf5_mul, cf6_mul;
  double cb1_mul, cb2_mul, cb3_mul, cb4_mul, cb5_mul, cb6_mul;
  double pb1_mul, pb2_mul, pb3_mul, pb4_mul, pb5_mul, pb6_mul;
  double pf3_mul, pf4_mul, pf5_mul, pf6_mul;
  double pa3_mul, pa4_mul, pa5_mul, pa6_mul;
  double fricationAmplitude_mul, parallelBypass_mul, voiceTurbulenceAmplitude_mul;
  double voiceAmplitude_abs, aspirationAmplitude_abs, glottalOpenQuotient_abs;
  double vibratoPitchOffset_abs, vibratoSpeed_abs;
  double cf4_abs, cf5_abs, cf6_abs, cfNP_mul;
  double voicedTiltDbPerOct;
  bool hasVoicedTilt;
};

static const BuiltinVoice kBuiltinVoices[] = {
  {"Adam", 1,1, 1,1,1,1,1,1, 1.3,1,1,1,1,1, 1,1,1,1,1,1, 1,1,1,1, 1,1,1,1.3,
   0.85,1,1, NAN,NAN,NAN,NAN,NAN, NAN,NAN,NAN,1.0, 0.0,true},
  {"Benjamin", 1,1, 1.01,1.02,1,1,1,1, 1.3,1,1,1,1,1, 1,1,1,1,1,1, 1,1,1,1, 1,1,1,1.3,
   0.7,1,1, NAN,NAN,NAN,NAN,NAN, 3770,4100,5000,0.9, 0.0,true},
  {"Caleb", 1,1, 1,1,1,1,1,1, 1,1,1,1,1,1, 1,1,1,1,1,1, 1,1,1,1, 1,1,1,1,
   1,1,1, 0.0,1.0,NAN,NAN,NAN, NAN,NAN,NAN,1.0, 0.0,true},
  {"David", 0.75,0.75, 0.90,0.93,0.95,1,1,1, 1,1,1,1,1,1, 1,1,1,1,1,1, 1,1,1,1, 1,1,1,1,
   1,1,1, NAN,NAN,NAN,NAN,NAN, NAN,NAN,NAN,1.0, 0.0,true},
  {"Robert", 1.10,1.10, 1.02,1.06,1.08,1.08,1.10,1.05, 0.65,0.68,0.72,0.75,0.78,0.80,
   0.72,0.75,0.78,0.80,0.82,0.85, 1.06,1.08,1.10,1.00, 1.08,1.15,1.20,1.25,
   0.75,0.70,0.20, NAN,NAN,0.30,0.0,0.0, NAN,NAN,NAN,1.0, -6.0,true},
};
static constexpr int kNumVoices = sizeof(kBuiltinVoices) / sizeof(kBuiltinVoices[0]);

static const BuiltinVoice* findBuiltinVoice(const char* name) {
  for (int i = 0; i < kNumVoices; i++) {
    // Case-insensitive compare
    const char *a = name, *b = kBuiltinVoices[i].name;
    bool match = true;
    for (; *a && *b; a++, b++) {
      if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) { match = false; break; }
    }
    if (match && !*a && !*b) return &kBuiltinVoices[i];
  }
  return nullptr;
}

static void applyBuiltinVoice(speechPlayer_frame_t& f, const BuiltinVoice& v) {
  f.voicePitch *= v.voicePitch_mul;  f.endVoicePitch *= v.endVoicePitch_mul;
  f.cf1 *= v.cf1_mul; f.cf2 *= v.cf2_mul; f.cf3 *= v.cf3_mul;
  f.cf4 *= v.cf4_mul; f.cf5 *= v.cf5_mul; f.cf6 *= v.cf6_mul;
  f.cb1 *= v.cb1_mul; f.cb2 *= v.cb2_mul; f.cb3 *= v.cb3_mul;
  f.cb4 *= v.cb4_mul; f.cb5 *= v.cb5_mul; f.cb6 *= v.cb6_mul;
  f.pa3 *= v.pa3_mul; f.pa4 *= v.pa4_mul; f.pa5 *= v.pa5_mul; f.pa6 *= v.pa6_mul;
  f.pb1 *= v.pb1_mul; f.pb2 *= v.pb2_mul; f.pb3 *= v.pb3_mul;
  f.pb4 *= v.pb4_mul; f.pb5 *= v.pb5_mul; f.pb6 *= v.pb6_mul;
  f.pf3 *= v.pf3_mul; f.pf4 *= v.pf4_mul; f.pf5 *= v.pf5_mul; f.pf6 *= v.pf6_mul;
  f.fricationAmplitude *= v.fricationAmplitude_mul;
  f.parallelBypass *= v.parallelBypass_mul;
  f.voiceTurbulenceAmplitude *= v.voiceTurbulenceAmplitude_mul;
  f.cfNP *= v.cfNP_mul;
  if (!std::isnan(v.voiceAmplitude_abs))      f.voiceAmplitude = v.voiceAmplitude_abs;
  if (!std::isnan(v.aspirationAmplitude_abs)) f.aspirationAmplitude = v.aspirationAmplitude_abs;
  if (!std::isnan(v.glottalOpenQuotient_abs)) f.glottalOpenQuotient = v.glottalOpenQuotient_abs;
  if (!std::isnan(v.vibratoPitchOffset_abs))  f.vibratoPitchOffset = v.vibratoPitchOffset_abs;
  if (!std::isnan(v.vibratoSpeed_abs))        f.vibratoSpeed = v.vibratoSpeed_abs;
  if (!std::isnan(v.cf4_abs)) f.cf4 = v.cf4_abs;
  if (!std::isnan(v.cf5_abs)) f.cf5 = v.cf5_abs;
  if (!std::isnan(v.cf6_abs)) f.cf6 = v.cf6_abs;
}

// ============================================================================
// Emoji padding (same as tgsb_bridge.cpp)
// ============================================================================

static std::string padEmojiWithSpaces(const char *text) {
  std::string out;
  out.reserve(std::strlen(text) * 2);
  const unsigned char *p = (const unsigned char *)text;
  while (*p) {
    if (p[0] == 0xF0 && p[1] >= 0x9F && p[1] <= 0x9F &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      if (!out.empty() && out.back() != ' ') out += ' ';
      out += (char)p[0]; out += (char)p[1]; out += (char)p[2]; out += (char)p[3];
      p += 4;
      while (p[0] == 0xEF && p[1] == 0xB8 && (p[2] == 0x8E || p[2] == 0x8F)) {
        out += (char)p[0]; out += (char)p[1]; out += (char)p[2]; p += 3;
      }
      if (*p && *p != ' ') out += ' ';
      continue;
    }
    if (p[0] == 0xE2 && p[1] >= 0x98 && p[1] <= 0x9E && (p[2] & 0xC0) == 0x80) {
      if (!out.empty() && out.back() != ' ') out += ' ';
      out += (char)p[0]; out += (char)p[1]; out += (char)p[2];
      p += 3;
      while (p[0] == 0xEF && p[1] == 0xB8 && (p[2] == 0x8E || p[2] == 0x8F)) {
        out += (char)p[0]; out += (char)p[1]; out += (char)p[2]; p += 3;
      }
      if (*p && *p != ' ') out += ' ';
      continue;
    }
    out += (char)*p++;
  }
  return out;
}

// ============================================================================
// Parameter mapping (same as tgsbRender.cpp)
// ============================================================================

static double ssipRateToSpeed(int ssipRate) {
  if (ssipRate < -100) ssipRate = -100;
  if (ssipRate > 100) ssipRate = 100;
  return std::pow(2.0, ssipRate / 100.0);
}

static double sliderPitchToBaseHz(int pitch0to100) {
  if (pitch0to100 < 0) pitch0to100 = 0;
  if (pitch0to100 > 100) pitch0to100 = 100;
  return 25.0 + (21.25 * (pitch0to100 / 12.5));
}

// ============================================================================
// SD protocol I/O
// ============================================================================

static std::mutex g_outMtx;

static FILE* g_dbg = nullptr;
static void dbg(const char* fmt, ...) {
  if (!g_dbg) g_dbg = fopen("/tmp/sd_tgsb.log", "a");
  if (!g_dbg) return;
  va_list ap; va_start(ap, fmt); vfprintf(g_dbg, fmt, ap); va_end(ap);
  fputc('\n', g_dbg); fflush(g_dbg);
}

// ============================================================================
// Shared read buffer — both sd_readline() and sd_poll_stop() use raw read()
// on STDIN_FILENO through this buffer. NEVER use fgets/fread on stdin.
// This avoids mixing buffered and raw I/O which causes lost commands.
// ============================================================================

static std::string g_readBuf;
static bool g_eof = false;

static std::string sd_readline() {
  while (true) {
    // Check for a complete line in the buffer
    auto nl = g_readBuf.find('\n');
    if (nl != std::string::npos) {
      std::string line = g_readBuf.substr(0, nl);
      g_readBuf.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      dbg("RECV: %s", line.c_str());
      return line;
    }
    if (g_eof) return {};
    // Read more from stdin
    char buf[4096];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) { g_eof = true; dbg("RECV: <EOF>"); return {}; }
    g_readBuf.append(buf, (size_t)n);
  }
}

static std::string sd_read_text() {
  std::string text;
  while (true) {
    std::string line = sd_readline();
    if (line == ".") break;
    if (line.empty() && g_eof) break;
    if (!line.empty() && line[0] == '.') line = line.substr(1);
    if (!text.empty()) text += ' ';
    text += line;
  }
  return text;
}

static void sd_send(const char* msg) {
  std::lock_guard<std::mutex> lock(g_outMtx);
  fputs(msg, stdout);
  fputc('\n', stdout);
  fflush(stdout);
  dbg("SEND: %s", msg);
}

// ============================================================================
// Audio output — 705 AUDIO protocol (SD handles playback + flushing).
//
// Same approach as sd_espeak-ng: synchronous, single-threaded.
// Audio chunks sent via HDLC-escaped 705 blocks. SD server plays them
// and can flush instantly on STOP — no buffer tail issues.
// Format from speechd/src/modules/module_process.c.
// ============================================================================

static void sd_send_audio(const int16_t* samples, int count, int sampleRate) {
  std::lock_guard<std::mutex> lock(g_outMtx);

  fprintf(stdout, "705-bits=16\n");
  fprintf(stdout, "705-num_channels=1\n");
  fprintf(stdout, "705-sample_rate=%d\n", sampleRate);
  fprintf(stdout, "705-num_samples=%d\n", count);
  fprintf(stdout, "705-big_endian=0\n");
  // "705-AUDIO" followed by null byte — marks start of binary data
  fprintf(stdout, "705-AUDIO");
  fputc(0, stdout);

  // HDLC escaping: only 0x7D (escape) and 0x0A (newline) need escaping.
  // NOT 0x00 — that's valid data. (Confirmed from speechd source.)
  const char escape = 0x7D;
  const char invert = 1 << 5;  // 0x20
  const uint8_t* p = (const uint8_t*)samples;
  const uint8_t* end = p + (size_t)count * 2;
  while (p < end) {
    // Find next special character
    const uint8_t* esc = (const uint8_t*)memchr(p, escape, end - p);
    const uint8_t* nl  = (const uint8_t*)memchr(p, '\n', end - p);
    const uint8_t* stop = nullptr;
    if (esc && (!nl || esc < nl)) stop = esc;
    else if (nl) stop = nl;
    if (!stop) stop = end;
    // Write non-special bytes
    if (stop > p) fwrite(p, 1, stop - p, stdout);
    // Escape the special byte
    if (stop < end) {
      fputc(escape, stdout);
      fputc((*stop) ^ invert, stdout);
      p = stop + 1;
    } else {
      p = stop;
    }
  }

  fputc('\n', stdout);
  fprintf(stdout, "705 AUDIO\n");
  fflush(stdout);
}

// Non-blocking check for STOP/CANCEL on stdin.
// Reads into the shared buffer so sd_readline() can process the full commands later.
static bool sd_poll_stop() {
  fd_set rfds;
  struct timeval tv = {0, 0};  // non-blocking
  FD_ZERO(&rfds);
  FD_SET(STDIN_FILENO, &rfds);
  if (select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv) <= 0)
    return false;

  // Read available data into shared buffer
  char buf[4096];
  ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
  if (n <= 0) { g_eof = true; return true; }  // EOF = stop
  g_readBuf.append(buf, (size_t)n);

  // Check if buffer contains STOP or CANCEL (leave data for sd_readline)
  if (g_readBuf.find("STOP") != std::string::npos ||
      g_readBuf.find("CANCEL") != std::string::npos) {
    dbg("POLL: got STOP/CANCEL in buffer");
    return true;
  }
  return false;
}

// ============================================================================
// Frame callback (queues to speechPlayer — same as tgsbRender)
// ============================================================================

struct SynthCtx {
  speechPlayer_handle_t player;
  int sampleRate;
  double volume;
  const BuiltinVoice* builtinVoice;
};

static int g_frameCount = 0;
static void onFrame(void* userData,
                    const nvspFrontend_Frame* frameOrNull,
                    const nvspFrontend_FrameEx* frameExOrNull,
                    double durationMs, double fadeMs, int userIndex) {
  auto* ctx = (SynthCtx*)userData;
  if (!ctx || !ctx->player) { dbg("onFrame: null ctx/player!"); return; }
  g_frameCount++;

  auto msToSamples = [&](double ms) -> unsigned int {
    return ms > 0.0 ? (unsigned int)(ms * ctx->sampleRate / 1000.0 + 0.5) : 0;
  };

  unsigned int minSamples = msToSamples(durationMs);
  unsigned int fadeSamples = msToSamples(fadeMs);

  if (frameOrNull) {
    speechPlayer_frame_t f{};
    std::memcpy(&f, frameOrNull, sizeof(f));
    f.outputGain *= ctx->volume;
    if (ctx->builtinVoice) applyBuiltinVoice(f, *ctx->builtinVoice);

    if (frameExOrNull) {
      speechPlayer_queueFrameEx(ctx->player, &f,
          (const speechPlayer_frameEx_t*)frameExOrNull,
          (unsigned int)sizeof(nvspFrontend_FrameEx),
          minSamples, fadeSamples, userIndex, false);
    } else {
      speechPlayer_queueFrame(ctx->player, &f, minSamples, fadeSamples, userIndex, false);
    }
  } else {
    speechPlayer_queueFrame(ctx->player, nullptr, minSamples, fadeSamples, userIndex, false);
  }
}

// ============================================================================
// Synthesis: clause split → phonemize → render → send audio chunks
// ============================================================================

static void synthesize(const std::string& text,
                       EspeakLib& espeak,
                       nvspFrontend_handle_t fe,
                       speechPlayer_handle_t player,
                       int sampleRate,
                       double speed, double basePitchHz, double inflection,
                       double volume, const BuiltinVoice* voice,
                       bool& stopFlag) {
  dbg("SYNTH: text='%s' lang speed=%.2f pitch=%.1f", text.c_str(), speed, basePitchHz);
  sd_send("701 BEGIN");

  SynthCtx ctx;
  ctx.player = player;
  ctx.sampleRate = sampleRate;
  ctx.volume = volume;
  ctx.builtinVoice = voice;

  // Cap synthesis at 2.0x, excess into time-stretch
  double timeStretch = 1.0;
  if (speed > 2.0) { timeStretch = speed / 2.0; speed = 2.0; }
  speechPlayer_setTimeStretch(player, timeStretch);

  // Clause-splitting loop (same as tgsbRender --espeak / tgsb_bridge.cpp)
  const char *p = text.c_str();
  while (*p && !stopFlag) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!*p) break;

    const char *clauseStart = p;
    char clauseType = '.';

    while (*p) {
      char c = *p;
      if (c == '?' || c == '!') { clauseType = c; p++; break; }
      if (c == ',' || c == '.') {
        bool prevDigit = (p > clauseStart) && (unsigned char)(*(p-1) - '0') <= 9;
        bool nextDigit = *(p+1) && (unsigned char)(*(p+1) - '0') <= 9;
        if (prevDigit && nextDigit) { p++; continue; }
        clauseType = c; p++; break;
      }
      if ((unsigned char)c == 0xE2 && (unsigned char)*(p+1) == 0x80 &&
          (unsigned char)*(p+2) == 0xA6) { clauseType = '.'; p += 3; break; }
      if (c == ';' || c == ':') {
        char next = *(p+1);
        if (next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == '\0') {
          clauseType = ','; p++; break;
        }
      }
      p++;
    }

    size_t len = (size_t)(p - clauseStart);
    if (len == 0) continue;

    std::string clause(clauseStart, len);

    // Text normalization
    char *prepared = nvspFrontend_prepareText(fe, clause.c_str());
    if (prepared) { clause = prepared; nvspFrontend_freeString(prepared); }

    // Emoji padding
    std::string padded = padEmojiWithSpaces(clause.c_str());
    if (padded.size() != clause.size()) clause = std::move(padded);

    // Phonemize
    std::string ipa = espeak.phonemize(clause.c_str());
    dbg("SYNTH: clause='%s' ipa='%s'", clause.c_str(), ipa.c_str());
    if (ipa.empty()) continue;

    // Queue frames
    char clauseStr[2] = { clauseType, '\0' };
    int qok = nvspFrontend_queueIPA_ExWithText(fe, clause.c_str(), ipa.c_str(),
        speed, basePitchHz, inflection, clauseStr, 0, onFrame, &ctx);
    dbg("SYNTH: queueIPA returned %d, frames queued: %d", qok, g_frameCount);

    // Synthesize this clause's audio and send via 705 blocks.
    // Poll stdin between chunks for responsive STOP (same as sd_espeak-ng).
    std::vector<sample> pcm(2048);
    int totalSamples = 0;
    while (!stopFlag) {
      int n = speechPlayer_synthesize(player, (unsigned int)pcm.size(), pcm.data());
      if (n <= 0) break;
      totalSamples += n;
      sd_send_audio((const int16_t*)pcm.data(), n, sampleRate);
      // Poll stdin for STOP/CANCEL between chunks
      if (sd_poll_stop()) { stopFlag = true; break; }
    }
    dbg("SYNTH: synthesized %d total samples", totalSamples);
  }

  if (stopFlag) {
    // Drain any remaining frames
    speechPlayer_queueFrame(player, nullptr, 0, 0, -1, true);
    sd_send("703 STOP");
  } else {
    sd_send("702 END");
  }
}

// ============================================================================
// Auto-detect pack directory
// ============================================================================

static std::string findPackDir() {
  const char* env = getenv("TGSB_PACKDIR");
  if (env && env[0]) return env;
  if (access("/usr/local/share/tgspeechbox/packs", F_OK) == 0)
    return "/usr/local/share/tgspeechbox";
  if (access("/usr/share/tgspeechbox/packs", F_OK) == 0)
    return "/usr/share/tgspeechbox";
  return "/usr/local/share/tgspeechbox";
}

// ============================================================================
// main — SD module command loop
// ============================================================================

int main(int argc, char** argv) {
  // Disable buffering on stdout (protocol is line-oriented)
  setvbuf(stdout, nullptr, _IONBF, 0);

  // Ignore SIGPIPE — when we kill paplay on STOP, the next write() to the
  // dead pipe must return EPIPE, not kill our entire module process.
  signal(SIGPIPE, SIG_IGN);

  std::string packDir = findPackDir();
  int sampleRate = 22050;
  std::string language = "en-us";
  std::string voiceName = "Adam";
  int ssipRate = 0;
  int ssipPitch = 50;
  double ssipVolume = 1.0;
  double inflection = 0.5;

  // Parse simple config file if given (one keyword per line)
  if (argc > 1 && argv[1]) {
    FILE* f = fopen(argv[1], "r");
    if (f) {
      char line[1024];
      while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[256], val[256];
        // Parse: Keyword "value" or Keyword value
        if (sscanf(line, "%255s \"%255[^\"]\"", key, val) == 2 ||
            sscanf(line, "%255s %255s", key, val) == 2) {
          if (strcmp(key, "TGSBPackDir") == 0) packDir = val;
          else if (strcmp(key, "TGSBSampleRate") == 0) sampleRate = atoi(val);
          else if (strcmp(key, "TGSBDefaultVoice") == 0) voiceName = val;
          else if (strcmp(key, "TGSBDefaultLanguage") == 0) language = val;
        }
      }
      fclose(f);
    }
  }

  // Wait for INIT from Speech Dispatcher
  std::string cmd = sd_readline();
  if (cmd != "INIT") return 1;

  // Load espeak-ng
  EspeakLib espeak;
  if (!espeak.load(nullptr)) {
    sd_send("399 ERR CANT INIT MODULE");
    return 1;
  }
  if (!espeak.setVoice(language.c_str())) {
    espeak.close();
    sd_send("399 ERR CANT INIT MODULE");
    return 1;
  }

  // Create TGSpeechBox engine
  speechPlayer_handle_t player = speechPlayer_initialize(sampleRate);
  if (!player) {
    espeak.close();
    sd_send("399 ERR CANT INIT MODULE");
    return 1;
  }

  nvspFrontend_handle_t fe = nvspFrontend_create(packDir.c_str());
  if (!fe) {
    speechPlayer_terminate(player);
    espeak.close();
    sd_send("399 ERR CANT INIT MODULE");
    return 1;
  }
  nvspFrontend_setLanguage(fe, language.c_str());

  // Default voicing tone
  speechPlayer_voicingTone_t tone = speechPlayer_getDefaultVoicingTone();
  speechPlayer_setVoicingTone(player, &tone);

  // Apply built-in voice
  const BuiltinVoice* activeVoice = findBuiltinVoice(voiceName.c_str());
  // Note: built-in voice voicedTilt is applied per-frame in applyBuiltinVoice().
  // No need to poke the voicingTone struct directly (was using unsafe pointer arithmetic).

  sd_send("299 OK LOADED SUCCESSFULLY");

  // Synthesis state (single-threaded — no std::thread needed)
  bool stopFlag = false;

  // Command loop
  while (true) {
    cmd = sd_readline();
    if (cmd.empty() && g_eof) break;

    if (cmd == "SPEAK" || cmd == "CHAR" || cmd == "KEY" || cmd == "SOUND_ICON") {
      sd_send("202 OK RECEIVING MESSAGE");
      std::string text = sd_read_text();
      if (text.empty()) {
        sd_send("301 ERROR CANT SPEAK");
        continue;
      }

      stopFlag = false;

      double speed = ssipRateToSpeed(ssipRate);
      double pitch = sliderPitchToBaseHz(ssipPitch);

      // Strip SSML tags (SD wraps text in <speak>...</speak>)
      {
        std::string stripped;
        bool inTag = false;
        for (char c : text) {
          if (c == '<') { inTag = true; continue; }
          if (c == '>') { inTag = false; continue; }
          if (!inTag) stripped += c;
        }
        text = stripped;
      }

      // Handle language=c or language=NULL — use default
      if (language == "c" || language == "C" || language == "NULL" || language.empty()) {
        language = "en-us";
        nvspFrontend_setLanguage(fe, language.c_str());
        espeak.setVoice(language.c_str());
      }

      sd_send("200 OK SPEAKING");

      // Synchronous synthesis — same architecture as sd_espeak-ng.
      // Audio sent via 705 blocks (SD handles playback + flushing).
      // sd_poll_stop() checks stdin between chunks for responsive STOP.
      synthesize(text, espeak, fe, player,
          sampleRate, speed, pitch, inflection,
          ssipVolume, activeVoice, stopFlag);
    }
    else if (cmd == "STOP" || cmd == "CANCEL") {
      // In synchronous mode, STOP arrives via sd_poll_stop() during synthesis.
      // If we get here, synthesis already finished — just purge.
      stopFlag = true;
      speechPlayer_queueFrame(player, nullptr, 0, 0, -1, true);
    }
    else if (cmd == "PAUSE") {
      stopFlag = true;
      speechPlayer_queueFrame(player, nullptr, 0, 0, -1, true);
    }
    else if (cmd == "SET" || cmd.compare(0, 4, "SET ") == 0) {
      // Handle both multi-line "SET\nKEY=val\n." and single-line "SET SELF RATE 50"
      // Multi-line needs "202 OK RECEIVING MESSAGE" ack first.
      auto applyParam = [&](const std::string& rawKey, const std::string& val) {
        // SD sends lowercase keys in multi-line SET blocks (rate=78)
        std::string key = rawKey;
        for (auto& c : key) c = (char)tolower((unsigned char)c);

        if (key == "rate") ssipRate = atoi(val.c_str());
        else if (key == "pitch") ssipPitch = atoi(val.c_str());
        else if (key == "volume") ssipVolume = 1.0 + atoi(val.c_str()) / 100.0;
        else if (key == "voice" || key == "synthesis_voice") {
          // SD sends voice=male1 (generic) and synthesis_voice=Adam (specific).
          // Ignore generic SSIP voice types.
          if (val == "male1" || val == "male2" || val == "male3" ||
              val == "female1" || val == "female2" || val == "female3" ||
              val == "child_male" || val == "child_female" || val == "NULL")
            return;
          voiceName = val;
          activeVoice = findBuiltinVoice(val.c_str());
          // Built-in voices are frame multipliers, not YAML profiles
          if (activeVoice)
            nvspFrontend_setVoiceProfile(fe, "");
          else if (!val.empty())
            nvspFrontend_setVoiceProfile(fe, val.c_str());
        }
        else if (key == "language") {
          // SD sends "c", "C", or "NULL" for unset language — keep default
          if (val != "c" && val != "C" && val != "NULL" && !val.empty()) {
            language = val;
            nvspFrontend_setLanguage(fe, val.c_str());
            espeak.setVoice(val.c_str());
          }
        }
      };

      if (cmd.size() > 4 && cmd[3] == ' ') {
        // Single-line: "SET SELF RATE 50" — parse inline
        std::string rest = cmd.substr(4);
        auto sp1 = rest.find(' ');
        if (sp1 != std::string::npos) {
          rest = rest.substr(sp1 + 1);  // skip scope
          auto sp2 = rest.find(' ');
          if (sp2 != std::string::npos)
            applyParam(rest.substr(0, sp2), rest.substr(sp2 + 1));
        }
      } else {
        // Multi-line block: ack, read KEY=val lines until "."
        sd_send("202 OK RECEIVING MESSAGE");
        while (true) {
          std::string line = sd_readline();
          if (line == "." || (line.empty() && g_eof)) break;
          auto eq = line.find('=');
          if (eq != std::string::npos)
            applyParam(line.substr(0, eq), line.substr(eq + 1));
        }
      }
      sd_send("203 OK SETTINGS RECEIVED");
    }
    else if (cmd == "LIST VOICES") {
      std::lock_guard<std::mutex> lock(g_outMtx);
      // List built-in voices for all supported languages
      char* langList = nvspFrontend_getAvailableLanguages(fe);
      std::vector<std::string> langs;
      if (langList && langList[0]) {
        // langList is newline-separated
        char* p = langList;
        while (*p) {
          char* nl = strchr(p, '\n');
          if (nl) { langs.emplace_back(p, nl); p = nl + 1; }
          else { langs.emplace_back(p); break; }
        }
        nvspFrontend_freeString(langList);
      }
      if (langs.empty()) langs.push_back("en");

      // Filter out root language tags that have region-specific children.
      // e.g. "en" is excluded if "en-us" or "en-gb" exist; "es" if "es-mx" exists.
      std::vector<std::string> filtered;
      for (const auto& lang : langs) {
        if (lang.find('-') == std::string::npos) {
          // Root tag — check if any child exists
          bool hasChild = false;
          std::string prefix = lang + "-";
          for (const auto& other : langs) {
            if (other.compare(0, prefix.size(), prefix) == 0) {
              hasChild = true; break;
            }
          }
          if (hasChild) continue;  // skip root, children cover it
        }
        filtered.push_back(lang);
      }

      // Sort with en-us first — Orca picks the language from the first voice
      // entry when switching voices, so the default must be sensible.
      std::sort(filtered.begin(), filtered.end(), [](const std::string& a, const std::string& b) {
        if (a == "en-us") return true;
        if (b == "en-us") return false;
        return a < b;
      });

      for (const auto& lang : filtered) {
        // Uppercase dialect portion to match Orca's locale format.
        // Orca prepends a default voice with locale-derived "en-US" — if we
        // output "en-us", Orca shows two separate language entries.
        std::string displayLang = lang;
        auto dash = displayLang.find('-');
        if (dash != std::string::npos) {
          for (size_t i = dash + 1; i < displayLang.size(); i++)
            displayLang[i] = (char)toupper((unsigned char)displayLang[i]);
        }
        for (int i = 0; i < kNumVoices; i++) {
          fprintf(stdout, "200-%s\t%s\tMALE%d\n",
                  kBuiltinVoices[i].name, displayLang.c_str(), (i % 3) + 1);
        }
      }
      fprintf(stdout, "249 OK VOICES LISTED\n");
      fflush(stdout);
    }
    else if (cmd == "QUIT") {
      break;
    }
    else if (cmd == "AUDIO" || cmd == "LOGLEVEL" ||
             cmd == "DEBUG" || cmd == "OUTPUT") {
      // Multi-line block: ack → read params until "." → confirm
      sd_send("202 OK RECEIVING MESSAGE");
      while (true) {
        std::string line = sd_readline();
        if (line == "." || (line.empty() && g_eof)) break;
      }
      sd_send("203 OK SETTINGS RECEIVED");
    }
    else {
      // Unknown command — respond with error so SD doesn't hang waiting
      dbg("UNKNOWN: %s", cmd.c_str());
      sd_send("300 ERR UNKNOWN COMMAND");
    }
  }

  // Cleanup
  nvspFrontend_destroy(fe);
  speechPlayer_terminate(player);
  espeak.close();

  return 0;
}
