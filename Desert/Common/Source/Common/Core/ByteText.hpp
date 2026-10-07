#pragma once

#include <cstdint>
#include <span>
#include <string_view>

// A text document travels as bytes (a pak block, a ReadSequence argument) and is parsed as text: this is the
// one place the two views of the same storage meet. char and uint8_t (unsigned char) may alias any object, so
// viewing one as the other is well-defined; the cast lives here once instead of at every reader.
namespace Common
{
    /// The bytes of a text document, viewed as its text (no copy).
    [[nodiscard]] inline std::string_view TextOf( std::span<const uint8_t> bytes ) noexcept
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): uint8_t and char may alias (see above)
        return { reinterpret_cast<const char*>( bytes.data() ), bytes.size() };
    }

    /// The text of a document, viewed as its bytes (no copy).
    [[nodiscard]] inline std::span<const uint8_t> BytesOf( std::string_view text ) noexcept
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): uint8_t and char may alias (see above)
        return { reinterpret_cast<const uint8_t*>( text.data() ), text.size() };
    }
} // namespace Common
