#include "WaterWavesUpgrade.hpp"

#include <Water/Public/WaterWaves.hpp>

#include <Common/Json/ReadWrite.hpp>

#include <string_view>
#include <utility>

namespace Desert::Migration
{
    namespace
    {
        // Cuts a retired member out of the text before the typed read: the struct has no field of its shape, and
        // the read must not be asked whether it tolerates one. The value is a number or an array of numbers; the
        // member's own comma goes with it (the one after it, or the one before it when it is the last).
        Common::BoolResultStr CutMember( std::string& body, const std::string_view name )
        {
            const std::string key = "\"" + std::string( name ) + "\"";
            const std::size_t at  = body.find( key );
            if ( at == std::string::npos )
                return BOOLSUCCESS;
            if ( body.find( key, at + key.size() ) != std::string::npos )
                return Common::MakeFormattedError<bool>( "{} is stated more than once", name );
            const std::size_t colon = body.find( ':', at + key.size() );
            std::size_t       value =
                 colon == std::string::npos ? colon : body.find_first_not_of( " \t\r\n", colon + 1u );
            if ( value != std::string::npos && body[value] == '[' )
                value = body.find( ']', value );
            const std::size_t end = value == std::string::npos ? value : body.find_first_of( ",}", value );
            if ( end == std::string::npos )
                return Common::MakeFormattedError<bool>( "{} has no value", name );
            if ( body[end] == ',' )
            {
                body.erase( at, end + 1u - at );
                return BOOLSUCCESS;
            }
            const std::size_t before = body.find_last_not_of( " \t\r\n", at - 1u );
            const std::size_t from   = before != std::string::npos && body[before] == ',' ? before : at;
            body.erase( from, end - from );
            return BOOLSUCCESS;
        }
    } // namespace

    Common::ResultStr<std::string> MigrateWaterWavesToV3( const std::string& text )
    {
        namespace S = Assets::Serialization;

        std::string body = text;
        for ( const std::string_view name : { std::string_view( "WindAngleDeg" ), std::string_view( "CascadeSizes" ) } )
            if ( const auto cut = CutMember( body, name ); !cut )
                return Common::MakeError<std::string>( cut.GetError() );

        const auto read = Common::Json::Read<S::WaterWavesData>( body );
        if ( !read )
            return Common::MakeFormattedError<std::string>( "the body does not read: {}", read.GetError() );
        const S::WaterWavesData& data = read.GetValue();
        if ( !data.Header )
            return Common::MakeError<std::string>( "the file states no header, and this step raises WAVS 1 and 2" );
        const auto stated = data.Header->Versions.find( "WAVS" );
        if ( stated == data.Header->Versions.end() || stated->second < 1u || stated->second > 2u )
            return Common::MakeFormattedError<std::string>(
                 "the header states WAVS {}, and this step raises WAVS 1 and 2",
                 stated == data.Header->Versions.end() ? std::string( "nothing" )
                                                       : std::to_string( stated->second ) );

        std::string written = S::WriteWaterWaves( data );
        if ( auto reread = S::ParseWaterWaves( written ); !reread )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as WAVS 3: {}",
                                                            reread.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }
} // namespace Desert::Migration
