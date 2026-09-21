#include "CloudNoiseService.hpp"

#include <Engine/Core/Formats/ImageFormat.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    void CloudNoiseService::Announce( const Assets::Asset<Assets::CloudNoiseVolumeAsset>& asset )
    {
        if ( !asset )
        {
            LOG_ERROR( "[Clouds] CloudNoiseService::Announce was given a null asset; nothing was recorded, "
                       "so whatever file this was will resolve to the default volume." );
            return;
        }

        const Assets::AssetHandle& handle = asset->GetMetadata().Handle;

        // ANNOUNCING AN ALREADY-KNOWN HANDLE KEEPS WHAT IS THERE. The re-scan after a cook ("Rebuild
        // Cooked Assets") walks the same directory again, and replacing the entry would throw away an
        // uploaded volume that nothing asked to be thrown away — every view bound to it would then spend
        // a frame pending for no reason the user can see.
        auto& entry = m_Volumes[handle];
        if ( !entry.Source )
            entry.Source = asset;
    }

    Common::BoolResultStr
    CloudNoiseService::Register( const std::shared_ptr<Assets::CloudNoiseVolumeAsset>& asset )
    {
        if ( !asset )
            return Common::MakeError( "CloudNoiseService::Register was given a null asset" );

        if ( !asset->IsReadyForUse() )
            return Common::MakeFormattedError<bool>( "cloud noise volume '{}' is not loaded",
                                                     asset->GetMetadata().Filepath.string() );

        const Assets::AssetHandle&          handle = asset->GetMetadata().Handle;
        const Assets::CloudNoiseVolumeData& data   = asset->GetVolume();

        if ( auto it = m_Volumes.find( handle );
             it != m_Volumes.end() && it->second.Volume && it->second.Revision == asset->GetRevision() )
            return BOOLSUCCESS;

        // A fresh image rather than an in-place update: the backend has no path for re-uploading a volume,
        // and the resolution itself may have changed. The previous image goes through Image3D's own
        // deletion queue when the last reference drops, which is what keeps a frame still reading it safe.
        const Core::Formats::Image3DSpecification spec{
             .Tag        = "CloudNoise:" + asset->GetMetadata().Filepath.filename().string(),
             .Width      = data.Params.Resolution,
             .Height     = data.Params.Resolution,
             .Depth      = data.Params.Resolution,
             .Format     = Core::Formats::ImageFormat::RGBA8F,
             .Data       = data.Voxels,
             .Properties = Core::Formats::Sample,
        };

        auto volume = Graphic::Image3D::Create( spec );
        if ( !volume )
            return Common::MakeFormattedError<bool>(
                 "the {0}x{0}x{0} RGBA8 volume for '{1}' could not be created on the device",
                 data.Params.Resolution, asset->GetMetadata().Filepath.string() );

        // Whose the 8 MiB volume is, in the ledger — see Engine/Graphic/ResourceLedger.hpp.
        volume->ClaimOwnership( Graphic::ResourceOwner::AssetService, handle );
        volume->RecordDeviceBytes( data.Voxels.size() );

        // THE ENTRY IS UPDATED, NOT REPLACED, and that is not tidiness: replacing it would drop the
        // `LoadRequest` this call is very likely running inside the completion of, and destroying a
        // request from within its own delegate is the reentrancy the deferred-completion rule exists to
        // keep out of this code.
        Entry& entry   = m_Volumes[handle];
        entry.Volume   = std::move( volume );
        entry.Revision = asset->GetRevision();
        entry.Failed   = false;
        if ( !entry.Source )
            entry.Source = asset;

        LOG_INFO( "[Clouds] Noise volume '{}' uploaded: {}^3 RGBA8, {:.2f} MiB on the device.",
                  asset->GetMetadata().Filepath.string(), data.Params.Resolution,
                  static_cast<double>( data.Voxels.size() ) / ( 1024.0 * 1024.0 ) );
        return BOOLSUCCESS;
    }

    void CloudNoiseService::SetDefault( const Assets::AssetHandle& handle )
    {
        m_Default = handle;
    }

    void CloudNoiseService::BeginRead( const Assets::AssetHandle& handle, Entry& entry )
    {
        // BOTH DELEGATES, AND THE CANCEL ONE IS NOT DECORATION HERE. `Clear()` drops every request when a
        // project closes; without a cancel delegate this service would keep an entry marked "being read"
        // forever and every later `Require` for that handle would answer Pending at a worker that is
        // never coming back — the exact wedge T2.3 makes the delegate mandatory to prevent.
        entry.Request = Assets::AsyncAssetLoader::Get().Request(
             entry.Source,
             [this, handle]( const Assets::Asset<Assets::AssetBase>& loaded, const Assets::LoadOutcome outcome,
                             const std::string& error )
             {
                 const auto it = m_Volumes.find( handle );
                 if ( it == m_Volumes.end() )
                     return; // cleared while the read was running; there is nothing to fill

                 // RELEASED FIRST. `Register` below may run for a long time (an 8 MiB device upload), and
                 // leaving a settled request in the entry would make `Require` answer Pending for a
                 // volume that is already on the device.
                 it->second.Request.Release();

                 if ( outcome != Assets::LoadOutcome::Loaded )
                 {
                     it->second.Failed = true;
                     LOG_ERROR( "[Clouds] Noise volume '{}' could not be read: {}. Every layer naming it "
                                "falls back to the default volume from here on; it is not retried, because "
                                "a retry per frame on a corrupt file is a frame-rate defect that reads as "
                                "a rendering one.",
                                it->second.Source ? it->second.Source->GetMetadata().Filepath.string()
                                                  : std::string( "<unknown>" ),
                                error );
                     return;
                 }

                 const auto asset    = std::static_pointer_cast<Assets::CloudNoiseVolumeAsset>( loaded );
                 const auto uploaded = Register( asset );

                 // RE-FOUND, NOT REUSED. `Register` writes into the same map, so the iterator above is
                 // only guaranteed valid while nothing inserts — and "it cannot insert because the key
                 // is already there" is exactly the kind of reasoning that stops being true when
                 // somebody adds a line. One lookup is cheaper than the hour spent on the crash.
                 if ( !uploaded )
                 {
                     const auto entry = m_Volumes.find( handle );
                     if ( entry != m_Volumes.end() )
                         entry->second.Failed = true;
                     LOG_ERROR( "[Clouds] Noise volume '{}' was read but could not be uploaded: {}",
                                asset->GetMetadata().Filepath.string(), uploaded.GetError() );
                 }
             },
             [this, handle]
             {
                 const auto it = m_Volumes.find( handle );
                 if ( it == m_Volumes.end() )
                     return;
                 // BACK TO "NOT ASKED FOR", not to "failed". A cancel says the caller stopped wanting it,
                 // which is a different fact from the file being unreadable: the next `Require` should
                 // start a fresh read rather than inherit a refusal nobody measured.
                 it->second.Request.Release();
             } );
    }

    Assets::AssetRef<Graphic::Image3D> CloudNoiseService::Resolve( const Assets::AssetHandle& handle,
                                                                   const bool                 mayRequest )
    {
        if ( handle != 0 )
        {
            const auto it = m_Volumes.find( handle );
            if ( it != m_Volumes.end() )
            {
                if ( it->second.Volume )
                    return Assets::AssetRef<Graphic::Image3D>::Ready( handle, it->second.Volume );

                if ( it->second.Request.IsValid() )
                    return Assets::AssetRef<Graphic::Image3D>::Pending( handle );

                if ( !it->second.Failed && it->second.Source )
                {
                    if ( !mayRequest )
                        return Assets::AssetRef<Graphic::Image3D>::Pending( handle );

                    BeginRead( handle, it->second );
                    return Assets::AssetRef<Graphic::Image3D>::Pending( handle );
                }
                // Announced, read, failed: fall through to the default exactly as an absent one does.
            }
            else if ( mayRequest && m_ReportedMissing.insert( handle ).second )
            {
                // NOT a silent fall-through, and still logged ONCE per handle rather than once per
                // frame. The artist chose a volume and it is not in the project; saying which handle is
                // the difference between a five-minute fix and an afternoon.
                LOG_ERROR( "[Clouds] Noise volume {} is referenced but was never announced — falling back "
                           "to the default volume. The scene names a .dcnv the asset scan did not find.",
                           static_cast<uint64_t>( handle ) );
            }
        }

        if ( m_Default != 0 )
        {
            const auto it = m_Volumes.find( m_Default );
            if ( it != m_Volumes.end() )
            {
                if ( it->second.Volume )
                    return Assets::AssetRef<Graphic::Image3D>::Ready( m_Default, it->second.Volume );

                if ( it->second.Request.IsValid() )
                    return Assets::AssetRef<Graphic::Image3D>::Pending( m_Default );

                if ( !it->second.Failed && it->second.Source )
                {
                    if ( !mayRequest )
                        return Assets::AssetRef<Graphic::Image3D>::Pending( m_Default );

                    BeginRead( m_Default, it->second );
                    return Assets::AssetRef<Graphic::Image3D>::Pending( m_Default );
                }
            }
        }

        if ( mayRequest && !m_ReportedMissingDefault )
        {
            m_ReportedMissingDefault = true;
            LOG_ERROR( "[Clouds] No default noise volume is available, so the clouds cannot be shaped and "
                       "will not draw. Expected a .dcnv under the project's Assets/Clouds folder." );
        }
        return Assets::AssetRef<Graphic::Image3D>::Null();
    }

    Assets::AssetRef<Graphic::Image3D> CloudNoiseService::Require( const Assets::AssetHandle& handle )
    {
        return Resolve( handle, /*mayRequest=*/true );
    }

    Assets::AssetRef<Graphic::Image3D> CloudNoiseService::Peek( const Assets::AssetHandle& handle )
    {
        return Resolve( handle, /*mayRequest=*/false );
    }

    size_t CloudNoiseService::ResidentCount() const
    {
        size_t resident = 0;
        for ( const auto& [handle, entry] : m_Volumes )
        {
            if ( entry.Volume )
                ++resident;
        }
        return resident;
    }

    void CloudNoiseService::Clear()
    {
        // CANCELLED RATHER THAN DROPPED, and the difference reaches the log. `Clear()` is a project or a
        // scene going away while its content is still being read; the cancel delegate is how each entry
        // learns that the worker it was waiting for is not coming, and cancelling before a worker picks
        // the job up is what stops a closed scene sitting through its own reads.
        for ( auto& [handle, entry] : m_Volumes )
            entry.Request.Cancel();

        m_Volumes.clear();
        m_Default                = Assets::AssetHandle{ 0 };
        m_ReportedMissingDefault = false;
        m_ReportedMissing.clear();
    }
} // namespace Desert::Runtime
