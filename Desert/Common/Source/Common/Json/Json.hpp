#pragma once

#include <Common/Core/ResultStr.hpp>

#include <rflcpp/rfl/DefaultIfMissing.hpp>
#include <rflcpp/rfl/ExtraFields.hpp>
#include <rflcpp/rfl/Generic.hpp>
#include <rflcpp/rfl/NoExtraFields.hpp>
#include <rflcpp/rfl/json.hpp>
#include <rflcpp/rfl/named_tuple_t.hpp>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// THE ONE DOOR BETWEEN A STRUCT AND JSON (JS1a, owner decision 2026-09-25). Every JSON protocol of the
// project maps a plain struct to text through compile-time reflection (reflect-cpp); this header is the only
// place that names rfl::json, so the options every file is read with live in ONE spot instead of being
// re-chosen at each of a hundred call sites. The JsonCensus suite refuses rfl::json / rfl::Generic / yyjson_
// anywhere else that is not a migrator or a named row of its exception register.
//
// THE PROJECT OPTIONS, and why (lead decision on JS1a2: a missing config or datum is an error, not a fallback):
//   - STRICT BY DEFAULT. A field MISSING from the text is an error naming its path, and so is a key the struct
//     does not declare (rfl::NoExtraFields). A value that may legitimately be absent says so in its own type
//     (std::optional); nothing else is ever filled in behind the reader's back. A format that changes shape
//     moves its files forward with a migration, it does not lean on defaults;
//   - a struct that must carry keys it does not know (a file shared with other builds) declares a CarriedKeys
//     member: those keys are captured instead of refused and written back on save;
//   - LENIENT ONLY BY NAME. DESERT_JSON_LENIENT( Type, "reason" ) lets a missing field take its in-struct
//     default and an unknown key pass, for a file several builds write at once (a settings file shared by
//     every worktree of the machine), where a key one build lacks is not damage. The reason is mandatory and
//     compiled in, so every lenient type answers "why" in its own header;
//   - members are written in declaration order (rfl's only order), compact; a file on disk goes through the
//     canonical layout (Content/CanonicalText.hpp) so an edit to one field is a diff of one line.
//
// ERRORS: every failure is a returned ResultStr, never an exception (rfl's internal throws are caught here),
// and it names the FIELD PATH ("inner.list.a") instead of rfl's nested "Failed to parse field ..." chain; the
// *File functions also name the file.
namespace Common::Json
{
    // What DESERT_JSON_STRUCT binds to a type: the format's name and the version its files carry.
    struct FormatInfo
    {
        std::string_view Name;
        std::uint32_t    Version = 0;
    };

    // A member of type CarriedKeys holds every key of the object the struct does not declare, and writes them
    // back flat at the struct's own level: a file shared between builds keeps the settings of the other build
    // instead of losing them on the first save of this one.
    using CarriedKeys = rfl::ExtraFields<rfl::Generic>;

    namespace Detail
    {
        template <typename Values>
        struct HasNoRawPointer;

        template <typename... Ts>
        struct HasNoRawPointer<rfl::Tuple<Ts...>>
             : std::bool_constant<( !std::is_pointer_v<std::remove_cvref_t<Ts>> && ... )>
        {
        };

        // "Failed to parse field 'a': Failed to parse field 'b': Could not cast to int." ->
        // "field 'a.b': Could not cast to int." Non-template so the parsing is compiled once.
        std::string DescribeReadError( std::string_view rflMessage );

        ResultStr<std::string> ReadFileText( const std::filesystem::path& file );
        BoolResultStr          WriteTextAtomic( const std::filesystem::path& file, const std::string& json );
    } // namespace Detail

    // What compile-time reflection can read: an aggregate (so no private data members, no virtuals, no
    // user constructors hiding the fields) with no raw pointer member (a pointer has no JSON meaning).
    namespace Detail
    {
        // Deferred behind std::conjunction: rfl::named_tuple_t of a NON-aggregate does not compile, so it
        // must only be looked at once the aggregate test has passed.
        template <typename T>
        struct HasNoRawPointerMember : HasNoRawPointer<typename rfl::named_tuple_t<T>::Values>
        {
        };
    } // namespace Detail

    template <typename T>
    inline constexpr bool IsReflectable =
         std::conjunction_v<std::is_aggregate<T>, Detail::HasNoRawPointerMember<T>>;

    // The format bound to T by DESERT_JSON_STRUCT, found by argument-dependent lookup in T's namespace.
    template <typename T>
    concept HasFormat = requires { DesertJsonFormatOf( static_cast<const T*>( nullptr ) ); };

    template <HasFormat T>
    inline constexpr FormatInfo FormatOf = DesertJsonFormatOf( static_cast<const T*>( nullptr ) );

    // True only for a type marked DESERT_JSON_LENIENT (found by argument-dependent lookup in T's namespace).
    template <typename T>
    concept IsLenient = requires { DesertJsonLenientReason( static_cast<const T*>( nullptr ) ); };

    template <IsLenient T>
    inline constexpr std::string_view LenientReason = DesertJsonLenientReason( static_cast<const T*>( nullptr ) );

    template <typename T>
    ResultStr<T> Read( std::string_view json )
    {
        try
        {
            auto parsed = [&]
            {
                if constexpr ( IsLenient<T> )
                    return rfl::json::read<T, rfl::DefaultIfMissing>( json );
                else
                    return rfl::json::read<T, rfl::NoExtraFields>( json );
            }();
            if ( !parsed )
                return MakeError<T>( Detail::DescribeReadError( parsed.error().what() ) );
            return MakeSuccess( std::move( parsed.value() ) );
        }
        catch ( const std::exception& e )
        {
            return MakeError<T>( Detail::DescribeReadError( e.what() ) );
        }
    }

    template <typename T>
    std::string Write( const T& value )
    {
        return rfl::json::write( value );
    }

    // The top-level members of a JSON object in file order, each value re-written compactly — for a caller
    // that has to compare two documents key by key without knowing their struct (which settings changed,
    // which keys another build added).
    ResultStr<std::vector<std::pair<std::string, std::string>>> ObjectMembers( std::string_view json );

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

// A MARK, NOT A SCHEMA (owner decision 2026-09-25: functions for reading and writing, no field-listing
// macros). Put it right after the struct, in the struct's own namespace. It proves at compile time that the
// type is readable by reflection, binds a format name and version to it (Common::Json::FormatOf<Type>) and
// is counted by the JsonCensus suite, which also refuses two types claiming one format name.
#define DESERT_JSON_STRUCT( Type, FormatName, FormatVersion )                                                     \
    static_assert( ::Common::Json::IsReflectable<Type>,                                                           \
                   #Type " is not readable by reflection: it must be an aggregate with no raw pointer member" );  \
    [[maybe_unused]] constexpr ::Common::Json::FormatInfo DesertJsonFormatOf( const Type* )                       \
    {                                                                                                             \
        return ::Common::Json::FormatInfo{ FormatName, FormatVersion };                                           \
    }

// THE ONE WAY OUT OF STRICT READING (see "THE PROJECT OPTIONS" above). Right after the struct, in its own
// namespace. The reason is a sentence a reviewer can check; one shorter than a sentence does not compile.
#define DESERT_JSON_LENIENT( Type, Reason )                                                                       \
    static_assert( std::string_view( Reason ).size() >= 24,                                                       \
                   "DESERT_JSON_LENIENT(" #Type "): the reason must say why a missing field may default" );       \
    [[maybe_unused]] constexpr std::string_view DesertJsonLenientReason( const Type* )                            \
    {                                                                                                             \
        return Reason;                                                                                            \
    }
