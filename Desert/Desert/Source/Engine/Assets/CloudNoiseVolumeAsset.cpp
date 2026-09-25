#include <Engine/Assets/CloudNoiseVolumeAsset.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <fstream>
#include <optional>
#include <span>
#include <sstream>

namespace Desert::Assets
{
    CloudNoiseVolumeAsset::CloudNoiseVolumeAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::CloudNoiseVolume )
    {
        // THE VOLUME'S IDENTITY IS ITS ENVELOPE GUID (container 3), adopted HERE rather than in the load, for
        // the layout's reason: the asset manager keys its handle lookup at creation. A file with no readable
        // header (absent: Save is about to create it; or a bare container) keeps the path-derived handle -
        // the load refuses the latter by name, so no volume is ever READY under that handle.
        const auto guid = ReadCloudNoiseVolumeGuid( m_Metadata.Filepath );
        if ( guid.IsNull() )
            return;
        AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) ),
                             Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    Common::Content::AssetGuid CloudNoiseVolumeAsset::ReadCloudNoiseVolumeGuid( const Common::Filepath& filepath )
    {
        // Only the envelope header is read, through the VFS first like the load.
        namespace CC                              = Common::Content;
        const CC::SubsystemVersion       kKnown[] = { { kCloudNoiseSubsystemTag, kCloudNoiseContainerVersion } };
        const CC::AssetHeaderReadContext context{ kKnown };

        std::optional<Common::ResultStr<CC::EnvelopeHeader>> header;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( filepath ) ? Common::Utils::VFS::ReadFile( filepath ) : std::nullopt;
             packed.has_value() )
        {
            std::istringstream in( *packed );
            header.emplace( CC::ReadEnvelopeHeader( in, context ) );
        }
        else if ( std::ifstream in( filepath, std::ios::binary ); in )
            header.emplace( CC::ReadEnvelopeHeader( in, context ) );

        if ( !header || !*header || header->GetValue().Asset.Kind != CC::ContentKind::CloudNoiseVolume )
            return {};
        return header->GetValue().Asset.Guid;
    }

    Common::BoolResultStr CloudNoiseVolumeAsset::LoadFromFile()
    {
        const std::string path = m_Metadata.Filepath.string();

        // Through the VFS rather than straight off the disk, so a packaged build reads the volume out of
        // its .dpak exactly like every other asset. `ReadFile` returns the bytes as a std::string; a
        // container is binary, and std::string is byte-transparent, so the conversion is a copy and not an
        // interpretation.
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
                return Common::MakeFormattedError<bool>( "Cloud noise volume '{}' could not be opened", path );

            bytes.assign( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() );
        }

        auto decoded = DecodeCloudNoiseVolume( bytes );
        if ( !decoded )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "Cloud noise volume '{}' is not usable: {}", path,
                                                     decoded.GetError() );
        }

        m_Volume = decoded.ExtractValue();
        m_Ready  = true;
        ++m_Revision;

        // THE LOG MUST NOT RECITE A RECIPE THAT DOES NOT EXIST. An imported volume's seed and periods are
        // zero placeholders, and printing them in the shape of a recipe would put a line in the log that
        // reads exactly like a generated volume with unusual settings — which is the sort of false evidence
        // somebody eventually spends an afternoon chasing.
        if ( m_Volume.Origin == CloudNoiseVolumeOrigin::Imported )
        {
            LOG_INFO( "[Clouds] Noise volume '{}' loaded: {}^3 RGBA8, imported from a slice sheet - it has no "
                      "generator recipe, so it cannot be re-baked, only replaced.",
                      path, m_Volume.Params.Resolution );
        }
        else
        {
            LOG_INFO( "[Clouds] Noise volume '{}' loaded: {}^3 RGBA8, seed {}, generator v{}, periods "
                      "{:.0f}/{:.0f} wispy and {:.0f}/{:.0f} billowy, curl {:.2f}.",
                      path, m_Volume.Params.Resolution, m_Volume.Params.Seed, m_Volume.GeneratorVersion,
                      m_Volume.Params.WispyPeriodLowFrequency, m_Volume.Params.WispyPeriodHighFrequency,
                      m_Volume.Params.BillowPeriodLowFrequency, m_Volume.Params.BillowPeriodHighFrequency,
                      m_Volume.Params.CurlStrength );

            // Said out loud rather than tolerated. A volume baked by an older generator still decodes and
            // still renders — the container did not change — but it is NOT what this build's maths produces,
            // and an artist comparing two volumes is entitled to know that one of them predates the noise
            // itself. Asked only of GENERATED volumes: an imported one's version is zero on purpose, and
            // warning about that would be warning about a fact the line above already states.
            if ( m_Volume.GeneratorVersion != kCloudNoiseGeneratorVersion )
                LOG_WARN( "[Clouds] Noise volume '{}' was baked by generator v{}; this build generates v{}. "
                          "Its channels are whatever the older maths produced — re-bake it to compare like "
                          "with like.",
                          path, m_Volume.GeneratorVersion, kCloudNoiseGeneratorVersion );
        }

        return BOOLSUCCESS;
    }

    Common::BoolResultStr CloudNoiseVolumeAsset::Unload()
    {
        m_Volume.Voxels.clear();
        m_Volume.Voxels.shrink_to_fit();

        // The 8 MiB was already released correctly; what stayed behind was everything the FILE said about
        // it — the origin, the generator version, and a Params block still carrying the loaded file's
        // seed and periods on an asset that reports itself not ready. Reset wholesale, so a member added
        // to CloudNoiseVolumeData tomorrow cannot silently become the next survivor.
        //
        // The result is the RECIPE'S DEFAULTS, not zeroes, and that is right rather than a compromise:
        // Params is what a volume would be GENERATED from, not a description of the buffer that was
        // freed, so a released asset ends up in the same state a never-loaded one is in. The two facts a
        // caller may act on are `Voxels.empty()` and `IsReadyForUse()`, and both now say released.
        m_Volume = CloudNoiseVolumeData{};
        m_Ready  = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr CloudNoiseVolumeAsset::Save( const Common::Filepath&     filepath,
                                                       const CloudNoiseVolumeData& volume )
    {
        // THE SAME SPLIT THE DECODER MAKES, for the same reason and so the two cannot disagree about what is
        // writable. An imported volume's recipe is empty by construction; asking the generator's question of
        // it would refuse every import at the moment the artist tried to keep it.
        if ( volume.Origin == CloudNoiseVolumeOrigin::Generated )
        {
            if ( auto valid = ValidateCloudNoiseVolumeParams( volume.Params ); !valid )
                return Common::MakeFormattedError<bool>( "refusing to write '{}': {}", filepath.string(),
                                                         valid.GetError() );
        }
        else
        {
            if ( auto valid = ValidateCloudNoiseVolumeResolution( volume.Params.Resolution ); !valid )
                return Common::MakeFormattedError<bool>( "refusing to write '{}': {}", filepath.string(),
                                                         valid.GetError() );
        }

        const uint64_t expected = volume.VoxelCount() * 4u;
        if ( volume.Voxels.size() != expected )
            return Common::MakeFormattedError<bool>(
                 "refusing to write '{}': {} voxel bytes for a {}^3 volume, which needs {}", filepath.string(),
                 volume.Voxels.size(), volume.Params.Resolution, expected );

        std::error_code ec;
        if ( filepath.has_parent_path() )
            std::filesystem::create_directories( filepath.parent_path(), ec );

        // A RE-BAKE OVER AN EXISTING VOLUME KEEPS THAT FILE'S GUID, so every reference to it (and the handle
        // a loaded asset already adopted) survives the regeneration. A new file mints one; the caller's own
        // GUID is never copied to a different path, which would give two files one identity.
        CloudNoiseVolumeData written = volume;
        written.Guid                 = ReadCloudNoiseVolumeGuid( filepath );
        const auto encodedResult     = EncodeCloudNoiseVolume( written );
        if ( !encodedResult )
            return Common::MakeFormattedError<bool>( "refusing to write '{}': {}", filepath.string(),
                                                     encodedResult.GetError() );
        const std::vector<unsigned char>& encoded = encodedResult.GetValue();

        // Through the write primitive, not a local std::ofstream (Д35): the local stream's flush is its
        // destructor, which runs after this function has already returned BOOLSUCCESS. A `.dcnv` is tens
        // of megabytes, so most of it does reach the OS during the write — but the tail does not, and a
        // volume filling up mid-bake produced a green save and a volume the decoder later refuses.
        if ( const auto written = Common::Utils::FileSystem::WriteBytesToFileAtomic(
                  filepath, std::as_bytes( std::span( encoded ) ) );
             !written )
            return Common::MakeFormattedError<bool>( "'{}' ({} bytes) could not be written: {}", filepath.string(),
                                                     encoded.size(), written.GetError() );

        LOG_INFO( "[Clouds] Noise volume written: '{}', {}^3 RGBA8, {} bytes, seed {}.", filepath.string(),
                  volume.Params.Resolution, encoded.size(), volume.Params.Seed );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
