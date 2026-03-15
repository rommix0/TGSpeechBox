/*
TGSpeechBox — UTF-8 utility function declarations.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#ifndef TGSB_FRONTEND_UTF8_H
#define TGSB_FRONTEND_UTF8_H

#include <string>
#include <string_view>

namespace nvsp_frontend {

// Best-effort UTF-8 -> UTF-32. Invalid sequences become U+FFFD.
std::u32string utf8ToU32(std::string_view s);

// UTF-32 -> UTF-8.
std::string u32ToUtf8(std::u32string_view s);

// Lowercase ASCII and convert '_' -> '-' (for language tags).
std::string normalizeLangTag(std::string_view tag);

// Strip invisible/zero-width Unicode characters (ZWS, ZWNJ, ZWJ, BOM,
// soft hyphen, bidi controls, etc.).
std::string stripInvisible(std::string_view s);

// Normalize UTF-8 string to NFKC form.
// Windows: NormalizeString (Vista+). Apple: CFStringNormalize.
// Other platforms: identity (normalize at the Java/Python/Swift layer).
std::string normalizeNFKC(const std::string& s);

// Convenience: stripInvisible + normalizeNFKC in one call.
std::string normalizeText(const std::string& s);

} // namespace nvsp_frontend

#endif
