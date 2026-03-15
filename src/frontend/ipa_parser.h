/*
TGSpeechBox — IPA tokenizer / parser: IPA text -> Token vector.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSB_FRONTEND_IPA_PARSER_H
#define TGSB_FRONTEND_IPA_PARSER_H

#include <string>
#include <vector>

#include "pack.h"
#include "ipa_engine.h"

namespace nvsp_frontend {
namespace internal {

// Tokenize a normalized IPA u32 string into a vector of Tokens.
// Handles greedy phoneme matching, stress marks, syllable boundaries,
// tone markers, stop closure gaps, post-stop aspiration, etc.
bool parseToTokens(const PackSet& pack, const std::u32string& text,
                   std::vector<Token>& outTokens, std::string& outError);

// Auto-tie adjacent vowel pairs as diphthongs when the pack has
// autoTieDiphthongs enabled and the second vowel is an offglide candidate.
void autoTieDiphthongs(const PackSet& pack, std::vector<Token>& tokens);

// Handle spelling-mode diphthong treatment (e.g. monophthongize letter-name
// diphthongs in acronyms/initialisms like "NASA").
void applySpellingDiphthongMode(const PackSet& pack, std::vector<Token>& tokens);

} // namespace internal
} // namespace nvsp_frontend

#endif
