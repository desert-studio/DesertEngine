#include <Engine/Assets/Serialization/WeatherPreset.hpp>
#include <Engine/Assets/TextAssetHeaderCheck.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Json/Json.hpp>

#include <cmath>
#include <string>
#include <system_error>
#include <unordered_set>

namespace Desert::Assets::Serialization
{
    Common::BoolResultStr ValidateWeatherPresetData( const WeatherPresetData& data )
    {
        std::unordered_set<std::string> names;
        for ( size_t i = 0; i < data.Parameters.size(); ++i )
        {
            const WeatherParameterValue& p = data.Parameters[i];
            if ( !IsWorldParameterName( p.Name ) )
                return Common::MakeFormattedError<bool>( "Parameters[{}] name '{}' is not an identifier", i,
                                                         p.Name );
            if ( !names.insert( p.Name ).second )
                return Common::MakeFormattedError<bool>( "Parameters[{}] name '{}' is stated twice", i, p.Name );
            for ( size_t c = 0; c < p.Value.size(); ++c )
                if ( !std::isfinite( p.Value[c] ) )
                    return Common::MakeFormattedError<bool>( "Parameters[{}] '{}' value[{}] {} is not finite", i,
                                                             p.Name, c, p.Value[c] );
        }
        if ( data.CloudCoverage && !( *data.CloudCoverage >= 0.0f && *data.CloudCoverage <= 1.0f ) )
            return Common::MakeFormattedError<bool>( "CloudCoverage {} is outside [0, 1]", *data.CloudCoverage );
        std::unordered_set<std::string> types;
        for ( size_t i = 0; i < data.CloudTypes.size(); ++i )
        {
            const WeatherCloudTypeWeight& t = data.CloudTypes[i];
            if ( t.Type.empty() )
                return Common::MakeFormattedError<bool>( "CloudTypes[{}] names no cloud type", i );
            if ( !types.insert( t.Type ).second )
                return Common::MakeFormattedError<bool>( "CloudTypes[{}] '{}' is stated twice", i, t.Type );
            if ( !( t.Weight >= 0.0f && t.Weight <= 1.0f ) )
                return Common::MakeFormattedError<bool>( "CloudTypes[{}] '{}' weight {} is outside [0, 1]", i,
                                                         t.Type, t.Weight );
        }
        if ( data.CloudDensity && !( *data.CloudDensity >= 0.0f && *data.CloudDensity <= 2.0f ) )
            return Common::MakeFormattedError<bool>( "CloudDensity {} is outside [0, 2]", *data.CloudDensity );
        if ( data.FogDensity && !( *data.FogDensity >= 0.0f && *data.FogDensity <= 0.5f ) )
            return Common::MakeFormattedError<bool>( "FogDensity {} is outside [0, 0.5]", *data.FogDensity );
        if ( data.WindSpeed && !( *data.WindSpeed >= 0.0f && std::isfinite( *data.WindSpeed ) ) )
            return Common::MakeFormattedError<bool>( "WindSpeed {} is not a finite speed >= 0", *data.WindSpeed );
        return BOOLSUCCESS;
    }

    Common::ResultStr<WeatherPresetData> ParseWeatherPreset( const std::string& text )
    {
        if ( text.empty() )
            return Common::MakeFormattedError<WeatherPresetData>( "the file is empty" );

        if ( auto headed = Assets::RefuseTextWithoutHeader( text, kWeatherPresetVersion, std::nullopt ); !headed )
            return Common::MakeFormattedError<WeatherPresetData>( "weather preset {}", headed.GetError() );

        const auto parsed = Common::Json::Read<WeatherPresetData>( text );
        if ( !parsed )
            return Common::MakeFormattedError<WeatherPresetData>( "{}", parsed.GetError() );
        WeatherPresetData data = parsed.GetValue();

        if ( auto header = Assets::CheckStatedHeader( data.Header, Common::Content::ContentKind::WeatherPreset,
                                                      Assets::kWeatherPresetSchemaTag, kWeatherPresetVersion,
                                                      WeatherPresetTextSubsystems() );
             !header )
            return Common::MakeFormattedError<WeatherPresetData>( "weather preset {}", header.GetError() );

        if ( auto valid = ValidateWeatherPresetData( data ); !valid )
            return Common::MakeFormattedError<WeatherPresetData>( "{}", valid.GetError() );

        if ( data.Header && !data.Header->Dependencies.empty() )
            return Common::MakeFormattedError<WeatherPresetData>(
                 "the header states {} Dependencies; a weather preset references no asset",
                 data.Header->Dependencies.size() );

        return Common::MakeSuccess( std::move( data ) );
    }

    std::string WriteWeatherPreset( const WeatherPresetData& data )
    {
        WeatherPresetData out = data;
        out.Header            = Assets::StampTextHeader( data.Header, Common::Content::ContentKind::WeatherPreset,
                                                         WeatherPresetTextSubsystems() );
        out.Header->Dependencies.clear();
        return Common::Json::Write( out );
    }

    Common::BoolResultStr SaveWeatherPresetFile( const std::filesystem::path& path, const WeatherPresetData& data )
    {
        if ( auto valid = ValidateWeatherPresetData( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write weather preset '{}': {}", path.string(),
                                                     valid.GetError() );
        std::error_code ec;
        if ( path.has_parent_path() )
            std::filesystem::create_directories( path.parent_path(), ec );
        if ( ec )
            return Common::MakeFormattedError<bool>( "cannot create '{}' for a weather preset: {}",
                                                     path.parent_path().string(), ec.message() );
        return Common::Content::WriteCanonicalJsonFileAtomic( path, WriteWeatherPreset( data ) );
    }
} // namespace Desert::Assets::Serialization
