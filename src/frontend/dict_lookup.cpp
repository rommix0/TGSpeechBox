/*
TGSpeechBox — Pronunciation dictionary lookup implementation.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "dict_lookup.h"

#include <cctype>
#include <cstdint>
#include <string>

// Debug logging (shares TPARSER_DEBUG_LOG toggle from text_parser.cpp).
#if 0
#include <cstdio>
#include <cstdlib>
static FILE* dlLogFile() {
  static FILE* f = nullptr;
  if (!f) {
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = "/tmp";
    std::string path = std::string(tmp) + "/tgsb_dict_lookup.log";
    f = std::fopen(path.c_str(), "a");
  }
  return f;
}
#define DLLOG(...) do { FILE* _f = dlLogFile(); if (_f) { std::fprintf(_f, __VA_ARGS__); std::fflush(_f); } } while(0)
#else
#define DLLOG(...) ((void)0)
#endif

namespace nvsp_frontend {

// Strip Unicode variation selectors (U+FE0E text, U+FE0F emoji) from a
// UTF-8 string.  iOS often appends U+FE0F when copying emoji, which
// prevents dictionary lookup from matching the bare codepoint.
// U+FE0E = EF B8 8E, U+FE0F = EF B8 8F in UTF-8.
static std::string stripVariationSelectors(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (i + 2 < s.size() &&
        static_cast<uint8_t>(s[i])   == 0xEF &&
        static_cast<uint8_t>(s[i+1]) == 0xB8 &&
        (static_cast<uint8_t>(s[i+2]) == 0x8E ||
         static_cast<uint8_t>(s[i+2]) == 0x8F)) {
      i += 2;  // skip 3-byte sequence
      continue;
    }
    out.push_back(s[i]);
  }
  return out;
}

std::string dictReplaceInText(const std::string& text, const PronDict& dict,
    std::unordered_map<std::string, std::string>* ipaOverrides) {
  if (text.empty() || dict.entries.empty()) return text;

  std::string result;
  result.reserve(text.size() + 32);

  size_t i = 0;
  while (i < text.size()) {
    // Skip whitespace.
    if (text[i] == ' ' || text[i] == '\t' ||
        text[i] == '\n' || text[i] == '\r') {
      result.push_back(text[i]);
      ++i;
      continue;
    }

    // Extract word token.
    size_t wordStart = i;
    while (i < text.size() && text[i] != ' ' && text[i] != '\t' &&
           text[i] != '\n' && text[i] != '\r') {
      ++i;
    }
    std::string token = text.substr(wordStart, i - wordStart);

    // Strip leading/trailing punctuation for lookup.
    size_t lo = 0;
    while (lo < token.size() &&
           std::ispunct(static_cast<unsigned char>(token[lo])))
      ++lo;
    size_t hi = token.size();
    while (hi > lo &&
           std::ispunct(static_cast<unsigned char>(token[hi - 1])))
      --hi;

    if (lo >= hi) {
      result += token;
      continue;
    }

    // Case-sensitive lookup: try exact case first (e.g., "Parton" as proper
    // noun), then fallback to lowercase (e.g., "parton" as common word).
    // This lets users have different pronunciations for capitalized vs
    // lowercase variants of the same word.
    // Strip variation selectors so "🦯\uFE0F" matches dict key "🦯".
    std::string exactKey = stripVariationSelectors(
        std::string(token, lo, hi - lo));
    std::string lowerKey;
    lowerKey.reserve(hi - lo);
    for (size_t k = lo; k < hi; ++k)
      lowerKey.push_back(static_cast<char>(
          std::tolower(static_cast<unsigned char>(token[k]))));

    auto it = dict.entries.find(exactKey);
    if (it == dict.entries.end() || it->second.masked) {
      // Fallback to lowercase.
      it = dict.entries.find(lowerKey);
      if (it == dict.entries.end() || it->second.masked) {
        result += token;
        continue;
      }
    }

    // If this entry has a toIpa override, don't replace the text — leave
    // the original word so eSpeak phonemizes it naturally.  Record the
    // override for downstream IPA splicing in runTextParser.
    if (ipaOverrides && !it->second.toIpa.empty()) {
      DLLOG("  dictIpaOverride: \"%s\" -> ipa \"%s\"\n",
            lowerKey.c_str(), it->second.toIpa.c_str());
      (*ipaOverrides)[lowerKey] = it->second.toIpa;
      result += token;  // keep original word
      continue;
    }

    // Convert hyphens to spaces in toText — users write them as pronunciation
    // guides ("shi-kah-go") but eSpeak treats hyphens inconsistently as word
    // boundaries in isolation vs phrase context.  Converting to spaces makes
    // "shi-kah-go" and "shi kah go" produce identical results.
    std::string replacement;
    replacement.reserve(it->second.toText.size());
    for (char c : it->second.toText) {
      replacement += (c == '-') ? ' ' : c;
    }

    DLLOG("  dictReplace: \"%s\" -> \"%s\"\n",
          it->second.fromText.c_str(), replacement.c_str());

    // Replace core with cleaned toText, preserving surrounding punctuation.
    result += token.substr(0, lo);
    result += replacement;
    result += token.substr(hi);
  }

  return result;
}

}  // namespace nvsp_frontend
