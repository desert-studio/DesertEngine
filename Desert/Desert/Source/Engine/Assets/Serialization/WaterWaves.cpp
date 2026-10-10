#include <Engine/Assets/Serialization/WaterWaves.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Json.hpp>

#include <cmath>
#include <string>
#include <system_error>

namespace Desert::Assets::Serialization
{
    namespace
    {
        Common::BoolResultStr CheckRange( const char* minName, float min, const char* maxName, float max )
        {
            if ( !std::isfinite( min ) || !std::isfinite( max ) || min < 0.0f || min > max )
                return Common::MakeFormattedError<bool>( "Generator.{} {} and Generator.{} {} must be finite, "
                                                         "not negative and {} <= {}",
                                                         minName, min, maxName, max, minName, maxName );
            return BOOLSUCCESS;
        }

        Common::BoolResultStr CheckAtLeastZero( const char* name, float value )
        {
            if ( !std::isfinite( value ) || value < 0.0f )
                return Common::MakeFormattedError<bool>( "Generator.{} {} must be finite and not negative", name,
                                                         value );
            return BOOLSUCCESS;
        }

        Common::BoolResultStr CheckUnit( const char* name, float value )
        {
            if ( !std::isfinite( value ) || value < 0.0f || value > 1.0f )
                return Common::MakeFormattedError<bool>( "Generator.{} {} must lie in [0, 1]", name, value );
            return BOOLSUCCESS;
        }
    } // namespace

    Common::BoolResultStr ValidateWaterWavesData( const WaterWavesData& data )
    {
        const Water::GerstnerWaveGenerator& g = data.Generator;
        if ( g.NumWaves < 1 || g.NumWaves > kWaterWavesMaxNumWaves )
            return Common::MakeFormattedError<bool>( "Generator.NumWaves {} must lie in [1, {}]", g.NumWaves,
                                                     kWaterWavesMaxNumWaves );
        if ( auto ok = CheckRange( "MinWavelength", g.MinWavelength, "MaxWavelength", g.MaxWavelength ); !ok )
            return ok;
        if ( auto ok = CheckRange( "MinAmplitude", g.MinAmplitude, "MaxAmplitude", g.MaxAmplitude ); !ok )
            return ok;
        if ( auto ok = CheckAtLeastZero( "WavelengthFalloff", g.WavelengthFalloff ); !ok )
            return ok;
        if ( auto ok = CheckAtLeastZero( "AmplitudeFalloff", g.AmplitudeFalloff ); !ok )
            return ok;
        if ( auto ok = CheckAtLeastZero( "SteepnessFalloff", g.SteepnessFalloff ); !ok )
            return ok;
        if ( auto ok = CheckAtLeastZero( "DirectionAngularSpreadDeg", g.DirectionAngularSpreadDeg ); !ok )
            return ok;
        if ( !std::isfinite( g.WindAngleDeg ) )
            return Common::MakeFormattedError<bool>( "Generator.WindAngleDeg {} must be finite", g.WindAngleDeg );
        if ( auto ok = CheckUnit( "Randomness", g.Randomness ); !ok )
            return ok;
        if ( auto ok = CheckUnit( "SmallWaveSteepness", g.SmallWaveSteepness ); !ok )
            return ok;
        if ( auto ok = CheckUnit( "LargeWaveSteepness", g.LargeWaveSteepness ); !ok )
            return ok;
        return BOOLSUCCESS;
    }

    Common::ResultStr<WaterWavesData> ParseWaterWaves( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<WaterWavesData>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kWaterWavesVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<WaterWavesData>( "water waves {}", headed.GetError() );

        const auto parsed = Common::Json::Read<WaterWavesData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<WaterWavesData>( "{}", parsed.GetError() );
        WaterWavesData data = parsed.GetValue();

        if ( auto header = Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::WaterWaves,
                                                      Assets::kWaterWavesSchemaTag, kWaterWavesVersion,
                                                      WaterWavesTextSubsystems() );
             !header )
            return Common::MakeFormattedError<WaterWavesData>( "water waves {}", header.GetError() );

        if ( auto valid = ValidateWaterWavesData( data ); !valid )
            return Common::MakeFormattedError<WaterWavesData>( "{}", valid.GetError() );

        // A wave set references no other asset, so a stated Dependency is an edge the registry would follow
        // to something this file never names.
        if ( data.Header && !data.Header->Dependencies.empty() )
            return Common::MakeFormattedError<WaterWavesData>(
                 "the header states {} Dependencies; a wave set references no asset",
                 data.Header->Dependencies.size() );

        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteWaterWaves( const WaterWavesData& data )
    {
        WaterWavesData out = data;
        out.Header         = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::WaterWaves,
                                                      WaterWavesTextSubsystems() );
        out.Header->Dependencies.clear();
        return Common::Json::Write( out );
    }

    Common::BoolResultStr SaveWaterWavesFile( const std::filesystem::path& path, const WaterWavesData& data )
    {
        if ( auto valid = ValidateWaterWavesData( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write water waves '{}': {}", path.string(),
                                                     valid.GetError() );
        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot create '{}' for water waves: {}",
                                                     path.parent_path().string(), ec.message() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteWaterWaves( data ) );
    }
} // namespace Desert::Assets::Serialization
