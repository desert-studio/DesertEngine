#include <Engine/Assets/PhysicsAsset.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <fstream>
#include <optional>
#include <span>
#include <sstream>

namespace Desert::Assets
{
    PhysicsAsset::PhysicsAsset( const Common::Filepath& filepath )
         : AssetBase( filepath, AssetTypeID::PhysicsAsset )
    {
        // THE PHYSICS ASSET'S IDENTITY IS ITS ENVELOPE GUID, adopted at construction because the asset manager
        // keys its handle lookup here (CloudModellingVolumeAsset's reason). A file with no readable header
        // (absent: Save is about to create it) keeps the path-derived handle; the load refuses a bad one.
        const auto guid = ReadPhysicsAssetGuid( m_Metadata.Filepath );
        if ( guid.IsNull() )
            return;
        AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) ),
                             Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    Common::Content::AssetGuid PhysicsAsset::ReadPhysicsAssetGuid( const Common::Filepath& filepath )
    {
        std::optional<Common::ResultStr<Common::Content::AssetGuid>> guid;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( filepath ) ? Common::Utils::VFS::ReadFile( filepath ) : std::nullopt;
             packed.has_value() )
        {
            std::istringstream in( *packed );
            guid.emplace( Physics::ReadPhysicsAssetGuid( in ) );
        }
        else if ( std::ifstream in( filepath, std::ios::binary ); in )
            guid.emplace( Physics::ReadPhysicsAssetGuid( in ) );

        if ( !guid || !*guid )
            return {};
        return guid->GetValue();
    }

    Common::BoolResultStr PhysicsAsset::LoadFromFile()
    {
        const std::string path = m_Metadata.Filepath.string();

        // Through the VFS first, so a packaged build reads the physics asset out of its .dpak like every asset.
        const auto contents = Common::Utils::VFS::Exists( m_Metadata.Filepath )
                                   ? Common::Utils::VFS::ReadFile( m_Metadata.Filepath )
                                   : std::nullopt;

        std::vector<unsigned char> bytes;
        if ( contents.has_value() )
            bytes.assign( contents->begin(), contents->end() );
        else
        {
            std::ifstream file( m_Metadata.Filepath, std::ios::binary );
            if ( !file )
                return Common::MakeFormattedError<bool>( "Physics asset '{}' could not be opened", path );
            bytes.assign( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() );
        }

        auto decoded = Physics::DecodePhysicsAsset( bytes );
        if ( !decoded )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "Physics asset '{}' is not usable: {}", path,
                                                     decoded.GetError() );
        }

        m_Data  = decoded.ExtractValue();
        m_Ready = true;
        LOG_INFO( "[Physics] Physics asset '{}' loaded: {} bodies, {} constraints.", path, m_Data.Bodies.size(),
                  m_Data.Constraints.size() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr PhysicsAsset::Unload()
    {
        // The whole value is reset, so a member added to PhysicsAssetData later cannot survive an unload.
        m_Data  = Physics::PhysicsAssetData{};
        m_Ready = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr PhysicsAsset::Save( const Common::Filepath&          filepath,
                                              const Physics::PhysicsAssetData& asset )
    {
        std::error_code ec;
        if ( filepath.has_parent_path() )
            std::filesystem::create_directories( filepath.parent_path(), ec );

        // A SAVE OVER THE FILE KEEPS ITS GUID; a new file mints one (the envelope refuses a null identity).
        Physics::PhysicsAssetData written = asset;
        written.Guid                      = ReadPhysicsAssetGuid( filepath );
        if ( written.Guid.IsNull() )
            written.Guid = Common::Content::AssetGuid::Generate();

        const auto encodedResult = Physics::EncodePhysicsAsset( written );
        if ( !encodedResult )
            return Common::MakeFormattedError<bool>( "refusing to write '{}': {}", filepath.string(),
                                                     encodedResult.GetError() );
        const std::vector<unsigned char>& encoded = encodedResult.GetValue();

        // Through the atomic write primitive: a local ofstream flushes in its destructor, after the return.
        if ( const auto done = Common::Utils::FileSystem::WriteBytesToFileAtomic(
                  filepath, std::as_bytes( std::span( encoded ) ) );
             !done )
            return Common::MakeFormattedError<bool>( "'{}' ({} bytes) could not be written: {}", filepath.string(),
                                                     encoded.size(), done.GetError() );

        LOG_INFO( "[Physics] Physics asset written: '{}', {} bodies, {} bytes.", filepath.string(),
                  asset.Bodies.size(), encoded.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
