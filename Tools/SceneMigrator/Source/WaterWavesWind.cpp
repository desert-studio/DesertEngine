#include "WaterWavesWind.hpp"

#include <Water/Public/WaterWaves.hpp>

#include <Common/Json/ReadWrite.hpp>

#include <utility>

namespace Desert::Migration
{
    Common::ResultStr<std::string> MigrateWaterWavesV1ToV2( const std::string& text )
    {
        namespace S = Assets::Serialization;

        // The retired member is cut out of the text before the typed read: v2's struct has no field for it, and
        // the read must not be asked whether it tolerates one. The value is a number, so it ends at the next ','
        // or '}'; the member's own comma goes with it (the one after it, or the one before it when it is last).
        std::string       body = text;
        const std::string key  = "\"WindAngleDeg\"";
        if ( const std::size_t at = body.find( key ); at != std::string::npos )
        {
            if ( body.find( key, at + key.size() ) != std::string::npos )
                return Common::MakeError<std::string>( "WAVS 1 states WindAngleDeg more than once" );
            const std::size_t colon = body.find( ':', at + key.size() );
            const std::size_t end   = colon == std::string::npos ? colon : body.find_first_of( ",}", colon + 1u );
            if ( end == std::string::npos )
                return Common::MakeError<std::string>( "WAVS 1 Generator.WindAngleDeg has no value" );
            if ( body[end] == ',' )
                body.erase( at, end + 1u - at );
            else if ( const std::size_t comma = body.rfind( ',', at ); comma != std::string::npos )
                body.erase( comma, end - comma );
            else
                body.erase( at, end - at );
        }

        const auto v1 = Common::Json::Read<S::WaterWavesData>( body );
        if ( !v1 )
            return Common::MakeFormattedError<std::string>( "WAVS 1 body does not read: {}", v1.GetError() );
        const S::WaterWavesData& data = v1.GetValue();
        if ( !data.Header )
            return Common::MakeError<std::string>( "the file states no header, and this step raises WAVS 1 only" );
        const auto stated = data.Header->Versions.find( "WAVS" );
        if ( stated == data.Header->Versions.end() || stated->second != 1u )
            return Common::MakeFormattedError<std::string>(
                 "the header states WAVS {}, and this step raises WAVS 1 only",
                 stated == data.Header->Versions.end() ? std::string( "nothing" )
                                                       : std::to_string( stated->second ) );

        std::string written = S::WriteWaterWaves( data );
        if ( auto reread = S::ParseWaterWaves( written ); !reread )
            return Common::MakeFormattedError<std::string>( "the raised file does not read as WAVS 2: {}",
                                                            reread.GetError() );
        return Common::MakeSuccess( std::move( written ) );
    }
} // namespace Desert::Migration
