#include <Engine/Assets/WeatherPresetAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Assets
{
    WeatherPresetAsset::WeatherPresetAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::WeatherPreset )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::BoolResultStr WeatherPresetAsset::LoadFromFile()
    {
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        const std::string           path = file.string();

        std::string text;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
             packed.has_value() )
            text = packed.value();
        else if ( auto read = Common::Utils::FileSystem::ReadFileContent( file ); read )
            text = read.ExtractValue();

        if ( text.empty() )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "weather preset '{}' is empty or could not be opened", path );
        }
        auto parsed = Serialization::ParseWeatherPreset( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "weather preset '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        }
        m_Data = parsed.ExtractValue();
        if ( m_Data.Header )
            if ( const auto guid = Common::Content::AssetGuidFromText( m_Data.Header->Guid ); guid )
                m_Guid = guid.GetValue();
        m_Ready = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WeatherPresetAsset::Unload()
    {
        m_Data  = Serialization::WeatherPresetData{};
        m_Ready = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WeatherPresetAsset::Save( const Common::Filepath&                 filepath,
                                                    const Serialization::WeatherPresetData& data )
    {
        if ( auto ok = Serialization::SaveWeatherPresetFile( filepath, data ); !ok )
            return ok;
        LOG_INFO( "[Weather] Preset written: '{}', {} parameters.", filepath.string(), data.Parameters.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
