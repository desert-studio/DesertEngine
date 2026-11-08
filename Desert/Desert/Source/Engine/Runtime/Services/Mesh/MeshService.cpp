#include "MeshService.hpp"

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/RendererContext.hpp>

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <algorithm>

namespace Desert::Runtime
{
    namespace
    {
        // GET'S RAW POINTER OUTLIVES EVERY FRAME THAT MAY HAVE READ IT. A rebuild over a built mesh replaces
        // the pointer a render worker of frame N-1 may still be recording with; the outgoing mesh retires
        // through the renderer's frame-retire queue (UE: deferred deletion) and dies at once only with no
        // renderer. Eviction keeps its own FrameRetireQueue (m_Retiring), which `RetireEvicted` collects.
        void RetireMesh( std::shared_ptr<Desert::Mesh>&& outgoing ) noexcept
        {
            if ( !outgoing )
                return;
            const auto ctx = EngineContext::GetInstance().GetRendererContext();
            if ( !ctx )
                return; // `outgoing` dies here
            ctx->RetireAfterFramesInFlight( [held = std::move( outgoing )]() mutable { held.reset(); } );
        }

        using MeshTable = std::unordered_map<Assets::AssetHandle, std::shared_ptr<Desert::Mesh>>;

        // Clear's table: every Mesh* it answered may still be recorded by a frame in flight, so the whole map
        // retires as one unit (ImageService::Clear and AnimatedImageService::Clear do the same).
        void RetireMeshTable( MeshTable&& table ) noexcept
        {
            if ( table.empty() )
                return;
            const auto ctx = EngineContext::GetInstance().GetRendererContext();
            if ( !ctx )
                return; // `table` dies here
            ctx->RetireAfterFramesInFlight( [held = std::move( table )]() mutable { held.clear(); } );
        }
    } // namespace

    MeshService::MeshService( std::unique_ptr<IMeshUploader> uploader ) : m_Uploader( std::move( uploader ) )
    {
    }

    MeshService::~MeshService() = default;

    Common::BoolResultStr MeshService::Register( const std::shared_ptr<Assets::MeshAsset>& meshAsset )
    {
        if ( !meshAsset )
        {
            return Common::MakeError( "Mesh asset is null" );
        }
        if ( !meshAsset->GetMetadata().IsValid() )
        {
            return Common::MakeError( "Mesh asset is invalid" );
        }

        // THE SHELL BEFORE THE BUILD. Recorded first so that a build which fails (a skinned mesh whose rig
        // is not in the manager yet, a cooked file that will not parse) still leaves something `Get` can
        // retry from — and so BuildAndCache's own EnsureLoaded has the asset on record while it runs.
        std::unique_lock lock( m_Lock );
        DrainLateFailures();
        m_Entries[meshAsset->GetMetadata().Handle].Asset = meshAsset;

        return BuildAndCache( meshAsset );
    }

    Common::BoolResultStr MeshService::RegisterAsset( const std::shared_ptr<Assets::MeshAsset>&  meshAsset,
                                                      const std::weak_ptr<Assets::AssetManager>& resolveAgainst )
    {
        if ( !meshAsset )
            return Common::MakeError( "Mesh asset is null" );
        if ( resolveAgainst.expired() )
            return Common::MakeError( "Mesh asset '" + meshAsset->GetMetadata().Filepath.string() +
                                      "' was registered as a lazy shell against an AssetManager that is "
                                      "already gone; its deferred load could never resolve dependencies." );

        // Handle is path-derived in the ctor, so a not-yet-loaded shell is keyed correctly. The .stmesh parse
        // + GPU build are deferred to the first Get/GetAsset.
        std::unique_lock lock( m_Lock );
        m_Entries[meshAsset->GetMetadata().Handle].Asset = meshAsset;
        m_AssetManager                                   = resolveAgainst;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr MeshService::EnsureLoaded( const std::shared_ptr<Assets::MeshAsset>& meshAsset ) const
    {
        if ( meshAsset->IsReadyForUse() )
            return BOOLSUCCESS;

        const auto manager = m_AssetManager.lock();
        if ( !manager )
        {
            // Loudly, and then not at all: a skinned mesh parsed without a manager reports a skeleton
            // signature it cannot look up, MeshFactory refuses to build it, and the frame contains nothing
            // with no other trace anywhere. Naming the file and the reason is the whole difference between
            // this and the defect it replaces.
            return Common::MakeError( "MeshService: '" + meshAsset->GetMetadata().Filepath.string() +
                                      "' needs a deferred load but no AssetManager is bound — the shell was "
                                      "never registered through RegisterAsset, or its project has closed." );
        }

        return meshAsset->EnsureLoaded( *manager );
    }

    Common::BoolResultStr MeshService::BuildAndCache( const std::shared_ptr<Assets::MeshAsset>& meshAsset ) const
    {
        // The payload first. Building from an unparsed shell produces a Mesh with no geometry in it, and
        // that Mesh is then the cached answer for the life of the process — see the header on Register.
        if ( const auto loaded = EnsureLoaded( meshAsset ); !loaded )
            return loaded;

        const std::string path   = meshAsset->GetMetadata().Filepath.string();
        const auto        handle = meshAsset->GetMetadata().Handle;

        auto mesh = m_Uploader->Upload( meshAsset );
        if ( !mesh )
        {
            // The uploader has already said which of its preconditions failed; this adds the file, which it
            // does not have. Nothing is cached: the comment this replaces was right that a sticky null can
            // never recover once the dependency is in place.
            return Common::MakeFormattedError( "MeshService: no runtime mesh could be built for '{}'", path );
        }

        // THE RELATION. Asserted rather than assumed, because both sides are individually well-formed and
        // only their disagreement is the defect.
        if ( mesh->GetSubmeshes().size() != meshAsset->GetSubmeshes().size() )
        {
            return Common::MakeFormattedError(
                 "MeshService: '{}' holds {} submesh(es) but the runtime mesh built from it has {} — it "
                 "would draw nothing while looking like a built mesh, so it is NOT cached.",
                 path, meshAsset->GetSubmeshes().size(), mesh->GetSubmeshes().size() );
        }

        // THE AUTHORED LOD SCREEN SIZES, resolved against the chain just built (UE ResolveSectionInfo after the
        // DDC fetch): they are not a build input, so they are read from their home on every build.
        const auto lodScreenSizes =
             Assets::Serialization::ReadMeshLodScreenSizes( meshAsset->GetMetadata().Filepath );
        if ( !lodScreenSizes )
            return Common::MakeFormattedError( "MeshService: '{}': LOD screen sizes: {}", path,
                                               lodScreenSizes.GetError() );
        mesh->SetAuthoredLODScreenSizes( lodScreenSizes.GetValue() );

        std::shared_ptr<Mesh>& slot = m_Meshes[handle];
        std::shared_ptr<Mesh>  outgoing;
        if ( slot ) // a rebuild over a built mesh: the pointer callers were answered is replaced
        {
            m_BuiltRevision.fetch_add( 1, std::memory_order_acq_rel );
            outgoing = std::move( slot );
        }
        slot = std::move( mesh );
        RetireMesh( std::move( outgoing ) );
        return BOOLSUCCESS;
    }

    void MeshService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        std::unique_lock lock( m_Lock );
        m_AssetManager = assets;
    }

    void MeshService::DrainLateFailures() const
    {
        std::vector<LateFailure> landed;
        {
            std::lock_guard failLock( m_LateFailLock );
            if ( m_LateFailures.empty() )
                return;
            landed.swap( m_LateFailures );
        }
        for ( const auto& failure : landed )
            Fail( failure.Owner, failure.Reason );
    }

    void MeshService::Fail( const Assets::AssetHandle& owner, const std::string& reason ) const
    {
        auto& entry = m_Entries[owner];
        if ( entry.Failed )
            return;
        entry.Failed = true;
        LOG_ERROR( "[MeshService] mesh {} will not be drawn: {}", static_cast<uint64_t>( owner ), reason );
    }

    MeshService::Entry* MeshService::FindOrDiscover( const Assets::AssetHandle& handle ) const
    {
        if ( const auto it = m_Entries.find( handle ); it != m_Entries.end() )
            return &it->second;

        using Common::Content::ContentKind;
        const auto id      = static_cast<uint64_t>( handle );
        const bool skinned = Assets::ContentRegistry::RowOf( ContentKind::SkinnedMesh, id ).has_value();
        if ( !skinned && !Assets::ContentRegistry::RowOf( ContentKind::StaticMesh, id ) )
            return nullptr; // a procedural handle, or no mesh at all: nothing to discover

        std::shared_ptr<Assets::MeshAsset> shell;
        std::string                        error;
        if ( skinned )
        {
            auto created = Assets::CreateFromRegistryRow<Assets::SkinnedMeshAsset>( m_AssetManager, handle,
                                                                                    ContentKind::SkinnedMesh );
            created ? void( shell = created.GetValue() ) : void( error = created.GetError() );
        }
        else
        {
            auto created = Assets::CreateFromRegistryRow<Assets::StaticMeshAsset>( m_AssetManager, handle,
                                                                                   ContentKind::StaticMesh );
            created ? void( shell = created.GetValue() ) : void( error = created.GetError() );
        }
        Entry& entry = m_Entries[handle];
        entry.Asset  = std::move( shell );
        if ( !entry.Asset )
            Fail( handle, error );
        return &entry;
    }

    void MeshService::RequestRead( const Assets::AssetHandle&                owner,
                                   const std::shared_ptr<Assets::AssetBase>& asset,
                                   Assets::LoadRequest&                      slot ) const
    {
        // One read per file: another consumer's request for the same rig (or this mesh's own, not pumped
        // yet) is already the read, and `Arrived` looks at readiness rather than at who asked.
        if ( Assets::AsyncAssetLoader::Get().IsRequested( asset->GetMetadata().Handle ) )
            return;
        slot = Assets::AsyncAssetLoader::Get().Request(
             asset,
             [this, owner]( const Assets::Asset<Assets::AssetBase>& read, const Assets::LoadOutcome outcome,
                            const std::string& error )
             {
                 if ( outcome != Assets::LoadOutcome::Failed )
                     return;
                 // Queued, not applied: see m_LateFailures (a delegate never takes m_Lock).
                 std::lock_guard failLock( m_LateFailLock );
                 m_LateFailures.push_back(
                      { owner, "'" + read->GetMetadata().Filepath.string() + "' could not be read: " + error } );
             },
             [] {} );
    }

    bool MeshService::RearmOnRigWritten( const Assets::AssetHandle& handle, Entry& entry )
    {
        if ( !entry.AwaitedRig )
            return false;
        RigAwaited& awaited = *entry.AwaitedRig;
        if ( Assets::ContentRegistry::WriteSerial() <= awaited.Since )
            return false;

        const auto rigs =
             Assets::ContentRegistry::WrittenSince( Common::Content::ContentKind::Skeleton, awaited.Since );
        const auto meshes =
             Assets::ContentRegistry::WrittenSince( Common::Content::ContentKind::SkinnedMesh, awaited.Since );
        const bool rigWritten =
             std::ranges::any_of( rigs.Rows, [&]( const auto& row ) { return row.Skeleton == awaited.Skeleton; } );
        const bool meshWritten =
             std::ranges::any_of( meshes.Rows, [&]( const auto& row ) { return row.Handle == handle; } );
        awaited.Since = std::min( rigs.Serial, meshes.Serial );
        if ( !rigWritten && !meshWritten )
            return false;

        entry.AwaitedRig.reset();
        entry.Failed = false;
        return true;
    }

    bool MeshService::Arrived( const Assets::AssetHandle& handle, Entry& entry ) const
    {
        if ( entry.Failed && !RearmOnRigWritten( handle, entry ) )
            return false;
        if ( !entry.Asset )
            return false;

        bool ready = true;
        if ( !entry.Asset->IsReadyForUse() )
        {
            RequestRead( handle, entry.Asset, entry.MeshRead );
            ready = false;
        }
        if ( !entry.Asset->IsSkinned() )
        {
            // A static mesh read by the async loader was never resolved (Load, not EnsureLoaded): its distance
            // field waits on its materials, which only the manager can load. IsSkinned() is false, and only
            // StaticMeshAsset answers it so.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
            auto& staticMesh = static_cast<Assets::StaticMeshAsset&>( *entry.Asset );
            if ( ready && !staticMesh.AreDependenciesResolved() )
                if ( const auto manager = m_AssetManager.lock() )
                    staticMesh.ResolveDependencies( *manager );
            return ready;
        }

        // THE RIG IS NAMED BY THE REGISTRY, NOT FOUND BY A SEARCH. The mesh row's Rig tag is the signature
        // its header states and the skeleton row states the same one, so the rig is requested beside the
        // mesh, before either file is read, and no other skeleton is touched.
        if ( !entry.Rig )
        {
            // Taken BEFORE the lookup, so a Skeleton row written between the lookup and the failure is
            // still seen by RearmOnRigWritten.
            const uint64_t seen      = Assets::ContentRegistry::WriteSerial();
            const auto     row       = Assets::ContentRegistry::RowOf( Common::Content::ContentKind::SkinnedMesh,
                                                                       static_cast<uint64_t>( handle ) );
            const Common::Content::AssetGuid skeleton = row ? row->Skeleton : Common::Content::AssetGuid{};
            const auto                       rig      = Assets::ContentRegistry::RigRow( skeleton );
            if ( !rig )
            {
                Fail( handle,
                      fmt::format( "the skinned mesh '{}' references skeleton {} and no Skeleton row of the "
                                   "content registry is it yet; it is drawn once that skeleton is written",
                                   entry.Asset->GetMetadata().Filepath.string(),
                                   Common::Content::AssetGuidToText( skeleton ) ) );
                entry.AwaitedRig = RigAwaited{ skeleton, seen };
                return false;
            }
            auto created = Assets::CreateFromRegistryRow<Assets::SkeletonAsset>(
                 m_AssetManager, rig->Handle, Common::Content::ContentKind::Skeleton );
            if ( !created )
            {
                Fail( handle, created.GetError() );
                return false;
            }
            entry.Rig = created.GetValue();
        }
        if ( !entry.Rig->IsReadyForUse() )
        {
            RequestRead( handle, entry.Rig, entry.RigRead );
            ready = false;
        }
        if ( !ready )
            return false;

        // IsSkinned() held above, and only SkinnedMeshAsset answers it true.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        auto& skinned = static_cast<Assets::SkinnedMeshAsset&>( *entry.Asset );
        if ( !skinned.GetSkeletonDependency().IsValid() )
        {
            if ( const auto manager = m_AssetManager.lock() )
                skinned.ResolveDependencies( *manager );
            if ( !skinned.GetSkeletonDependency().IsValid() )
            {
                Fail( handle,
                      fmt::format( "'{}' is resident with its registry skeleton '{}' and does not bind it; the "
                                   "registry's Rig tag is stale — re-scan",
                                   entry.Asset->GetMetadata().Filepath.string(),
                                   entry.Rig->GetMetadata().Filepath.string() ) );
                return false;
            }
        }
        return true;
    }

    Desert::Mesh* MeshService::Get( const Assets::AssetHandle& handle ) const
    {
        {
            std::shared_lock lock( m_Lock );
            if ( auto it = m_Meshes.find( handle ); it != m_Meshes.end() )
                return it->second.get();
        }

        // The miss: discovery, the read request and the build all write, so the lock is retaken exclusively
        // and the hit re-checked (another thread may have built it between the two locks).
        std::unique_lock lock( m_Lock );
        DrainLateFailures();
        if ( auto it = m_Meshes.find( handle ); it != m_Meshes.end() )
            return it->second.get();

        Entry* entry = FindOrDiscover( handle );
        if ( entry == nullptr || !Arrived( handle, *entry ) )
            return nullptr;

        if ( const auto built = BuildAndCache( entry->Asset ); !built )
        {
            Fail( handle, built.GetError() );
            return nullptr;
        }
        return m_Meshes[handle].get();
    }

    MeshService::State MeshService::StateOf( const Assets::AssetHandle& handle ) const noexcept
    {
        std::shared_lock lock( m_Lock );
        {
            // A read failure a delegate queued and no locked door has applied yet is already a failure.
            std::lock_guard failLock( m_LateFailLock );
            for ( const auto& failure : m_LateFailures )
            {
                if ( failure.Owner == handle )
                    return State::Failed;
            }
        }
        if ( m_Meshes.find( handle ) != m_Meshes.end() )
            return State::Ready;
        const auto it = m_Entries.find( handle );
        if ( it == m_Entries.end() )
            return State::Unknown;
        return it->second.Failed ? State::Failed : State::Pending;
    }

    Assets::MeshAsset* MeshService::GetAsset( const Assets::AssetHandle& handle ) const
    {
        std::unique_lock lock( m_Lock );
        DrainLateFailures();
        Entry* entry = FindOrDiscover( handle );
        if ( entry == nullptr || !Arrived( handle, *entry ) )
            return nullptr;
        return entry->Asset.get();
    }

    void MeshService::StartRead( const Assets::AssetHandle&        handle,
                                 std::vector<Assets::AssetHandle>& awaited ) const
    {
        std::unique_lock lock( m_Lock );
        DrainLateFailures();
        Entry* entry = FindOrDiscover( handle );
        if ( entry == nullptr || Arrived( handle, *entry ) )
            return;
        if ( entry->Asset && !entry->Asset->IsReadyForUse() )
            awaited.emplace_back( entry->Asset->GetMetadata().Handle );
        if ( entry->Rig && !entry->Rig->IsReadyForUse() )
            awaited.emplace_back( entry->Rig->GetMetadata().Handle );
    }

    std::size_t MeshService::AwaitResident( std::span<const Assets::AssetHandle> handles ) const
    {
        std::vector<Assets::AssetHandle> awaited;
        for ( const auto& handle : handles )
            StartRead( handle, awaited );
        for ( const auto& read : awaited )
            Assets::AsyncAssetLoader::Get().AwaitOne( read );

        std::size_t drawable = 0;
        for ( const auto& handle : handles )
            drawable += Get( handle ) != nullptr ? 1 : 0;
        return drawable;
    }

    Desert::Mesh* MeshService::LoadNow( const Assets::AssetHandle& handle )
    {
        // FlushOne runs loader delegates, so it is called with the lock released and with the handles copied
        // out of the entry (the Entry* is not used past the lock: another door may rehash m_Entries).
        std::optional<Assets::AssetHandle> meshFile;
        std::optional<Assets::AssetHandle> rigFile;
        {
            std::unique_lock lock( m_Lock );
            DrainLateFailures();
            Entry* entry = FindOrDiscover( handle );
            if ( entry == nullptr )
                return nullptr;
            (void)Arrived( handle, *entry );
            if ( entry->Asset )
                meshFile = entry->Asset->GetMetadata().Handle;
            if ( entry->Rig )
                rigFile = entry->Rig->GetMetadata().Handle;
        }
        if ( meshFile )
            Assets::AsyncAssetLoader::Get().FlushOne( *meshFile );
        if ( rigFile )
            Assets::AsyncAssetLoader::Get().FlushOne( *rigFile );
        return Get( handle );
    }

    bool MeshService::EvictBuilt( const Assets::AssetHandle& handle )
    {
        std::unique_lock lock( m_Lock );
        const auto       built = m_Meshes.find( handle );
        if ( built == m_Meshes.end() )
            return false;

        // Parked, not destroyed: the frames recorded before this sweep may still be reading the buffers.
        m_Retiring.Park( std::move( built->second ), Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );
        m_Meshes.erase( built );
        m_BuiltRevision.fetch_add( 1, std::memory_order_acq_rel );
        return true;
    }

    std::size_t MeshService::RetireEvicted()
    {
        const auto&      frames = Engine::FrameManager::GetInstance();
        std::unique_lock lock( m_Lock );
        return m_Retiring.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );
    }

    void MeshService::Clear()
    {
        MeshTable outgoing;
        {
            std::unique_lock lock( m_Lock );
            outgoing.swap( m_Meshes );
            m_BuiltRevision.fetch_add( 1, std::memory_order_acq_rel );
            m_Retiring.Clear();
            m_Entries.clear();
            m_AssetManager.reset();
            std::lock_guard failLock( m_LateFailLock );
            m_LateFailures.clear();
        }
        // Outside the lock: with no renderer the table is destroyed right here, and that runs no reader's wait.
        RetireMeshTable( std::move( outgoing ) );
    }

    std::optional<bool> MeshService::IsSkinned( const Assets::AssetHandle& handle ) const
    {
        // Cheap query: the concrete asset subclass (Static vs Skinned, chosen by extension at registration)
        // encodes skinned-ness via a virtual constant — no need to parse the .stmesh or build the GPU mesh.
        // This must stay cheap: UI (e.g. the mesh selector popup) calls it for EVERY mesh asset every frame.
        // Building here would force a synchronous load+GPU-build of every mesh in the project → multi-second
        // freeze that looks like a hang.
        std::shared_lock lock( m_Lock );
        if ( auto ait = m_Entries.find( handle ); ait != m_Entries.end() && ait->second.Asset )
            return std::make_optional( ait->second.Asset->IsSkinned() );

        return std::nullopt;
    }

} // namespace Desert::Runtime
