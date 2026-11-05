#pragma once

#include <filesystem>
#include <string>

// A path as UTF-8 text, for a log line or a std::string field. path::string() converts to the ANSI code page on
// Windows and THROWS std::system_error for a character that page cannot hold (a Cyrillic folder on a Western
// install); u8string() encodes every well-formed name. The char8_t-to-char copy lives here once.
namespace Common
{
    [[nodiscard]] inline std::string PathUtf8( const std::filesystem::path& path )
    {
        const std::u8string text = path.u8string();
        return std::string( text.begin(), text.end() );
    }
} // namespace Common
