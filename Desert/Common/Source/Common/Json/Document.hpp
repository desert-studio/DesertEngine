#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Common/Json/Json.hpp>

#include <rflcpp/rfl/enums.hpp>
#include <rflcpp/rfl/fields.hpp>
#include <rflcpp/rfl/from_generic.hpp>
#include <rflcpp/rfl/to_generic.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// UNTYPED JSON DOCUMENTS THROUGH THE FACADE (JS1c). Json.hpp maps a struct to text; this header is for the
// documents whose shape only their owners know — a scene's entity records, whose component blocks are keyed by
// the registry, and the world cells cut from them. Json::Value stays the storage; what this header adds is that
// nobody outside Common/Json reads it by calling the library's members (JsonCensus refuses .to_object() and
// friends elsewhere). Reading goes through a Node, a non-owning view that knows WHERE it is
// ("Entities[id=4127].Foliage.Density"), and writing through an ObjectBuilder or FromStruct.
//
// THE WRONG-TYPE RULE (lead decision on JS1c, 2026-09-26) — one rule for every field-mapped reader:
//   - a value of the wrong type is NEVER silent. ReadInto records an Issue carrying the FULL path (entity ->
//     component -> field, when the caller roots the Node at that path) plus what was expected and what was
//     found. The load reports its Issues in its result and logs them as an error (ReportIssues: one grouped
//     line per load, like the foreign-key report);
//   - the field keeps the value it had (its declared default on a fresh component) and loading continues —
//     there is no substituted 0.0, no ignored field;
//   - a Ser-struct block read whole (AsBlock / ReadInto of a struct) is atomic: an issue anywhere inside it
//     drops the WHOLE block — the target keeps what it had — and is reported the same way.
// An ABSENT key is not an issue: "absent = keep" is what lets a field be added without a version bump
// (AuthoredComponentIO.hpp). An UNKNOWN key is refused by As<T> (strict, like Json::Read) unless T declares
// CarriedKeys, and tolerated by AsBlock<T> — inside a component block the document merge carries it.
namespace Common::Json
{
    // Where a value sits in its document, kept as segments and rendered only when something is reported.
    class Path
    {
    public:
        [[nodiscard]] Path Key( std::string_view key ) const;
        [[nodiscard]] Path Index( std::size_t index ) const;
        // An element of an array of records, named by its id instead of its position: "Entities[id=4127]".
        [[nodiscard]] Path Record( std::string_view id ) const;
        // "Entities[id=4127].Foliage.Density"; the empty path renders as "document".
        [[nodiscard]] std::string ToString() const;

    private:
        enum class SegmentKind
        {
            Key,
            Index,
            Record
        };
        struct Segment
        {
            SegmentKind Kind = SegmentKind::Key;
            std::string Text;
        };
        std::vector<Segment> m_Segments;
    };

    enum class Kind
    {
        Null,
        Bool,
        Integer,
        Real,
        String,
        Array,
        Object
    };

    // One value that could not be read: where, what the reader wanted, what the file holds.
    struct Issue
    {
        std::string Path;
        std::string Expected;
        std::string Found;
    };
    using Issues = std::vector<Issue>;

    // "Entities[id=7].Light.Intensity: expected number, found string \"bright\"".
    [[nodiscard]] std::string Describe( const Issue& issue );

    // The one error line a load writes for everything it could not read (nothing is logged for none); the
    // same text is returned so the load result can carry it. `document` names the file or record.
    std::string ReportIssues( const Issues& issues, std::string_view document );

    class Node;

    namespace Detail
    {
        const Value& NullValue();
        std::string  KindName( Kind kind );
        // `string "abc"`, `integer 300`, `object with 2 members` — the Found text of an Issue.
        std::string DescribeFound( const Value& value );
        template <typename T>
        inline constexpr bool IsGlmVec = false;
        template <glm::length_t L, glm::qualifier Q>
        inline constexpr bool IsGlmVec<glm::vec<L, float, Q>> = true;
    } // namespace Detail

    // A non-owning view of one value and its path. It never copies a subtree; the Value it looks at must
    // outlive it (Root refuses a temporary for that reason).
    class Node
    {
    public:
        Node() : m_Value( &Detail::NullValue() )
        {
        }
        Node( const Value& value, Path path ) : m_Value( &value ), m_Path( std::move( path ) )
        {
        }

        [[nodiscard]] Kind        GetKind() const;
        [[nodiscard]] const Path& Where() const
        {
            return m_Path;
        }
        // The stored value itself — for a merge or a write-back that has to keep a subtree as it is.
        [[nodiscard]] const Value& Raw() const
        {
            return *m_Value;
        }

        // Absent, or this is not an object -> nullopt.
        [[nodiscard]] std::optional<Node> Find( std::string_view key ) const;
        // A required member; the error names the path.
        [[nodiscard]] ResultStr<Node> Get( std::string_view key ) const;

        // f( std::string_view name, const Node& member ), in file order. Nothing when this is not an object.
        template <typename F>
        void ForEachMember( F&& f ) const
        {
            if ( const auto* object = std::get_if<Object>( &m_Value->variant() ) )
                for ( const auto& [name, value] : *object )
                    f( std::string_view( name ), Node( value, m_Path.Key( name ) ) );
        }
        // f( std::size_t index, const Node& element ). Nothing when this is not an array.
        template <typename F>
        void ForEachElement( F&& f ) const
        {
            if ( const auto* array = std::get_if<Value::Array>( &m_Value->variant() ) )
                for ( std::size_t i = 0; i < array->size(); ++i )
                    f( i, Node( ( *array )[i], m_Path.Index( i ) ) );
        }

        [[nodiscard]] ResultStr<bool> AsBool() const;
        // An integer or a real.
        [[nodiscard]] ResultStr<double> AsNumber() const;
        // An integer, or a real that is integral and inside int64 ("5.0" is a legitimate spelling of 5; a
        // real at or beyond 2^63 has already lost the integer it claims to be).
        [[nodiscard]] ResultStr<std::int64_t> AsInteger() const;
        [[nodiscard]] ResultStr<std::string>  AsString() const;
        // A DECIMAL STRING: a 64-bit id does not survive JSON's double, so ids are written as text.
        [[nodiscard]] ResultStr<UUID> AsUuid() const;

        // STRICT typed extraction, Json::Read's rules: missing member and unknown key are errors (unless T is
        // DESERT_JSON_LENIENT, or declares CarriedKeys, which captures the unknown keys).
        template <typename T>
        [[nodiscard]] ResultStr<T> As() const
        {
            try
            {
                auto parsed = [&]
                {
                    if constexpr ( IsLenient<T> )
                        return rfl::from_generic<T, rfl::DefaultIfMissing>( *m_Value );
                    else
                        return rfl::from_generic<T, rfl::NoExtraFields>( *m_Value );
                }();
                if ( !parsed )
                    return MakeError<T>( Located( Detail::DescribeReadError( parsed.error().what() ) ) );
                return MakeSuccess( std::move( parsed.value() ) );
            }
            catch ( const std::exception& e )
            {
                return MakeError<T>( Located( Detail::DescribeReadError( e.what() ) ) );
            }
        }

        // A component block read whole: a missing member keeps T's in-struct default, an unknown key is
        // tolerated (the document merge carries it), a wrong-typed member refuses the WHOLE block.
        template <typename T>
        [[nodiscard]] ResultStr<T> AsBlock() const
        {
            try
            {
                auto parsed = rfl::from_generic<T, rfl::DefaultIfMissing>( *m_Value );
                if ( !parsed )
                    return MakeError<T>( Located( Detail::DescribeReadError( parsed.error().what() ) ) );
                return MakeSuccess( std::move( parsed.value() ) );
            }
            catch ( const std::exception& e )
            {
                return MakeError<T>( Located( Detail::DescribeReadError( e.what() ) ) );
            }
        }

        // Member `key` into `out` under THE WRONG-TYPE RULE (top of this header): absent -> `out` untouched;
        // wrong type or out of range -> an Issue with the member's full path, `out` untouched. A struct (or
        // any type without a scalar arm) is read as a block and replaces `out` whole or not at all.
        template <typename T>
        void ReadInto( std::string_view key, T& out, Issues& issues ) const
        {
            if ( GetKind() != Kind::Object )
            {
                issues.push_back( { m_Path.ToString(), "object", Detail::DescribeFound( *m_Value ) } );
                return;
            }
            const auto member = Find( key );
            if ( member )
                member->ReadValue( out, issues );
        }

        // This node itself into `out`, same rule as ReadInto (the element loop of an array uses it).
        template <typename T>
        void ReadValue( T& out, Issues& issues ) const;

        // True when this node is of `kind`; otherwise an Issue naming this path and false — for a reader that
        // walks a container itself (a reflected block, a vector field) rather than reading one member.
        bool ExpectKind( Kind kind, Issues& issues ) const
        {
            if ( GetKind() == kind )
                return true;
            Report( issues, Detail::KindName( kind ) );
            return false;
        }

        // Records that this value is not what the reader expected: an Issue with this path, `expected`, and
        // what the file holds — for a reader whose rule is its own (an enum stored by its integer, an asset
        // reference with several accepted forms).
        void Report( Issues& issues, std::string expected ) const
        {
            issues.push_back( { m_Path.ToString(), std::move( expected ), Detail::DescribeFound( *m_Value ) } );
        }

    private:
        [[nodiscard]] std::string Located( const std::string& message ) const
        {
            return m_Path.ToString() + ": " + message;
        }

        const Value* m_Value;
        Path         m_Path;
    };

    // Parses a whole document; a syntax error names its byte offset.
    [[nodiscard]] ResultStr<Value> Parse( std::string_view json );

    [[nodiscard]] inline Node Root( const Value& value, Path path = {} )
    {
        return { value, std::move( path ) };
    }
    // A Node over a temporary would dangle as soon as the full expression ends.
    Node Root( const Value&& value, Path path = {} ) = delete;

    // A value of T as JSON would spell it — the same text Json::Write( value ) gives. Proven byte-identical
    // per field kind by the JsonDocument suite (risk R2 of the JS1c design): rfl writes a float as its
    // double, an integer as int64/uint64, and the value tree keeps exactly those.
    template <typename T>
    [[nodiscard]] Value FromStruct( const T& value )
    {
        return rfl::to_generic( value );
    }

    namespace Detail
    {
        template <typename T>
        Value ToValue( const T& value )
        {
            using U = std::remove_cvref_t<T>;
            if constexpr ( std::is_same_v<U, Value> )
                return value;
            else if constexpr ( std::is_same_v<U, Object> || std::is_same_v<U, bool> )
                return Value( value );
            else if constexpr ( std::is_enum_v<U> )
                return Value( rfl::enum_to_string( value ) );
            else if constexpr ( std::is_integral_v<U> )
                return Value( static_cast<std::int64_t>( value ) );
            else if constexpr ( std::is_floating_point_v<U> )
                return Value( static_cast<double>( value ) );
            else if constexpr ( std::is_convertible_v<const U&, std::string_view> )
                return Value( std::string( std::string_view( value ) ) );
            else if constexpr ( std::is_same_v<U, UUID> )
                return Value( value.ToString() );
            else if constexpr ( IsGlmVec<U> )
            {
                Value::Array array;
                for ( glm::length_t i = 0; i < U::length(); ++i )
                    array.emplace_back( static_cast<double>( value[i] ) );
                return { std::move( array ) };
            }
            else
                return FromStruct( value );
        }
    } // namespace Detail

    // Builds an object member by member. Insertion order is write order (the canonical layout keeps it); a
    // key set twice keeps its first position and takes the last value.
    class ObjectBuilder
    {
    public:
        // bool, integer, real, string, enum (by name), UUID (decimal string), glm vec (array of numbers),
        // Value/Object, or a reflectable struct (FromStruct).
        template <typename T>
        ObjectBuilder& Set( std::string_view key, const T& value )
        {
            m_Object[std::string( key )] = Detail::ToValue( value );
            return *this;
        }
        ObjectBuilder& Set( std::string_view key, const char* value )
        {
            return Set( key, std::string_view( value ) );
        }

        // Moves the object out (Set returns a reference, so a chain ends on an lvalue); the builder is empty
        // after.
        [[nodiscard]] Object Build()
        {
            return std::move( m_Object );
        }

    private:
        Object m_Object;
    };

    // The member names of T in declaration order (= write order), computed once per type.
    template <typename T>
    [[nodiscard]] const std::vector<std::string>& MemberNames()
    {
        static const std::vector<std::string> names = []
        {
            std::vector<std::string> out;
            for ( const auto& field : rfl::fields<T>() )
                out.push_back( field.name() );
            return out;
        }();
        return names;
    }

    // Two values are the same when their Write text is (the PrefabOverrides rule): member order counts,
    // 1 and 1.0 differ.
    [[nodiscard]] bool Same( const Value& a, const Value& b );

    // Json::Write of the value in the canonical file layout (Content/CanonicalText.hpp).
    [[nodiscard]] ResultStr<std::string> WriteCanonical( const Value& value );

    template <typename T>
    void Node::ReadValue( T& out, Issues& issues ) const
    {
        using U = std::remove_cvref_t<T>;
        if constexpr ( std::is_same_v<U, Value> )
            out = *m_Value;
        else if constexpr ( std::is_same_v<U, bool> )
        {
            if ( const auto v = AsBool() )
                out = v.GetValue();
            else
                Report( issues, "bool" );
        }
        else if constexpr ( std::is_enum_v<U> )
        {
            bool       read = false;
            const auto text = AsString();
            if ( text )
            {
                const auto parsed = rfl::string_to_enum<U>( text.GetValue() );
                if ( parsed )
                {
                    out  = parsed.value();
                    read = true;
                }
            }
            if ( !read )
                Report( issues, "an enumerator name" );
        }
        else if constexpr ( std::is_integral_v<U> )
        {
            const auto v    = AsInteger();
            bool       fits = false;
            if ( v )
            {
                const std::int64_t x = v.GetValue();
                if constexpr ( std::is_signed_v<U> )
                    fits = x >= static_cast<std::int64_t>( std::numeric_limits<U>::min() ) &&
                           x <= static_cast<std::int64_t>( std::numeric_limits<U>::max() );
                else
                    fits = x >= 0 && static_cast<std::uint64_t>( x ) <=
                                          static_cast<std::uint64_t>( std::numeric_limits<U>::max() );
            }
            if ( fits )
                out = static_cast<U>( v.GetValue() );
            else
                Report( issues, "integer in [" + std::to_string( std::numeric_limits<U>::min() ) + ", " +
                                     std::to_string( std::numeric_limits<U>::max() ) + "]" );
        }
        else if constexpr ( std::is_floating_point_v<U> )
        {
            const auto v = AsNumber();
            if ( v && ( !std::isfinite( v.GetValue() ) ||
                        std::abs( v.GetValue() ) <= static_cast<double>( std::numeric_limits<U>::max() ) ) )
                out = static_cast<U>( v.GetValue() );
            else
                Report( issues, "number" );
        }
        else if constexpr ( std::is_same_v<U, std::string> )
        {
            if ( const auto v = AsString() )
                out = v.GetValue();
            else
                Report( issues, "string" );
        }
        else if constexpr ( std::is_same_v<U, UUID> )
        {
            if ( const auto v = AsUuid() )
                out = v.GetValue();
            else
                Report( issues, "id (decimal string)" );
        }
        else if constexpr ( Detail::IsGlmVec<U> )
        {
            const auto* array = std::get_if<Value::Array>( &m_Value->variant() );
            U           read{};
            bool        ok = array && array->size() == static_cast<std::size_t>( U::length() );
            for ( glm::length_t i = 0; ok && i < U::length(); ++i )
            {
                const auto v = Node( ( *array )[i], m_Path.Index( i ) ).AsNumber();
                ok           = static_cast<bool>( v );
                if ( ok )
                    read[i] = static_cast<float>( v.GetValue() );
            }
            if ( ok )
                out = read;
            else
                Report( issues, "array of " + std::to_string( U::length() ) + " numbers" );
        }
        else
        {
            auto block = AsBlock<U>();
            if ( block )
                out = block.ExtractValue();
            else
            {
                // The error already starts with this node's path; the Issue carries it once.
                const std::string prefix = m_Path.ToString() + ": ";
                std::string       found  = block.GetError();
                if ( found.starts_with( prefix ) )
                    found.erase( 0, prefix.size() );
                issues.push_back( { m_Path.ToString(), "a readable block (dropped whole)", std::move( found ) } );
            }
        }
    }
} // namespace Common::Json
