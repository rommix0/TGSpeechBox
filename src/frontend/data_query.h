/*
TGSpeechBox — Generic data query layer.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.

Provides typed, paginated access to pack settings, phoneme data, and
(future) dictionary data without re-reading YAML from disk on every call.
*/

#ifndef TGSB_DATA_QUERY_H
#define TGSB_DATA_QUERY_H

#include <string>
#include <vector>

// Forward declarations — avoid pulling in pack.h.
namespace nvsp_frontend {
struct LanguagePack;
}

namespace tgsb_data {

// ── Domain IDs (must match NVSP_DATA_* defines in nvspFrontend.h) ──
constexpr int kDomainSettings   = 0;
constexpr int kDomainPhonemes   = 1;
constexpr int kDomainDictionary = 2;  // future

// ── Field types ──
enum class FieldType { Float, Bool, String };

// ── Cached setting record ──
struct SettingRecord {
  std::string key;          // dot-notation key (e.g. "boundarySmoothing.enabled")
  FieldType   type;
  std::string value;        // stringified value
  std::string group;        // first dot-segment, or "" for top-level
};

// ── Cached phoneme record ──
// Each field within a phoneme is a separate record.
// key: "phoneme.fieldName" (e.g. "ɪ.cf2", "h.frameEx.breathiness")
// group: the phoneme key (e.g. "ɪ", "h")
// For the lang-filtered view, mappingFrom is set (e.g. "ɜː" for a "ɜː→ɝː" rule).
struct PhonemeRecord {
  std::string key;          // "phonemeKey.fieldName"
  FieldType   type;
  std::string value;        // stringified value
  std::string group;        // phoneme IPA key
  std::string phonemeClass; // "vowel", "stop", "fricative", etc.
  std::string mappingFrom;  // non-empty only in lang-filtered view
};

// ── Per-domain cache ──
struct DataCache {
  std::string langTag;
  std::vector<SettingRecord> settings;
  bool settingsValid = false;

  std::string phonemesLangTag;  // "" for all, lang tag for filtered
  std::vector<PhonemeRecord> phonemes;
  bool phonemesValid = false;

  void invalidate() {
    settingsValid = false;
    phonemesValid = false;
  }
};

// ── Cache builders ──

// Build settings cache from the pack YAML file chain.
// packDir: root dir containing packs/.  langTag: e.g. "en-us".
void buildSettingsCache(DataCache& cache,
                        const std::string& packDir,
                        const std::string& langTag);

// Build phonemes cache from phonemes.yaml.
// If langTag is empty: all phonemes from base phonemes.yaml.
// If langTag is non-empty: only phonemes referenced as replacement targets
// in that language's normalization.replacements, with mappingFrom populated.
void buildPhonemesCache(DataCache& cache,
                        const std::string& packDir,
                        const std::string& langTag);

// ── JSON serializers ──

// Serialize a slice of the settings cache to a JSON array string.
// offset/limit: pagination (limit=0 means all from offset).
std::string serializeSettingsJson(const DataCache& cache, int offset, int limit);

// Serialize a slice of the phonemes cache to a JSON array string.
std::string serializePhonemesJson(const DataCache& cache, int offset, int limit);

// ── Type detection ──

// Infer FieldType from a scalar value string.
FieldType detectType(const std::string& value);

// Extract the group prefix from a dot-notation key.
// "boundarySmoothing.enabled" → "boundarySmoothing"
// "primaryStressDiv"          → ""
std::string extractGroup(const std::string& key);

} // namespace tgsb_data

#endif // TGSB_DATA_QUERY_H
