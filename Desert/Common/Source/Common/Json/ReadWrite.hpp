#pragma once

#include <Common/Json/Json.hpp>

#include <rflcpp/rfl/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

// THE TEXT HALF OF THE ONE DOOR (BUILD-MOD-4): Json.hpp declares what a JSON struct IS (the marks, the value
// types, the project options); this header turns one into text and back. It is the only header that includes
// rfl::json, whose yyjson reader/writer and full reflect-cpp parser cost every file that names it ~0.4 s of parse
// - six hundred files named it through Json.hpp, most of them only to declare a struct. A file that calls
// Read/Write includes this, the way UE keeps FJsonSerializer apart from the FJsonObject a header can hold.
namespace Common::Json
{
    // NO CATCH: rfl is used through its Result API only. rfl::json::read answers a text that is not JSON with
    // an error, and its Parser::read is noexcept - it turns a Reflector's or a custom constructor's throw into
    // an rfl::Error itself (rfl/parsing/Parser_default.hpp). What a `catch ( const std::exception& )` here
    // could still see is bad_alloc, which is not a read error to report but the end of the process.
    template <typename T>
    ResultStr<T> Read( std::string_view json ) noexcept
    {
        auto parsed = [&]
        {
            if constexpr ( IsPartial<T> )
                return rfl::json::read<T, rfl::DefaultIfMissing>( json );
            else
                return rfl::json::read<T, rfl::DefaultIfMissing, rfl::NoExtraFields>( json );
        }();
        if ( !parsed )
            return MakeError<T>( Detail::DescribeReadError( parsed.error().what() ) );
        return MakeSuccess( std::move( parsed.value() ) );
    }

    template <typename T>
    std::string Write( const T& value )
    {
        return rfl::json::write( value );
    }

    template <typename T>
    ResultStr<T> ReadFile( const std::filesystem::path& file )
    {
        const auto text = Detail::ReadFileText( file );
        if ( !text )
            return MakeError<T>( text.GetError() );
        auto parsed = Read<T>( text.GetValue() );
        if ( !parsed )
            return MakeError<T>( file.string() + ": " + parsed.GetError() );
        return parsed;
    }

    // Canonical layout, then the atomic replace (temporary file renamed over the target): a failure refuses
    // with the file named and leaves the file on disk as it was.
    template <typename T>
    BoolResultStr WriteFileAtomic( const std::filesystem::path& file, const T& value )
    {
        return Detail::WriteTextAtomic( file, Write( value ) );
    }
} // namespace Common::Json
