#include <Engine/Assets/CloudModellingVolumeAsset.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <fstream>
#include <optional>
#include <span>
#include <sstream>

namespace Desert::Assets
{
    CloudModellingVolumeAsset::CloudModellingVolumeAsset( AssetPriority           priority,
                                                          const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::CloudModellingVolume )
    {
        // THE VOLUME'S IDENTITY IS ITS ENVELOPE GUID (container 3), adopted HERE rather than in the load, for
        // the layout's reason: the asset manager keys its handle lookup at creation. A file with no readable
        // header (absent: Save is about to create it; or a bare container) keeps the path-derived handle -
        // the load refuses the latter by name, so no volume is ever READY under that handle.
        const auto guid = ReadCloudModellingVolumeGuid( m_Metadata.Filepath );
        if ( guid.IsNull() )
            return;
        AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) ),
                             Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    Common::Content::AssetGuid
    CloudModellingVolumeAsset::ReadCloudModellingVolumeGuid( const Common::Filepath& filepath )
    {
        // Only the envelope header is read, through the VFS first like the load.
        std::optional<Common::ResultStr<Common::Content::AssetGuid>> guid;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( filepath ) ? Common::Utils::VFS::ReadFile( filepath ) : std::nullopt;
             packed.has_value() )
        {
            std::istringstream in( *packed );
            guid.emplace( Assets::ReadCloudModellingVolumeGuid( in ) );
        }
        else if ( std::ifstream in( filepath, std::ios::binary ); in )
            guid.emplace( Assets::ReadCloudModellingVolumeGuid( in ) );

        if ( !guid || !*guid )
            return {};
        return guid->GetValue();
    }

    Common::BoolResultStr CloudModellingVolumeAsset::LoadFromFile()
    {
        const std::string path = m_Metadata.Filepath.string();

        // Through the VFS rather than straight off the disk, so a packaged build reads the volume out of
        // its .dpak exactly like every other asset.
        const auto contents = Common::Utils::VFS::Exists( m_Metadata.Filepath )
                                   ? Common::Utils::VFS::ReadFile( m_Metadata.Filepath )
                                   : std::nullopt;

        std::vector<unsigned char> bytes;
        if ( contents.has_value() )
        {
            bytes.assign( contents->begin(), contents->end() );
        }
        else
        {
            std::ifstream file( m_Metadata.Filepath, std::ios::binary );
            if ( !file )
                return Common::MakeFormattedError<bool>( "Cloud modelling volume '{}' could not be opened", path );

            bytes.assign( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() );
        }

        auto decoded = DecodeCloudModellingVolume( bytes );
        if ( !decoded )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "Cloud modelling volume '{}' is not usable: {}", path,
                                                     decoded.GetError() );
        }

        m_Volume = decoded.ExtractValue();
        m_Ready  = true;
        ++m_Revision;

        LOG_INFO( "[Clouds] Modelling volume '{}' loaded: {}x{}x{} RGBA8 over {:.2f} x {:.2f} x {:.2f} km "
                  "({:.1f} m per voxel horizontally), {} lumps, blend {:.0f} m, generator v{}.",
                  path, kCloudModellingVolumeWidth, kCloudModellingVolumeHeight, kCloudModellingVolumeDepth,
                  m_Volume.Recipe.SizeKm.x, m_Volume.Recipe.SizeKm.y, m_Volume.Recipe.SizeKm.z,
                  m_Volume.Recipe.SizeKm.x * 1000.0f / static_cast<float>( kCloudModellingVolumeWidth ),
                  m_Volume.Recipe.Blobs.size(), m_Volume.Recipe.BlendRadiusKm * 1000.0f,
                  m_Volume.GeneratorVersion );

        // Said out loud rather than tolerated. A volume baked by an older generator still decodes and still
        // renders — the container did not change — but it is NOT what this build's maths produces, and an
        // artist comparing two clouds is entitled to know that one of them predates the sculpting itself.
        if ( m_Volume.GeneratorVersion != kCloudModellingGeneratorVersion )
            LOG_WARN( "[Clouds] Modelling volume '{}' was baked by generator v{}; this build generates v{}. Its "
                      "channels are whatever the older maths produced — re-bake it with Tools/CloudVolumeBaker "
                      "to compare like with like.",
                      path, m_Volume.GeneratorVersion, kCloudModellingGeneratorVersion );

        return BOOLSUCCESS;
    }

    Common::BoolResultStr CloudModellingVolumeAsset::Unload()
    {
        m_Volume.Voxels.clear();
        m_Volume.Voxels.shrink_to_fit();

        // THE RECIPE'S `Blobs` IS A HEAP MEMBER TOO — up to 64 lumps — and it was the one allocation this
        // body left behind. The whole volume is reset rather than the two vectors picked off, so a member
        // added to CloudModellingVolumeData tomorrow does not silently become the next survivor: the
        // sculpting tool reads the recipe back out of the asset, and it re-reads it through Load like
        // everything else.
        m_Volume = CloudModellingVolumeData{};
        m_Ready  = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr CloudModellingVolumeAsset::Save( const Common::Filepath&         filepath,
                                                           const CloudModellingVolumeData& volume )
    {
        if ( auto valid = ValidateCloudModellingRecipe( volume.Recipe ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write '{}': {}", filepath.string(),
                                                     valid.GetError() );

        if ( volume.Voxels.size() != kCloudModellingVoxelBytes )
            return Common::MakeFormattedError<bool>(
                 "refusing to write '{}': {} voxel bytes, and a {}x{}x{} RGBA8 volume is {}", filepath.string(),
                 volume.Voxels.size(), kCloudModellingVolumeWidth, kCloudModellingVolumeHeight,
                 kCloudModellingVolumeDepth, kCloudModellingVoxelBytes );

        std::error_code ec;
        if ( filepath.has_parent_path() )
            std::filesystem::create_directories( filepath.parent_path(), ec );

        // A RE-BAKE OVER AN EXISTING VOLUME KEEPS THAT FILE'S GUID, so every reference to it (and the handle
        // a loaded asset already adopted) survives the regeneration. A new file mints one; the caller's own
        // GUID is never copied to a different path, which would give two files one identity.
        CloudModellingVolumeData written = volume;
        written.Guid                     = ReadCloudModellingVolumeGuid( filepath );
        const auto encodedResult         = EncodeCloudModellingVolume( written );
        if ( !encodedResult )
            return Common::MakeFormattedError<bool>( "refusing to write '{}': {}", filepath.string(),
                                                     encodedResult.GetError() );
        const std::vector<unsigned char>& encoded = encodedResult.GetValue();

        // Through the write primitive, not a local std::ofstream (Д35): the local stream's flush is its
        // destructor, which runs after this function has already returned BOOLSUCCESS.
        if ( const auto written = Common::Utils::FileSystem::WriteBytesToFileAtomic(
                  filepath, std::as_bytes( std::span( encoded ) ) );
             !written )
            return Common::MakeFormattedError<bool>( "'{}' ({} bytes) could not be written: {}", filepath.string(),
                                                     encoded.size(), written.GetError() );

        LOG_INFO( "[Clouds] Modelling volume written: '{}', {} lumps, {} bytes.", filepath.string(),
                  volume.Recipe.Blobs.size(), encoded.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
