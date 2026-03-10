/*
TGSpeechBox — Generic data query layer implementation.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include "data_query.h"
#include "pack.h"     // getEffectiveSettings (returns "key\tvalue\n" string)
#include "yaml_min.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <string>

// Forward declaration of internal pack.cpp helper.
namespace nvsp_frontend {
std::string getEffectiveSettings(const std::string& packDir,
                                 const std::string& langTag);
}

namespace tgsb_data {

// ── Type detection ──────────────────────────────────────────────────

FieldType detectType(const std::string& value) {
  if (value == "true" || value == "false") return FieldType::Bool;

  // Try parsing as a number.
  if (!value.empty()) {
    char* end = nullptr;
    errno = 0;
    (void)std::strtod(value.c_str(), &end);
    if (errno == 0 && end != value.c_str() && *end == '\0') {
      return FieldType::Float;
    }
  }
  return FieldType::String;
}

// ── Group extraction ────────────────────────────────────────────────

std::string extractGroup(const std::string& key) {
  auto dot = key.find('.');
  if (dot == std::string::npos) return {};
  return key.substr(0, dot);
}

// ── Settings cache builder ──────────────────────────────────────────

void buildSettingsCache(DataCache& cache,
                        const std::string& packDir,
                        const std::string& langTag) {
  cache.settings.clear();
  cache.langTag = langTag;

  // Reuse existing getEffectiveSettings which reads the YAML file chain
  // and returns flattened "key\tvalue\n" lines.
  std::string raw = nvsp_frontend::getEffectiveSettings(packDir, langTag);
  if (raw.empty()) {
    cache.settingsValid = true;
    return;
  }

  // Parse the tab-separated lines into typed records.
  const char* p = raw.c_str();
  const char* end = p + raw.size();

  while (p < end) {
    // Find tab separator.
    const char* tab = p;
    while (tab < end && *tab != '\t') ++tab;
    if (tab >= end) break;

    // Find newline.
    const char* nl = tab + 1;
    while (nl < end && *nl != '\n') ++nl;

    std::string key(p, tab);
    std::string value(tab + 1, nl);

    SettingRecord rec;
    rec.key   = std::move(key);
    rec.value = std::move(value);
    rec.type  = detectType(rec.value);
    rec.group = extractGroup(rec.key);
    cache.settings.push_back(std::move(rec));

    p = (nl < end) ? nl + 1 : end;
  }

  cache.settingsValid = true;
}

// ── JSON helpers ────────────────────────────────────────────────────

// Escape a string for JSON output (handles quotes, backslashes, control chars).
static void jsonEscapeAppend(std::string& out, const std::string& s) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

static const char* fieldTypeName(FieldType t) {
  switch (t) {
    case FieldType::Float:  return "float";
    case FieldType::Bool:   return "bool";
    case FieldType::String: return "string";
  }
  return "string";
}

// ── Settings JSON serializer ────────────────────────────────────────

std::string serializeSettingsJson(const DataCache& cache, int offset, int limit) {
  const auto& vec = cache.settings;
  const int total = static_cast<int>(vec.size());

  // Clamp offset.
  if (offset < 0) offset = 0;
  if (offset >= total) return "[]";

  // Determine end index.
  int endIdx = total;
  if (limit > 0 && offset + limit < total) {
    endIdx = offset + limit;
  }

  std::string out;
  out.reserve((endIdx - offset) * 120);  // rough estimate
  out += '[';

  bool first = true;
  for (int i = offset; i < endIdx; ++i) {
    const auto& rec = vec[i];

    if (!first) out += ',';
    first = false;

    out += '{';

    // "key": "..."
    out += "\"key\":";
    jsonEscapeAppend(out, rec.key);

    // "type": "float"|"bool"|"string"
    out += ",\"type\":\"";
    out += fieldTypeName(rec.type);
    out += '"';

    // "value": ...  (typed: number/bool unquoted, string quoted)
    out += ",\"value\":";
    if (rec.type == FieldType::Bool) {
      out += rec.value;  // "true" or "false" — valid JSON literals
    } else if (rec.type == FieldType::Float) {
      out += rec.value;  // numeric literal
    } else {
      jsonEscapeAppend(out, rec.value);
    }

    // "group": "..."
    out += ",\"group\":";
    jsonEscapeAppend(out, rec.group);

    out += '}';
  }

  out += ']';
  return out;
}

} // namespace tgsb_data
