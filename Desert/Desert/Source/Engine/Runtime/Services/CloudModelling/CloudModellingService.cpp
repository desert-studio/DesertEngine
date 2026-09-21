#include "CloudModellingService.hpp"

#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/Clouds/CloudAuthoredPayload.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    void CloudModellingService::Announce( const Assets::Asset<Assets::CloudModellingVolumeAsset>& asset )
    {
        if ( !asset )
        {
            LOG_ERROR( "[Clouds] CloudModellingService::Announce was given a null asset; nothing was "
                       "recorded, so whatever body this was will not draw and the hero cloud naming it "
                       "will be reported as a missing reference." );
            return;
        }

        auto& entry = m_Volumes[asset->GetMetadata().Handle];
        if ( !entry.Asset )
            entry.Asset = asset;
    }

    Common::BoolResultStr
    CloudModellingService::Register( const std::shared_ptr<Assets::CloudModellingVolumeAsset>& asset )
    {
        if ( !asset )
            return Common::MakeError( "CloudModellingService::Register was given a null asset" );

        if ( !asset->IsReadyForUse() )
            return Common::MakeFormattedError<bool>( "cloud modelling volume '{}' is not loaded",
                                                     asset->GetMetadata().Filepath.string() );

        const Assets::AssetHandle&              handle = asset->GetMetadata().Handle;
        const Assets::CloudModellingVolumeData& data   = asset->GetVolume();

        if ( data.Voxels.size() != static_cast<size_t>( Assets::kCloudModellingVoxelBytes ) )
            return Common::MakeFormattedError<bool>( "'{}' decoded to {} bytes where a modelling volume is {}",
                                                     asset->GetMetadata().Filepath.string(), data.Voxels.size(),
                                                     Assets::kCloudModellingVoxelBytes );

        if ( auto it = m_Volumes.find( handle );
             it != m_Volumes.end() && it->second.Loaded && it->second.Revision == asset->GetRevision() )
            return BOOLSUCCESS;

        // Updated rather than replaced: replacing would drop the `LoadRequest` this call is very likely
        // running inside the completion of. See CloudNoiseService::Register for the whole reason.
        Entry& entry   = m_Volumes[handle];
        entry.Asset    = asset;
        entry.SizeKm   = data.Recipe.SizeKm;
        entry.Revision = asset->GetRevision();
        entry.Loaded   = true;
        entry.Failed   = false;

        LOG_INFO( "[Clouds] Modelling volume '{}' registered: {}x{}x{} RGBA8, {:.2f} MiB, "
                  "{:.2f} x {:.2f} x {:.2f} km as sculpted.",
                  asset->GetMetadata().Filepath.string(), Assets::kCloudModellingVolumeWidth,
                  Assets::kCloudModellingVolumeHeight, Assets::kCloudModellingVolumeDepth,
                  static_cast<double>( data.Voxels.size() ) / ( 1024.0 * 1024.0 ), data.Recipe.SizeKm.x,
                  data.Recipe.SizeKm.y, data.Recipe.SizeKm.z );
        return BOOLSUCCESS;
    }

    void CloudModellingService::BeginRead( const Assets::AssetHandle& handle, Entry& entry )
    {
        entry.Request = Assets::AsyncAssetLoader::Get().Request(
             entry.Asset,
             [this, handle]( const Assets::Asset<Assets::AssetBase>& loaded, const Assets::LoadOutcome outcome,
                             const std::string& error )
             {
                 const auto it = m_Volumes.find( handle );
                 if ( it == m_Volumes.end() )
                     return;

                 it->second.Request.Release();

                 if ( outcome != Assets::LoadOutcome::Loaded )
                 {
                     it->second.Failed = true;
                     LOG_ERROR( "[Clouds] Cloud modelling volume '{}' could not be read: {}. Every hero "
                                "cloud naming it stays undrawn from here on.",
                                it->second.Asset ? it->second.Asset->GetMetadata().Filepath.string()
                                                 : std::string( "<unknown>" ),
                                error );
                     return;
                 }

                 const auto asset      = std::static_pointer_cast<Assets::CloudModellingVolumeAsset>( loaded );
                 const auto registered = Register( asset );
                 if ( !registered )
                 {
                     const auto entryAfter = m_Volumes.find( handle );
                     if ( entryAfter != m_Volumes.end() )
                         entryAfter->second.Failed = true;
                     LOG_ERROR( "[Clouds] Cloud modelling volume '{}' was read but could not be "
                                "registered: {}",
                                asset->GetMetadata().Filepath.string(), registered.GetError() );
                 }
             },
             [this, handle]
             {
                 const auto it = m_Volumes.find( handle );
                 if ( it == m_Volumes.end() )
                     return;
                 it->second.Request.Release();
             } );
    }

    Assets::AssetRef<Assets::CloudModellingVolumeAsset>
    CloudModellingService::Resolve( const Assets::AssetHandle& handle, const bool mayRequest )
    {
        // AN EMPTY SLOT IS SILENCE, not an error and not a default. There is no built-in hero cloud and
        // there must not be one: a body nobody sculpted appearing in a scene is a cloud the artist cannot
        // explain, where an empty noise slot resolving to the shipped default is a sky they expect.
        if ( handle == 0 )
            return Assets::AssetRef<Assets::CloudModellingVolumeAsset>::Null();

        const auto it = m_Volumes.find( handle );
        if ( it == m_Volumes.end() )
        {
            // NOT a silent fall-through, and ONCE per handle rather than once per frame — the version
            // this replaces had no such guard and printed this line sixty times a second.
            if ( mayRequest && m_Reported.insert( handle ).second )
                LOG_ERROR( "[Clouds] Cloud modelling volume {} is referenced by a hero cloud but was never "
                           "announced, so that cloud will not draw. The scene names a .dcmv the asset scan "
                           "did not find.",
                           static_cast<uint64_t>( handle ) );
            return Assets::AssetRef<Assets::CloudModellingVolumeAsset>::Null();
        }

        if ( it->second.Loaded )
            return Assets::AssetRef<Assets::CloudModellingVolumeAsset>::Ready( handle, it->second.Asset );

        if ( it->second.Failed )
            return Assets::AssetRef<Assets::CloudModellingVolumeAsset>::Null();

        if ( it->second.Request.IsValid() || !mayRequest || !it->second.Asset )
            return Assets::AssetRef<Assets::CloudModellingVolumeAsset>::Pending( handle );

        BeginRead( handle, it->second );
        return Assets::AssetRef<Assets::CloudModellingVolumeAsset>::Pending( handle );
    }

    Assets::AssetRef<Assets::CloudModellingVolumeAsset>
    CloudModellingService::RequireBody( const Assets::AssetHandle& handle )
    {
        return Resolve( handle, /*mayRequest=*/true );
    }

    Assets::AssetRef<Assets::CloudModellingVolumeAsset>
    CloudModellingService::Peek( const Assets::AssetHandle& handle )
    {
        return Resolve( handle, /*mayRequest=*/false );
    }

    size_t CloudModellingService::ResidentCount() const
    {
        size_t resident = 0;
        for ( const auto& [handle, entry] : m_Volumes )
        {
            if ( entry.Loaded )
                ++resident;
        }
        return resident;
    }

    glm::vec3 CloudModellingService::GetSizeKm( const Assets::AssetHandle& handle )
    {
        if ( auto it = m_Volumes.find( handle ); it != m_Volumes.end() )
            return it->second.SizeKm;
        return glm::vec3( 0.0f );
    }

    CloudModellingAtlasBinding CloudModellingService::EnsureAtlas( const std::vector<Assets::AssetHandle>& bodies )
    {
        if ( bodies.empty() )
        {
            // NOT a rebuild to nothing: the previous atlas is released, because a scene that lost its hero
            // clouds must give its megabytes back. The caller binds the fallback image.
            if ( m_Atlas )
            {
                m_Atlas.reset();
                m_AtlasSlabs.clear();
                m_AtlasRevisions.clear();
            }
            return {};
        }

        // Is the live atlas already the one being asked for? Slab order is part of the question: the same
        // bodies in a different order are a different atlas, because a slab index is baked into every
        // instance the renderer packs.
        std::vector<uint32_t> revisions;
        revisions.reserve( bodies.size() );
        for ( const Assets::AssetHandle& handle : bodies )
        {
            const auto it = m_Volumes.find( handle );
            if ( it == m_Volumes.end() )
            {
                LOG_ERROR( "[Clouds] The atlas was asked for volume {}, which is not registered. No hero "
                           "cloud will draw this frame.",
                           static_cast<uint64_t>( handle ) );
                return {};
            }
            revisions.push_back( it->second.Revision );
        }

        if ( m_Atlas && m_AtlasSlabs == bodies && m_AtlasRevisions == revisions )
            return CloudModellingAtlasBinding{ m_Atlas, static_cast<uint32_t>( m_AtlasSlabs.size() ) };

        std::vector<const std::vector<unsigned char>*> voxels;
        voxels.reserve( bodies.size() );
        for ( const Assets::AssetHandle& handle : bodies )
            voxels.push_back( &m_Volumes[handle].Asset->GetVolume().Voxels );

        const auto assembled = Assets::AssembleCloudModellingAtlas( voxels );
        if ( !assembled )
        {
            LOG_ERROR( "[Clouds] The modelling atlas could not be assembled: {}", assembled.GetError() );
            return {};
        }

        const std::vector<unsigned char>& bytes = assembled.GetValue();

        // A FRESH IMAGE rather than an in-place update: the backend has no path for re-uploading a volume.
        // The previous image goes through Image3D's own deletion queue when the last reference drops,
        // which is what keeps a frame still reading it safe.
        const Core::Formats::Image3DSpecification spec{
             .Tag        = "CloudBodyAtlas:" + std::to_string( bodies.size() ),
             .Width      = Assets::kCloudModellingVolumeWidth,
             .Height     = Assets::kCloudModellingVolumeHeight,
             .Depth      = Assets::kCloudModellingVolumeDepth * static_cast<uint32_t>( bodies.size() ),
             .Format     = Core::Formats::ImageFormat::RGBA8F,
             .Data       = bytes,
             .Properties = Core::Formats::Sample,
        };

        auto atlas = Graphic::Image3D::Create( spec );
        if ( !atlas )
        {
            LOG_ERROR( "[Clouds] The {}x{}x{} RGBA8 modelling atlas ({} bodies) could not be created on the "
                       "device, so no hero cloud will draw.",
                       spec.Width, spec.Height, spec.Depth, bodies.size() );
            return {};
        }

        // The atlas is ONE image built from SEVERAL assets, so it names none of them: it is the service's,
        // and asset eviction must not reach it through any single body's handle. See ResourceLedger.hpp.
        atlas->ClaimOwnership( Graphic::ResourceOwner::AssetService );
        atlas->RecordDeviceBytes( bytes.size() );

        m_Atlas          = std::move( atlas );
        m_AtlasSlabs     = bodies;
        m_AtlasRevisions = std::move( revisions );

        LOG_INFO( "[Clouds] Modelling atlas built: {} bodies, {}x{}x{} RGBA8, {:.2f} MiB on the device.",
                  bodies.size(), spec.Width, spec.Height, spec.Depth,
                  static_cast<double>( bytes.size() ) / ( 1024.0 * 1024.0 ) );

        return CloudModellingAtlasBinding{ m_Atlas, static_cast<uint32_t>( m_AtlasSlabs.size() ) };
    }

    void CloudModellingService::Clear()
    {
        // Cancelled rather than dropped: see CloudNoiseService::Clear for why the delegate is what tells
        // an entry that the worker it waits for is not coming back.
        for ( auto& [handle, entry] : m_Volumes )
            entry.Request.Cancel();

        m_Volumes.clear();
        m_Reported.clear();
        m_Atlas.reset();
        m_AtlasSlabs.clear();
        m_AtlasRevisions.clear();
    }
} // namespace Desert::Runtime
