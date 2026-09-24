#include "CanonicalText.hpp"

#include <Common/Core/Core.hpp>

#include <rflcpp/rfl/thirdparty/yyjson.h>

#include <cstdlib>
#include <memory>

namespace Common::Content
{
    namespace
    {
        struct DocFree
        {
            void operator()( yyjson_doc* doc ) const
            {
                yyjson_doc_free( doc );
            }
        };

        // A scalar is spelled by yyjson itself, the writer rfl::json writes with: the shortest round-trip
        // spelling of a real, the same escaping of a string. Owning the spelling here would be a second
        // source of truth for how a number looks, and any disagreement would be a value change on reformat.
        bool AppendScalar( std::string& out, yyjson_val* value )
        {
            std::size_t length = 0;
            char*       text   = yyjson_val_write( value, YYJSON_WRITE_NOFLAG, &length );
            if ( text == nullptr )
                return false;
            out.append( text, length );
            std::free( text );
            return true;
        }

        bool IsContainer( yyjson_val* value )
        {
            return yyjson_is_obj( value ) || yyjson_is_arr( value );
        }

        void Indent( std::string& out, std::size_t depth )
        {
            out.append( depth * 4, ' ' );
        }

        bool AppendValue( std::string& out, yyjson_val* value, std::size_t depth )
        {
            if ( yyjson_is_obj( value ) )
            {
                if ( yyjson_obj_size( value ) == 0 )
                {
                    out += "{}";
                    return true;
                }
                out += "{\n";
                yyjson_obj_iter iter;
                yyjson_obj_iter_init( value, &iter );
                std::size_t remaining = yyjson_obj_size( value );
                while ( yyjson_val* key = yyjson_obj_iter_next( &iter ) )
                {
                    Indent( out, depth + 1 );
                    if ( !AppendScalar( out, key ) )
                        return false;
                    out += ": ";
                    if ( !AppendValue( out, yyjson_obj_iter_get_val( key ), depth + 1 ) )
                        return false;
                    out += --remaining > 0 ? ",\n" : "\n";
                }
                Indent( out, depth );
                out += '}';
                return true;
            }
            if ( yyjson_is_arr( value ) )
            {
                const std::size_t size = yyjson_arr_size( value );
                if ( size == 0 )
                {
                    out += "[]";
                    return true;
                }
                bool        scalars = true;
                std::size_t idx     = 0;
                std::size_t count   = 0;
                yyjson_val* element = nullptr;
                yyjson_arr_foreach( value, idx, count, element )
                {
                    scalars = scalars && !IsContainer( element );
                }
                if ( scalars && size <= kCanonicalInlineArray )
                {
                    out += '[';
                    yyjson_arr_foreach( value, idx, count, element )
                    {
                        if ( idx > 0 )
                            out += ", ";
                        if ( !AppendScalar( out, element ) )
                            return false;
                    }
                    out += ']';
                    return true;
                }
                out += "[\n";
                yyjson_arr_foreach( value, idx, count, element )
                {
                    // Scalars go kCanonicalInlineArray to a line; a container starts a line of its own.
                    const bool opensLine = !scalars || idx % kCanonicalInlineArray == 0;
                    if ( opensLine )
                        Indent( out, depth + 1 );
                    if ( scalars ? !AppendScalar( out, element ) : !AppendValue( out, element, depth + 1 ) )
                        return false;
                    const bool last      = idx + 1 == count;
                    const bool closeLine = last || !scalars || ( idx + 1 ) % kCanonicalInlineArray == 0;
                    if ( !last )
                        out += closeLine ? "," : ", ";
                    if ( closeLine )
                        out += '\n';
                }
                Indent( out, depth );
                out += ']';
                return true;
            }
            return AppendScalar( out, value );
        }
    } // namespace

    ResultStr<std::string> CanonicalJsonText( std::string_view json )
    {
        yyjson_read_err                            error{};
        const std::unique_ptr<yyjson_doc, DocFree> doc( yyjson_read_opts(
             const_cast<char*>( json.data() ), json.size(), YYJSON_READ_NOFLAG, nullptr, &error ) );
        if ( !doc )
            return MakeFormattedError<std::string>( "canonical text: the input is not JSON ({} at byte {} of {})",
                                                    error.msg != nullptr ? error.msg : "unknown error", error.pos,
                                                    json.size() );
        std::string out;
        out.reserve( json.size() + json.size() / 2 );
        if ( !AppendValue( out, yyjson_doc_get_root( doc.get() ), 0 ) )
            return MakeFormattedError<std::string>( "canonical text: a value of the {}-byte document could not be "
                                                    "spelled (yyjson_val_write failed)",
                                                    json.size() );
        out += '\n';
        return MakeSuccess( std::move( out ) );
    }

    std::string CanonicalJsonTextOfWriterOutput( std::string_view json )
    {
        auto canonical = CanonicalJsonText( json );
        DESERT_VERIFY( canonical, "a JSON writer produced text that is not JSON: {}", canonical.GetError() );
        return std::move( canonical.GetValue() );
    }

    bool IsCanonicalJsonText( std::string_view json )
    {
        const auto canonical = CanonicalJsonText( json );
        return canonical && canonical.GetValue() == json;
    }
} // namespace Common::Content
