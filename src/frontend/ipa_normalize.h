/*
TGSpeechBox — IPA normalization: raw IPA text -> cleaned u32 string.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSB_FRONTEND_IPA_NORMALIZE_H
#define TGSB_FRONTEND_IPA_NORMALIZE_H

#include <string>

#include "pack.h"

namespace nvsp_frontend {
namespace internal {

// Normalize raw UTF-8 IPA text through the full cleanup pipeline:
//   - PUA-A stripping
//   - tie bar normalization
//   - eSpeak separator stripping
//   - preReplacements (with cross-phase protection)
//   - ZWJ/ZWNJ removal, tag stripping, wrapper punctuation removal
//   - stress/length marker normalization
//   - syllabic consonant normalization
//   - allophone digit stripping
//   - whitespace collapsing
//   - aliases and replacements
//   - unescape protected characters
//
// Returns the cleaned u32 string ready for tokenization.
std::u32string normalizeIpaText(const PackSet& pack, const std::string& ipaUtf8);

} // namespace internal
} // namespace nvsp_frontend

#endif
