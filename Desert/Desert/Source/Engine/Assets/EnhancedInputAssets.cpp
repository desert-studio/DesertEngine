#include <Engine/Assets/EnhancedInputAssets.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Assets
{
    namespace
    {
        // The file's text through the packaged VFS first, then the disk — the one read both kinds share.
        std::string ReadInputAssetText( const std::filesystem::path& file )
        {
            if ( const auto packed =
                      Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
                 packed.has_value() )
                return packed.value();
            if ( auto read = Common::Utils::FileSystem::ReadFileContent( file ); read )
                return read.ExtractValue();
            return {};
        }

        void AdoptHeaderGuid( const std::optional<Common::Content::TextAssetHeaderSerialized>& header,
                              Common::Content::AssetGuid&                                      guid )
        {
            if ( header )
                if ( const auto parsed = Common::Content::AssetGuidFromText( header->Guid ); parsed )
                    guid = parsed.GetValue();
        }
    } // namespace

    InputActionAsset::InputActionAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::InputAction )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::BoolResultStr InputActionAsset::LoadFromFile()
    {
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        const std::string           text = ReadInputAssetText( file );
        m_Ready                          = false;
        if ( text.empty() )
            return Common::MakeFormattedError<bool>( "input action '{}' is empty or could not be opened",
                                                     file.string() );
        auto parsed = Serialization::ParseInputAction( text );
        if ( !parsed )
            return Common::MakeFormattedError<bool>( "input action '{}' is not usable: {}", file.string(),
                                                     parsed.GetError() );
        m_Data = parsed.ExtractValue();
        AdoptHeaderGuid( m_Data.Header, m_Guid );
        m_Ready = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr InputActionAsset::Unload()
    {
        m_Data  = Serialization::InputActionData{};
        m_Ready = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr InputActionAsset::Save( const Common::Filepath&               filepath,
                                                  const Serialization::InputActionData& data )
    {
        return Serialization::SaveInputActionFile( filepath, data );
    }

    InputMappingContextAsset::InputMappingContextAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::InputMappingContext )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::BoolResultStr InputMappingContextAsset::LoadFromFile()
    {
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        const std::string           text = ReadInputAssetText( file );
        m_Ready                          = false;
        if ( text.empty() )
            return Common::MakeFormattedError<bool>( "input mapping context '{}' is empty or could not be opened",
                                                     file.string() );
        auto parsed = Serialization::ParseInputMappingContext( text );
        if ( !parsed )
            return Common::MakeFormattedError<bool>( "input mapping context '{}' is not usable: {}", file.string(),
                                                     parsed.GetError() );
        m_Data = parsed.ExtractValue();
        AdoptHeaderGuid( m_Data.Header, m_Guid );
        m_Ready = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr InputMappingContextAsset::Unload()
    {
        m_Data  = Serialization::InputMappingContextData{};
        m_Ready = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr InputMappingContextAsset::Save( const Common::Filepath&                       filepath,
                                                          const Serialization::InputMappingContextData& data )
    {
        return Serialization::SaveInputMappingContextFile( filepath, data );
    }
} // namespace Desert::Assets
