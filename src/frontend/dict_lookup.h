/*
TGSpeechBox — Pronunciation dictionary lookup.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSB_FRONTEND_DICT_LOOKUP_H
#define TGSB_FRONTEND_DICT_LOOKUP_H

#include "pack.h"

#include <string>
#include <unordered_map>

namespace nvsp_frontend {

// Replace words in text using the pronunciation dictionary.
// Case-insensitive lookup, preserves surrounding punctuation.
// Hyphens in replacement text are converted to spaces for eSpeak
// consistency between isolation and phrase context.
// Masked entries are skipped.
//
// If ipaOverrides is non-null, words whose dict entry has a non-empty
// toIpa are NOT replaced in the text (left for eSpeak to phonemize
// naturally).  Instead, their lowercase key and toIpa value are
// recorded in the map for downstream IPA splicing.
std::string dictReplaceInText(const std::string& text, const PronDict& dict,
    std::unordered_map<std::string, std::string>* ipaOverrides = nullptr);

}  // namespace nvsp_frontend

#endif  // TGSB_FRONTEND_DICT_LOOKUP_H
