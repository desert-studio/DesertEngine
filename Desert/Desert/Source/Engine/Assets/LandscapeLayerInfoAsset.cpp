#include <Engine/Assets/LandscapeLayerInfoAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Assets
{
    LandscapeLayerInfoAsset::LandscapeLayerInfoAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::LandscapeLayerInfo )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::BoolResultStr LandscapeLayerInfoAsset::LoadFromFile()
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
            return Common::MakeFormattedError<bool>( "landscape layer info '{}' is empty or could not be opened",
                                                     path );
        }
        auto parsed = Serialization::ParseLandscapeLayerInfo( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "landscape layer info '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        }
        m_Data = parsed.ExtractValue();
        if ( m_Data.Header )
            if ( const auto guid = Common::Content::AssetGuidFromText( m_Data.Header->Guid ); guid )
                m_Guid = guid.GetValue();
        m_Ready = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr LandscapeLayerInfoAsset::Unload()
    {
        m_Data  = Serialization::LandscapeLayerInfoData{};
        m_Ready = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr LandscapeLayerInfoAsset::Save( const Common::Filepath&                      filepath,
                                                         const Serialization::LandscapeLayerInfoData& data )
    {
        if ( auto ok = Serialization::SaveLandscapeLayerInfoFile( filepath, data ); !ok )
            return ok;
        LOG_INFO( "[Landscape] Layer info written: '{}', layer '{}'.", filepath.string(), data.LayerName );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
