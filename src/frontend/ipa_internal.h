/*
TGSpeechBox — Shared IPA helpers used by both parser and normalization modules.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSB_FRONTEND_IPA_INTERNAL_H
#define TGSB_FRONTEND_IPA_INTERNAL_H

#include <string>
#include "pack.h"
#include "ipa_engine.h"

namespace nvsp_frontend {
namespace ipa_internal {

// True if c is an IPA tie bar (U+0361 or U+035C).
inline bool isTieBar(char32_t c) {
  return c == U'\u0361' || c == U'\u035C';
}

// Look up a phoneme definition by its u32 key.
inline const PhonemeDef* findPhoneme(const PackSet& pack, const std::u32string& key) {
  auto it = pack.phonemes.find(key);
  if (it == pack.phonemes.end()) return nullptr;
  return &it->second;
}

// Token classification helpers shared between ipa_engine.cpp and ipa_parser.cpp.
inline bool tokenIsVoiced(const Token& t) {
  return t.def && ((t.def->flags & kIsVoiced) != 0);
}

inline bool tokenIsStop(const Token& t) {
  return t.def && ((t.def->flags & kIsStop) != 0);
}

inline bool tokenIsAfricate(const Token& t) {
  return t.def && ((t.def->flags & kIsAfricate) != 0);
}

inline bool tokenIsTap(const Token& t) {
  return t.def && ((t.def->flags & kIsTap) != 0);
}

inline bool tokenIsNasal(const Token& t) {
  return t.def && ((t.def->flags & kIsNasal) != 0);
}

inline double getFieldOrZero(const Token& t, FieldId id) {
  int idx = static_cast<int>(id);
  if ((t.setMask & (1ull << idx)) == 0) return 0.0;
  return t.field[idx];
}

inline bool tokenIsFricativeLike(const Token& t) {
  // Mirrors ipa_convert.py: fricationAmplitude > 0.05
  return getFieldOrZero(t, FieldId::fricationAmplitude) > 0.05;
}

} // namespace ipa_internal
} // namespace nvsp_frontend

#endif
