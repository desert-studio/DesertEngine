#include "WaterWavesUpgrade.hpp"

#include <Water/Public/WaterWaves.hpp>

#include <Common/Json/ReadWrite.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace Desert::Migration
{
    namespace
    {
        struct Member
        {
            std::size_t Key   = std::string::npos; // the opening quote of its name; npos: the text has none
            std::size_t Value = std::string::npos;
            std::size_t End   = std::string::npos; // one past the value
        };

        // The one member of the name, whose value is a number or an array of numbers.
        Common::ResultStr<Member> FindMember( const std::string& body, const std::string_view name )
        {
            const std::string key = "\"" + std::string( name ) + "\"";
            const std::size_t at  = body.find( key );
            if ( at == std::string::npos )
                return Common::MakeSuccess( Member{} );
            if ( body.find( key, at + key.size() ) != std::string::npos )
                return Common::MakeFormattedError<Member>( "{} is stated more than once", name );
            const std::size_t colon = body.find( ':', at + key.size() );
            const std::size_t value =
                 colon == std::string::npos ? colon : body.find_first_not_of( " \t\r\n", colon + 1u );
            std::size_t end = value;
            if ( value != std::string::npos && body[value] == '[' )
            {
                end = body.find( ']', value );
                end = end == std::string::npos ? end : end + 1u;
            }
            else if ( value != std::string::npos )
                end = body.find_first_of( ",} \t\r\n", value );
            if ( end == std::string::npos )
                return Common::MakeFormattedError<Member>( "{} has no value", name );
            return Common::MakeSuccess( Member{ at, value, end } );
        }

        // Cuts a retired member out of the text before the typed read: the struct has no field of its shape, and
        // the read must not be asked whether it tolerates one. The member's own comma goes with it (the one after
        // it, or the one before it when it is the last).
        void CutMember( std::string& body, const Member& member )
        {
            const std::size_t after = body.find_first_not_of( " \t\r\n", member.End );
            if ( after != std::string::npos && body[after] == ',' )
            {
                body.erase( member.Key, after + 1u - member.Key );
                return;
            }
            const std::size_t before = body.find_last_not_of( " \t\r\n", member.Key - 1u );
            const std::size_t from   = before != std::string::npos && body[before] == ',' ? before : member.Key;
            body.erase( from, member.End - from );
        }

        std::size_t ElementsOf( const std::string_view array )
        {
            return array.find_first_of( "0123456789" ) == std::string_view::npos
                        ? 0u
                        : static_cast<std::size_t>( std::count( array.begin(), array.end(), ',' ) ) + 1u;
        }
    } // namespace

    Common::ResultStr<WaterWavesRaised> MigrateWaterWavesToV3( const std::string& text )
    {
        namespace S = Assets::Serialization;

        std::string      body = text;
        WaterWavesRaised raised;
        for ( const std::string_view name : { std::string_view( "WindAngleDeg" ), std::string_view( "CascadeSizes" ) } )
        {
            const auto found = FindMember( body, name );
            if ( !found )
                return Common::MakeError<WaterWavesRaised>( found.GetError() );
            const Member& member = found.GetValue();
            if ( member.Key == std::string::npos )
                continue;
            const std::string_view value = std::string_view( body ).substr( member.Value, member.End - member.Value );
            if ( name == "CascadeSizes" && ElementsOf( value ) != 3u )
                continue;
            raised.Dropped.push_back( std::string( name ) + " " + std::string( value ) );
            CutMember( body, member );
        }

        const auto read = Common::Json::Read<S::WaterWavesData>( body );
        if ( !read )
            return Common::MakeFormattedError<WaterWavesRaised>( "the body does not read: {}", read.GetError() );
        const S::WaterWavesData& data = read.GetValue();
        if ( !data.Header )
            return Common::MakeError<WaterWavesRaised>( "the file states no header, and this step raises WAVS 1 and 2" );
        const auto stated = data.Header->Versions.find( "WAVS" );
        if ( stated == data.Header->Versions.end() || stated->second < 1u || stated->second > 2u )
            return Common::MakeFormattedError<WaterWavesRaised>(
                 "the header states WAVS {}, and this step raises WAVS 1 and 2",
                 stated == data.Header->Versions.end() ? std::string( "nothing" )
                                                       : std::to_string( stated->second ) );

        std::string written = S::WriteWaterWaves( data );
        if ( auto reread = S::ParseWaterWaves( written ); !reread )
            return Common::MakeFormattedError<WaterWavesRaised>( "the raised file does not read as WAVS 3: {}",
                                                                 reread.GetError() );
        raised.Text = std::move( written );
        return Common::MakeSuccess( std::move( raised ) );
    }
} // namespace Desert::Migration
