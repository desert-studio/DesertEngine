#include "Json.hpp"

#include <rflcpp/rfl/Generic.hpp>

namespace Common::Json
{
    namespace Detail
    {
        std::string DescribeReadError( std::string_view rflMessage )
        {
            // rfl reports a nested failure as one prefix per level it unwinds through. Peeling them into a
            // dotted path is what lets the message point at the line a person has to edit.
            constexpr std::string_view kNested  = "Failed to parse field '";
            constexpr std::string_view kMissing = "Field named '";

            std::string      path;
            std::string_view rest = rflMessage;
            while ( rest.starts_with( kNested ) )
            {
                const std::size_t close = rest.find( "': ", kNested.size() );
                if ( close == std::string_view::npos )
                    break;
                if ( !path.empty() )
                    path += '.';
                path += rest.substr( kNested.size(), close - kNested.size() );
                rest = rest.substr( close + 3 );
            }

            // The innermost level of a missing field names the field itself rather than a parse failure.
            if ( rest.starts_with( kMissing ) )
            {
                const std::size_t close = rest.find( '\'', kMissing.size() );
                if ( close != std::string_view::npos )
                {
                    if ( !path.empty() )
                        path += '.';
                    path += rest.substr( kMissing.size(), close - kMissing.size() );
                    rest = rest.substr( close + 1 );
                    while ( rest.starts_with( ' ' ) )
                        rest.remove_prefix( 1 );
                }
            }

            if ( path.empty() )
                return "document: " + std::string( rest );
            return "field '" + path + "': " + std::string( rest );
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
