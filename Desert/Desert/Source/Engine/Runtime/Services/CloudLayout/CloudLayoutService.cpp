#include "CloudLayoutService.hpp"

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    void CloudLayoutService::Announce( const Assets::Asset<Assets::CloudLayoutAsset>& asset )
    {
        if ( !asset )
        {
            LOG_ERROR( "[Clouds] CloudLayoutService::Announce was given a null asset; nothing was recorded, "
                       "so whatever painting this was will resolve to 'no painting'." );
            return;
        }

        // Announcing a known handle keeps what is there — the re-scan after a cook walks the same
        // directory again, and throwing away a read painting would make every layer bound to it wait
        // again for nothing the user did.
        auto& entry = m_Layouts[asset->GetMetadata().Handle];
        if ( !entry.Asset )
            entry.Asset = asset;
    }

    Common::BoolResultStr CloudLayoutService::Register( const std::shared_ptr<Assets::CloudLayoutAsset>& asset )
    {
        if ( !asset )
            return Common::MakeError( "CloudLayoutService::Register was given a null asset" );

        if ( !asset->IsReadyForUse() )
            return Common::MakeFormattedError<bool>( "cloud layout '{}' is not loaded",
                                                     asset->GetMetadata().Filepath.string() );

        const Assets::AssetHandle& handle = asset->GetMetadata().Handle;
        const uint32_t             hash   = asset->GetLayout().ContentHash;

        // THE CONTENT HASH IS THE REVISION HERE, where the sibling services keep a counter. It is the
        // stronger statement of the two: a counter says "the file was read again", the hash says "the
        // pixels differ". Re-saving a painting without changing it therefore does NOT invalidate the baked
        // volume, and re-baking two million voxels is a stall an artist feels.
        if ( auto it = m_Layouts.find( handle );
             it != m_Layouts.end() && it->second.Loaded && it->second.ContentHash == hash )
            return BOOLSUCCESS;

        // UPDATED, NOT REPLACED: replacing would drop the `LoadRequest` this call is very likely running
        // inside the completion of, and destroying a request from within its own delegate is the
        // reentrancy the deferred-completion rule exists to keep out of this code.
        Entry& entry      = m_Layouts[handle];
        entry.Asset       = asset;
        entry.ContentHash = hash;
        entry.Loaded      = true;
        entry.Failed      = false;

        const Assets::CloudLayoutData& layout = asset->GetLayout();
        LOG_INFO( "[Clouds] Cloud layout '{}' registered as {} ({}x{}, pattern {}, mask {}, content {:08x}).",
                  asset->GetMetadata().Filepath.filename().string(), static_cast<uint64_t>( handle ),
                  layout.Resolution, layout.Resolution, layout.HasPattern() ? "yes" : "no",
                  layout.HasMask() ? "yes" : "no", hash );
        return BOOLSUCCESS;
    }

    void CloudLayoutService::BeginRead( const Assets::AssetHandle& handle, Entry& entry )
    {
        entry.Request = Assets::AsyncAssetLoader::Get().Request(
             entry.Asset,
             [this, handle]( const Assets::Asset<Assets::AssetBase>& loaded, const Assets::LoadOutcome outcome,
                             const std::string& error )
             {
                 const auto it = m_Layouts.find( handle );
                 if ( it == m_Layouts.end() )
                     return;

                 it->second.Request.Release();

                 if ( outcome != Assets::LoadOutcome::Loaded )
                 {
                     it->second.Failed = true;
                     LOG_ERROR( "[Clouds] Cloud layout '{}' could not be read: {}. Every layer naming it "
                                "places its clouds procedurally from here on.",
                                it->second.Asset ? it->second.Asset->GetMetadata().Filepath.string()
                                                 : std::string( "<unknown>" ),
                                error );
                     return;
                 }

                 const auto asset      = std::static_pointer_cast<Assets::CloudLayoutAsset>( loaded );
                 const auto registered = Register( asset );
                 if ( !registered )
                 {
                     // Re-found rather than reused: `Register` writes into this same map.
                     const auto entryAfter = m_Layouts.find( handle );
                     if ( entryAfter != m_Layouts.end() )
                         entryAfter->second.Failed = true;
                     LOG_ERROR( "[Clouds] Cloud layout '{}' was read but could not be registered: {}",
                                asset->GetMetadata().Filepath.string(), registered.GetError() );
                 }
             },
             [this, handle]
             {
                 const auto it = m_Layouts.find( handle );
                 if ( it == m_Layouts.end() )
                     return;
                 // Back to "not asked for", not to "failed": a cancel is the caller changing its mind,
                 // which is a different fact from the file being unreadable.
                 it->second.Request.Release();
             } );
    }

    Assets::AssetRef<const Assets::CloudLayoutData> CloudLayoutService::Resolve( const Assets::AssetHandle& handle,
                                                                                 const bool mayRequest )
    {
        // AN EMPTY SLOT IS NULL AND NEVER PENDING. Nothing was asked for, so there is nothing to wait
        // for; this is the state every scene in the repository ships in and the bake's "no painting".
        if ( handle == 0 )
            return Assets::AssetRef<const Assets::CloudLayoutData>::Null();

        const auto it = m_Layouts.find( handle );
        if ( it == m_Layouts.end() )
        {
            // Said once per handle rather than once per frame: a missing layout is a permanent state of
            // the scene, and a message repeated sixty times a second is a log nobody reads.
            if ( mayRequest && m_Reported.insert( handle ).second )
                LOG_ERROR( "[Clouds] Cloud layout {} is referenced but was never announced — the layer "
                           "places its clouds procedurally instead. The scene names a .dclayout the asset "
                           "scan did not find.",
                           static_cast<uint64_t>( handle ) );
            return Assets::AssetRef<const Assets::CloudLayoutData>::Null();
        }

        if ( it->second.Loaded && it->second.Asset )
        {
            // The aliasing constructor: a share of the ASSET's lifetime, addressing its layout member. See
            // the header for why the bake must not be handed a borrowed pointer.
            return Assets::AssetRef<const Assets::CloudLayoutData>::Ready(
                 handle, std::shared_ptr<const Assets::CloudLayoutData>( it->second.Asset,
                                                                         &it->second.Asset->GetLayout() ) );
        }

        if ( it->second.Failed )
            return Assets::AssetRef<const Assets::CloudLayoutData>::Null();

        if ( it->second.Request.IsValid() || !mayRequest )
            return Assets::AssetRef<const Assets::CloudLayoutData>::Pending( handle );

        BeginRead( handle, it->second );
        return Assets::AssetRef<const Assets::CloudLayoutData>::Pending( handle );
    }

    Assets::AssetRef<const Assets::CloudLayoutData>
    CloudLayoutService::Require( const Assets::AssetHandle& handle )
    {
        return Resolve( handle, /*mayRequest=*/true );
    }

    Assets::AssetRef<const Assets::CloudLayoutData> CloudLayoutService::Peek( const Assets::AssetHandle& handle )
    {
        return Resolve( handle, /*mayRequest=*/false );
    }

    size_t CloudLayoutService::ResidentCount() const
    {
        size_t resident = 0;
        for ( const auto& [handle, entry] : m_Layouts )
        {
            if ( entry.Loaded )
                ++resident;
        }
        return resident;
    }

    void CloudLayoutService::Clear()
    {
        // Cancelled rather than dropped: see CloudNoiseService::Clear for why the delegate is what tells
        // an entry that the worker it waits for is not coming back.
        for ( auto& [handle, entry] : m_Layouts )
            entry.Request.Cancel();

        m_Layouts.clear();
        m_Reported.clear();
    }
} // namespace Desert::Runtime
