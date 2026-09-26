#include "Document.hpp"

#include <Common/Content/CanonicalText.hpp>
#include <Common/Core/Logger.hpp>

#include <rflcpp/rfl/json.hpp>

#include <limits>
#include <string>

namespace Common::Json
{
    Path Path::Key( std::string_view key ) const
    {
        Path next = *this;
        next.m_Segments.push_back( { SegmentKind::Key, std::string( key ) } );
        return next;
    }

    Path Path::Index( std::size_t index ) const
    {
        Path next = *this;
        next.m_Segments.push_back( { SegmentKind::Index, std::to_string( index ) } );
        return next;
    }

    Path Path::Record( std::string_view id ) const
    {
        Path next = *this;
        next.m_Segments.push_back( { SegmentKind::Record, std::string( id ) } );
        return next;
    }

    std::string Path::ToString() const
    {
        if ( m_Segments.empty() )
            return "document";
        std::string text;
        for ( const Segment& segment : m_Segments )
        {
            switch ( segment.Kind )
            {
                case SegmentKind::Key:
                    if ( !text.empty() )
                        text += '.';
                    text += segment.Text;
                    break;
                case SegmentKind::Index:
                    text += '[' + segment.Text + ']';
                    break;
                case SegmentKind::Record:
                    text += "[id=" + segment.Text + ']';
                    break;
            }
        }
        return text;
    }

    std::string Describe( const Issue& issue )
    {
        return issue.Path + ": expected " + issue.Expected + ", found " + issue.Found;
    }

    std::string ReportIssues( const Issues& issues, std::string_view document )
    {
        if ( issues.empty() )
            return {};
        std::string text = std::string( document ) + ": " + std::to_string( issues.size() ) +
                           ( issues.size() == 1 ? " value" : " values" ) +
                           " of the wrong type, each left at its current value: ";
        for ( std::size_t i = 0; i < issues.size(); ++i )
        {
            if ( i != 0 )
                text += "; ";
            text += Describe( issues[i] );
        }
        LOG_ERROR( "{}", text );
        return text;
    }

    namespace Detail
    {
        const Value& NullValue()
        {
            static const Value kNull;
            return kNull;
        }

        std::string KindName( Kind kind )
        {
            switch ( kind )
            {
                case Kind::Null:
                    return "null";
                case Kind::Bool:
                    return "bool";
                case Kind::Integer:
                    return "integer";
                case Kind::Real:
                    return "real";
                case Kind::String:
                    return "string";
                case Kind::Array:
                    return "array";
                case Kind::Object:
                    return "object";
            }
            return "unknown";
        }

        std::string DescribeFound( const Value& value )
        {
            const Kind kind = Root( value ).GetKind();
            switch ( kind )
            {
                case Kind::Bool:
                case Kind::Integer:
                case Kind::Real:
                case Kind::String:
                    return KindName( kind ) + " " + rfl::json::write( value );
                case Kind::Array:
                    return "array of " + std::to_string( std::get<Value::Array>( value.variant() ).size() ) +
                           " elements";
                case Kind::Object:
                    return "object with " + std::to_string( std::get<Object>( value.variant() ).size() ) +
                           " members";
                case Kind::Null:
                    break;
            }
            return "null";
        }
    } // namespace Detail

    Kind Node::GetKind() const
    {
        return std::visit(
             []( const auto& v ) -> Kind
             {
                 using V = std::remove_cvref_t<decltype( v )>;
                 if constexpr ( std::is_same_v<V, bool> )
                     return Kind::Bool;
                 else if constexpr ( std::is_same_v<V, std::int64_t> )
                     return Kind::Integer;
                 else if constexpr ( std::is_same_v<V, double> )
                     return Kind::Real;
                 else if constexpr ( std::is_same_v<V, std::string> )
                     return Kind::String;
                 else if constexpr ( std::is_same_v<V, Object> )
                     return Kind::Object;
                 else if constexpr ( std::is_same_v<V, Value::Array> )
                     return Kind::Array;
                 else
                     return Kind::Null;
             },
             m_Value->variant() );
    }

    std::optional<Node> Node::Find( std::string_view key ) const
    {
        const auto* object = std::get_if<Object>( &m_Value->variant() );
        if ( object == nullptr )
            return std::nullopt;
        for ( const auto& [name, value] : *object )
            if ( name == key )
                return Node( value, m_Path.Key( key ) );
        return std::nullopt;
    }

    ResultStr<Node> Node::Get( std::string_view key ) const
    {
        if ( GetKind() != Kind::Object )
            return MakeError<Node>( m_Path.ToString() + ": expected object, found " +
                                    Detail::DescribeFound( *m_Value ) );
        auto member = Find( key );
        if ( !member )
            return MakeError<Node>( m_Path.Key( key ).ToString() + ": missing — the format requires it" );
        return MakeSuccess( std::move( *member ) );
    }

    ResultStr<bool> Node::AsBool() const
    {
        if ( const auto* v = std::get_if<bool>( &m_Value->variant() ) )
            return MakeSuccess( *v );
        return MakeError<bool>( m_Path.ToString() + ": expected bool, found " +
                                Detail::DescribeFound( *m_Value ) );
    }

    ResultStr<double> Node::AsNumber() const
    {
        if ( const auto* v = std::get_if<double>( &m_Value->variant() ) )
            return MakeSuccess( *v );
        if ( const auto* v = std::get_if<std::int64_t>( &m_Value->variant() ) )
            return MakeSuccess( static_cast<double>( *v ) );
        return MakeError<double>( m_Path.ToString() + ": expected number, found " +
                                  Detail::DescribeFound( *m_Value ) );
    }

    ResultStr<std::int64_t> Node::AsInteger() const
    {
        if ( const auto* v = std::get_if<std::int64_t>( &m_Value->variant() ) )
            return MakeSuccess( *v );
        if ( const auto* d = std::get_if<double>( &m_Value->variant() ) )
        {
            constexpr double kUpperBound = 9223372036854775808.0; // 2^63, the first value int64 lacks
            constexpr double kLowerBound = -9223372036854775808.0;
            if ( *d >= kLowerBound && *d < kUpperBound &&
                 *d == static_cast<double>( static_cast<std::int64_t>( *d ) ) )
                return MakeSuccess( static_cast<std::int64_t>( *d ) );
        }
        return MakeError<std::int64_t>( m_Path.ToString() + ": expected integer, found " +
                                        Detail::DescribeFound( *m_Value ) );
    }

    ResultStr<std::string> Node::AsString() const
    {
        if ( const auto* v = std::get_if<std::string>( &m_Value->variant() ) )
            return MakeSuccess( std::string( *v ) );
        return MakeError<std::string>( m_Path.ToString() + ": expected string, found " +
                                       Detail::DescribeFound( *m_Value ) );
    }

    ResultStr<UUID> Node::AsUuid() const
    {
        const auto*   text  = std::get_if<std::string>( &m_Value->variant() );
        bool          ok    = text != nullptr && !text->empty() && text->size() <= 20;
        std::uint64_t value = 0;
        for ( std::size_t i = 0; ok && i < text->size(); ++i )
        {
            const char character = ( *text )[i];
            if ( character < '0' || character > '9' )
            {
                ok = false;
                break;
            }
            const auto digit = static_cast<std::uint64_t>( character - '0' );
            if ( value > ( std::numeric_limits<std::uint64_t>::max() - digit ) / 10 )
            {
                ok = false;
                break;
            }
            value = value * 10 + digit;
        }
        if ( !ok )
            return MakeError<UUID>( m_Path.ToString() + ": expected id (decimal string), found " +
                                    Detail::DescribeFound( *m_Value ) );
        return MakeSuccess( UUID( value ) );
    }

    ResultStr<Value> Parse( std::string_view json )
    {
        // yyjson_read_opts takes a mutable buffer (it writes into it only in insitu mode, which is off here);
        // a private copy gives it one without casting const away from the caller's text.
        std::string     buffer( json );
        yyjson_read_err error{};
        yyjson_doc*     doc = yyjson_read_opts( buffer.data(), buffer.size(), 0, nullptr, &error );
        if ( doc == nullptr )
            return MakeError<Value>( "document: not JSON at byte " + std::to_string( error.pos ) + ": " +
                                     ( error.msg != nullptr ? error.msg : "unknown error" ) );
        try
        {
            auto parsed = rfl::json::read<Value>( rfl::json::InputVarType( yyjson_doc_get_root( doc ) ) );
            yyjson_doc_free( doc );
            if ( !parsed )
                return MakeError<Value>( Detail::DescribeReadError( parsed.error().what() ) );
            return MakeSuccess( std::move( parsed.value() ) );
        }
        catch ( const std::exception& e )
        {
            yyjson_doc_free( doc );
            return MakeError<Value>( Detail::DescribeReadError( e.what() ) );
        }
    }

    bool Same( const Value& a, const Value& b )
    {
        return rfl::json::write( a ) == rfl::json::write( b );
    }

    ResultStr<std::string> WriteCanonical( const Value& value )
    {
        return Content::CanonicalJsonTextOfWriterOutput( rfl::json::write( value ) );
    }
} // namespace Common::Json
