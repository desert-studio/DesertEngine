#include "SparseVolumeImporter.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/SparseVolumeAsset.hpp>

#include <Common/Core/Logger.hpp>

#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace Desert::Editor
{
    std::filesystem::path SparseVolumeAssetPathFor( const std::filesystem::path& vol )
    {
        std::filesystem::path dsvol = vol;
        dsvol.replace_extension( Assets::kSparseVolumeExtension );
        return dsvol;
    }

    CookVerdict ImportSparseVolume( const std::filesystem::path& vol, const ImportPass pass, const bool force )
    {
        const std::filesystem::path dsvol = SparseVolumeAssetPathFor( vol );

        std::error_code ec;
        const bool      exists = std::filesystem::exists( dsvol, ec );
        if ( exists && !force )
        {
            const auto assetTime  = std::filesystem::last_write_time( dsvol, ec );
            const bool assetKnown = !ec;
            const auto sourceTime = std::filesystem::last_write_time( vol, ec );
            if ( assetKnown && !ec && assetTime >= sourceTime )
                return CookVerdict::UpToDate;
        }
        if ( pass == ImportPass::Cook )
        {
            LOG_ERROR(
                 "[Import] sparse volume '{}' has no current '{}'; re-import the .vol (drop it on the Content "
                 "Browser or Rebuild Cooked Assets).",
                 vol.string(), dsvol.filename().string() );
            return CookVerdict::Failed;
        }

        std::vector<unsigned char> source;
        {
            std::ifstream in( vol, std::ios::binary );
            if ( !in )
            {
                LOG_ERROR( "[Import] sparse volume '{}' could not be opened", vol.string() );
                return CookVerdict::Failed;
            }
            source.assign( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
        }
        auto built = Assets::ImportSparseVolumeCached( source, Assets::SparseVolumeImportSettings{} );
        if ( !built )
        {
            LOG_ERROR( "[Import] sparse volume '{}' was not imported: {}", vol.string(), built.GetError() );
            return CookVerdict::Failed;
        }
        Assets::SparseVolumeData data = built.ExtractValue();
        // The re-import's identity: the GUID of the `.dsvol` already there (EncodeSparseVolume mints one only when
        // the data carries none, i.e. on the first import).
        if ( exists )
            data.Guid = Assets::SparseVolumeAsset::ReadGuid( dsvol );

        const auto file = Assets::EncodeSparseVolume( data );
        if ( !file )
        {
            LOG_ERROR( "[Import] sparse volume '{}' was not encoded: {}", vol.string(), file.GetError() );
            return CookVerdict::Failed;
        }
        {
            std::ofstream                     out( dsvol, std::ios::binary | std::ios::trunc );
            const std::vector<unsigned char>& bytes = file.GetValue();
            out.write( reinterpret_cast<const char*>( bytes.data() ),
                       static_cast<std::streamsize>( bytes.size() ) );
            if ( !out )
            {
                LOG_ERROR( "[Import] sparse volume '{}' was not written", dsvol.string() );
                return CookVerdict::Failed;
            }
        }
        // UE: an imported asset is in the asset registry at once, so the volume's picker lists the `.dsvol` now.
        // A re-import of a volume already resident shows after the renderer's next residency (scene reload).
        Assets::ContentRegistry::NoteFile( dsvol );
        LOG_INFO( "[Import] sparse volume '{}' -> '{}' ({}x{}x{} voxels, {} of {} pages hold medium, peak {} K).",
                  vol.string(), dsvol.filename().string(), data.Resolution.x, data.Resolution.y, data.Resolution.z,
                  data.BrickCount(), data.PageCount(), data.PeakTemperature );
        return CookVerdict::Cooked;
    }
} // namespace Desert::Editor
