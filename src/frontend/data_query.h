/*
TGSpeechBox — Generic data query layer.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.

Provides typed, paginated access to pack settings (and future phoneme /
dictionary data) without re-reading YAML from disk on every call.
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
constexpr int kDomainPhonemes   = 1;  // Phase 1b
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

// ── Per-domain cache ──
struct DataCache {
  std::string langTag;
  std::vector<SettingRecord> settings;
  bool settingsValid = false;

  void invalidate() { settingsValid = false; }
};

// ── Cache builders ──

// Build settings cache from the pack YAML file chain.
// packDir: root dir containing packs/.  langTag: e.g. "en-us".
void buildSettingsCache(DataCache& cache,
                        const std::string& packDir,
                        const std::string& langTag);

// ── JSON serializers ──

// Serialize a slice of the settings cache to a JSON array string.
// offset/limit: pagination (limit=0 means all from offset).
std::string serializeSettingsJson(const DataCache& cache, int offset, int limit);

// ── Type detection ──

// Infer FieldType from a scalar value string.
FieldType detectType(const std::string& value);

// Extract the group prefix from a dot-notation key.
// "boundarySmoothing.enabled" → "boundarySmoothing"
// "primaryStressDiv"          → ""
std::string extractGroup(const std::string& key);

} // namespace tgsb_data

#endif // TGSB_DATA_QUERY_H
