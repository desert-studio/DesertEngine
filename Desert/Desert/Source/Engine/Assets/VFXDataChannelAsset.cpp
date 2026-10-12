#include <Engine/Assets/VFXDataChannelAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

namespace Desert::Assets
{
    VFXDataChannelAsset::VFXDataChannelAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::VFXDataChannel )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::BoolResultStr VFXDataChannelAsset::LoadFromFile()
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
            return Common::MakeFormattedError<bool>( "VFX data channel '{}' is empty or could not be opened",
                                                     path );
        }
        auto parsed = Serialization::ParseVFXDataChannel( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "VFX data channel '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        }
        m_Data = parsed.ExtractValue();
        if ( m_Data.Header )
            if ( const auto guid = Common::Content::AssetGuidFromText( m_Data.Header->Guid ); guid )
                m_Guid = guid.GetValue();
        m_Ready = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VFXDataChannelAsset::Unload()
    {
        m_Data  = Serialization::VFXDataChannelData{};
        m_Ready = false;
        return BOOLSUCCESS;
    }

    std::string VFXDataChannelAsset::ChannelName() const
    {
        return std::filesystem::path( m_Metadata.Filepath ).stem().string();
    }

    Common::ResultStr<std::filesystem::path> VFXDataChannelAsset::PathForName( std::string_view name )
    {
        const auto identifierChar = []( char c, bool first )
        {
            const bool letter = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || c == '_';
            return letter || ( !first && c >= '0' && c <= '9' );
        };
        bool ok = !name.empty();
        for ( std::size_t i = 0; ok && i < name.size(); ++i )
            ok = identifierChar( name[i], i == 0 );
        if ( !ok )
            return Common::MakeFormattedError<std::filesystem::path>(
                 "'{}' is not a VFX data channel name (the asset's name, e.g. \"Impacts\" - not a path)", name );
        return Common::MakeSuccess( Common::Constants::Path::VFX_PATH /
                                    ( std::string( name ) + Serialization::kVFXDataChannelExtension ) );
    }

    Common::BoolResultStr VFXDataChannelAsset::Save( const Common::Filepath&                  filepath,
                                                     const Serialization::VFXDataChannelData& data )
    {
        if ( auto ok = Serialization::SaveVFXDataChannelFile( filepath, data ); !ok )
            return ok;
        LOG_INFO( "[VFX] Data channel written: '{}', {} field(s).", filepath.string(), data.Fields.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
