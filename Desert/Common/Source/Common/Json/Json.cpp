#include "Json.hpp"

#include <rflcpp/rfl/Generic.hpp>

#include <string>
#include <vector>

namespace Common::Json
{
    namespace
    {
        constexpr std::string_view kNested  = "Failed to parse field '";
        constexpr std::string_view kMissing = "Field named '";
        constexpr std::string_view kUnknown = "Value named '";
        constexpr std::string_view kMany    = "Found ";

        void AppendPath( std::string& path, std::string_view name )
        {
            if ( !path.empty() )
                path += '.';
            path += name;
        }

        // Takes "<prefix>NAME'" off the front of rest and returns NAME, or nothing when rest does not start so.
        bool TakeQuoted( std::string_view& rest, std::string_view prefix, std::string_view& name )
        {
            if ( !rest.starts_with( prefix ) )
                return false;
            const std::size_t close = rest.find( '\'', prefix.size() );
            if ( close == std::string_view::npos )
                return false;
            name = rest.substr( prefix.size(), close - prefix.size() );
            rest = rest.substr( close + 1 );
            return true;
        }

        // rfl joins several failures of one object as "Found N errors:\n1) ...\n2) ...", indenting each
        // item's own continuation lines by four spaces; this splits them back into the item texts.
        std::vector<std::string> SplitItems( std::string_view body )
        {
            std::vector<std::string> items;
            std::size_t              start = 0;
            while ( start <= body.size() )
            {
                const std::size_t end  = body.find( '\n', start );
                std::string_view  line = body.substr( start, end == std::string_view::npos ? end : end - start );
                const std::size_t digits = line.find_first_not_of( "0123456789" );
                if ( digits != 0 && digits != std::string_view::npos && line.substr( digits ).starts_with( ") " ) )
                    items.emplace_back( line.substr( digits + 2 ) );
                else if ( !items.empty() && line.starts_with( "    " ) )
                    items.back().append( "\n" ).append( line.substr( 4 ) );
                else if ( !line.empty() )
                    items.emplace_back( line ); // rfl's "More than 10 errors occurred" tail
                if ( end == std::string_view::npos )
                    break;
                start = end + 1;
            }
            return items;
        }

        void Describe( std::string_view message, std::string path, std::vector<std::string>& out )
        {
            // rfl reports a nested failure as one prefix per level it unwinds through. Peeling them into a
            // dotted path is what lets the message point at the line a person has to edit.
            std::string_view rest = message;
            std::string_view name;
            while ( rest.starts_with( kNested ) && rest.find( "': ", kNested.size() ) != std::string_view::npos )
            {
                TakeQuoted( rest, kNested, name );
                AppendPath( path, name );
                rest.remove_prefix( 2 ); // the ": " after the quote
            }

            if ( rest.starts_with( kMany ) && rest.find( " errors:\n" ) != std::string_view::npos )
            {
                for ( const auto& item : SplitItems( rest.substr( rest.find( '\n' ) + 1 ) ) )
                    Describe( item, path, out );
                return;
            }

            std::string what( rest );
            if ( TakeQuoted( rest, kMissing, name ) )
            {
                AppendPath( path, name );
                what = "missing — the format requires it (only a std::optional member may be absent)";
            }
            else if ( TakeQuoted( rest, kUnknown, name ) )
            {
                AppendPath( path, name );
                what = "unknown key — the format does not declare it";
            }
            out.push_back( path.empty() ? "document: " + what : "field '" + path + "': " + what );
        }
    } // namespace

    namespace Detail
    {
        std::string DescribeReadError( std::string_view rflMessage )
        {
            std::vector<std::string> lines;
            Describe( rflMessage, {}, lines );
            std::string joined;
            for ( const auto& line : lines )
            {
                if ( !joined.empty() )
                    joined += "; ";
                joined += line;
            }
            return joined;
        }
    } // namespace Detail

    ResultStr<std::vector<std::pair<std::string, std::string>>> ObjectMembers( std::string_view json )
    {
        using Members = std::vector<std::pair<std::string, std::string>>;
        try
        {
            const auto parsed = rfl::json::read<rfl::Generic>( json );
            if ( !parsed )
                return MakeError<Members>( Detail::DescribeReadError( parsed.error().what() ) );
            const auto object = parsed.value().to_object();
            if ( !object )
                return MakeError<Members>( "document: the top level is not a JSON object" );

            Members members;
            for ( const auto& [name, value] : object.value() )
                members.emplace_back( name, rfl::json::write( value ) );
            return MakeSuccess( std::move( members ) );
        }
        catch ( const std::exception& e )
        {
            return MakeError<Members>( Detail::DescribeReadError( e.what() ) );
        }
    }
} // namespace Common::Json
