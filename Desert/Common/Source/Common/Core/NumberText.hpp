#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>

namespace Common
{
    // A FLOATING NUMBER READ FROM TEXT, AS A VALUE. std::stof / std::stod throw on a text that is not a number,
    // and engine code does not catch (UE: FCString::Atof / LexTryParseString answer, they never throw). These
    // are the forms that answer: nullopt exactly where std::stof / std::stod throw - no number at the start of
    // the text, or one out of the type's range (ERANGE) - and otherwise the value, with the count of characters
    // read in `used` (std::stof's `idx`), so a caller that needs the whole text to be the number compares it
    // to text.size().
    //
    // They are std::strtof / std::strtod, which is what std::stof / std::stod call: the same leading-whitespace
    // rule and the same LC_NUMERIC decimal point, so a caller moved here reads exactly what it read before.
    // (std::from_chars would be locale-free, but Apple clang 15's libc++ has no floating from_chars - see
    // ParseSeconds in Runtime/Source/MovieRender.hpp.) Integers have std::from_chars on every platform.
    [[nodiscard]] inline std::optional<float> FloatFromText( const std::string& text,
                                                             std::size_t*       used = nullptr ) noexcept
    {
        const char* const begin = text.c_str();
        char*             end   = nullptr;
        errno                   = 0;
        const float value       = std::strtof( begin, &end );
        if ( end == begin || errno == ERANGE )
            return std::nullopt;
        if ( used != nullptr )
            *used = static_cast<std::size_t>( end - begin );
        return value;
    }

    [[nodiscard]] inline std::optional<double> DoubleFromText( const std::string& text,
                                                               std::size_t*       used = nullptr ) noexcept
    {
        const char* const begin = text.c_str();
        char*             end   = nullptr;
        errno                   = 0;
        const double value      = std::strtod( begin, &end );
        if ( end == begin || errno == ERANGE )
            return std::nullopt;
        if ( used != nullptr )
            *used = static_cast<std::size_t>( end - begin );
        return value;
    }
} // namespace Common
