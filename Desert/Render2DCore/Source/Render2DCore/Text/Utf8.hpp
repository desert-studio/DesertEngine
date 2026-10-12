#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Minimal UTF-8 decoding for text layout. Every string the engine carries (component fields, scene
// files, typed input) is UTF-8, but glyph lookup is by CODEPOINT — walking a std::string byte by byte
// turns "Привет" into twelve broken glyphs. Pure stdlib so it unit-tests next to FontBaker.
namespace Desert::Text
{
    // U+FFFD REPLACEMENT CHARACTER — what malformed input decodes to, so bad bytes degrade to a visible
    // box instead of desynchronising the rest of the string.
    inline constexpr uint32_t kReplacementChar = 0xFFFDu;

    // Decodes the codepoint starting at `i` and advances `i` past it. Always makes progress (an invalid
    // lead or a truncated sequence consumes one byte and yields kReplacementChar), so callers can loop
    // on `i < s.size()` without any risk of hanging on malformed input.
    uint32_t Utf8Next( const std::string& s, size_t& i );

    // The whole string as codepoints.
    std::vector<uint32_t> Utf8Decode( const std::string& s );

    // Appends one codepoint as UTF-8 (used when a decoded string has to be rebuilt).
    void Utf8Append( std::string& s, uint32_t codepoint );

    // Removes the last codepoint (trailing continuation bytes + the lead byte) — a backspace that
    // doesn't leave half a character behind.
    void Utf8PopBack( std::string& s );
} // namespace Desert::Text
