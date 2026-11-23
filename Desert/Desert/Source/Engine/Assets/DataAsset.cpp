#include <Engine/Assets/DataAsset.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Serialization/DataAsset.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Assets
{
    DataAsset::DataAsset( const Common::Filepath& filepath ) : AssetBase( filepath, AssetTypeID::DataAsset )
    {
        if ( const TextAssetIdentity identity = ReadTextAssetIdentity( m_Metadata.Filepath );
             !identity.Guid.IsNull() )
        {
            m_Guid = identity.Guid;
            AdoptHandleFromFile( identity.Handle(), identity.StableKey() );
        }
    }

    Common::BoolResultStr DataAsset::LoadFromFile()
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

        // A FAILED RE-READ KEEPS THE LAST GOOD PAYLOAD: m_Ready is left as it was, so a hot reload of a
        // half-saved file never takes the weapon out from under the game.
        if ( text.empty() )
            return Common::MakeFormattedError<bool>( "data asset '{}' is empty or could not be opened", path );
        auto parsed = Serialization::ParseDataAsset( text );
        if ( !parsed )
            return Common::MakeFormattedError<bool>( "data asset '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        Serialization::DataAssetContents contents = parsed.ExtractValue();

        if ( m_Payload && m_Type == contents.Type )
            m_Type->Assign( m_Payload.get(), contents.Payload.get() ); // every reader's pointer stays valid
        else
        {
            m_Type    = contents.Type;
            m_Payload = std::move( contents.Payload );
        }
        m_Header = std::move( contents.Header );
        if ( m_Header )
            if ( const auto guid = Common::Content::AssetGuidFromText( m_Header->Guid ); guid )
                m_Guid = guid.GetValue();
        ++m_Revision;
        m_Ready = true;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr DataAsset::Unload()
    {
        m_Payload.reset();
        m_Type  = nullptr;
        m_Ready = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr DataAsset::Check( std::type_index wanted ) const
    {
        if ( !m_Ready || !m_Payload || m_Type == nullptr )
            return Common::MakeFormattedError<bool>( "data asset '{}' is not loaded",
                                                     m_Metadata.Filepath.string() );
        if ( m_Type->Type != wanted )
            return Common::MakeFormattedError<bool>( "data asset '{}' holds a '{}', not the type asked for ({})",
                                                     m_Metadata.Filepath.string(), m_Type->Name, wanted.name() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr DataAsset::Save() const
    {
        if ( !m_Ready || m_Type == nullptr )
            return Common::MakeFormattedError<bool>( "data asset '{}' is not loaded; nothing to save",
                                                     m_Metadata.Filepath.string() );
        if ( auto saved =
                  Serialization::SaveDataAssetFile( m_Metadata.Filepath, m_Header, *m_Type, m_Payload.get() );
             !saved )
            return saved;
        LOG_INFO( "[DataAsset] '{}' written ({}).", m_Metadata.Filepath.string(), m_Type->Name );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr DataAsset::CreateDefault( const Common::Filepath& filepath, const std::string& typeName )
    {
        const DataAssetType* type = DataAssetTypeRegistry::Get().Find( typeName );
        if ( type == nullptr )
            return Common::MakeFormattedError<bool>( "no module registers a data asset type '{}'", typeName );
        if ( std::error_code ec; std::filesystem::exists( filepath, ec ) )
            return Common::MakeFormattedError<bool>( "'{}' already exists", filepath.string() );
        const std::shared_ptr<void> payload = type->Make();
        if ( auto saved = Serialization::SaveDataAssetFile( filepath, std::nullopt, *type, payload.get() );
             !saved )
            return saved;
        LOG_INFO( "[DataAsset] '{}' created ({}).", filepath.string(), typeName );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
