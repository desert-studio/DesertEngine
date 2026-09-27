#include "MeshService.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/RegistryDiscovery.hpp>

namespace Desert::Runtime
{
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

        m_Meshes[handle] = std::move( mesh );
        return BOOLSUCCESS;
    }

    Assets::AssetHandle MeshService::RegisterProcedural( const std::shared_ptr<Mesh>& mesh )
    {
        // Procedural meshes have no source path, so mint a fresh random id (the default handle is now Null).
        Assets::AssetHandle handle = Assets::AssetHandle::Generate();
        // Build the GPU vertex/index buffers — the mesh ctor doesn't. Without this a builtin procedural
        // mesh (e.g. the Cube) has no buffers and renders nothing.
        if ( mesh )
        {
            // Reported, not refused: the caller receives a handle either way and the registry is the
            // only place this mesh can be found again. What must not happen is the previous behaviour —
            // a mesh whose buffers never uploaded sitting in the registry, drawing nothing, with the
            // handle looking exactly like a working one.
            if ( const auto uploaded = m_Uploader->UploadProcedural( mesh, handle ); !uploaded )
                LOG_ERROR( "[MeshService] procedural mesh {} has no GPU buffers: {}", (uint64_t)handle,
                           uploaded.GetError() );
        }
        m_Meshes[handle] = mesh;
        return handle;
    }

    void MeshService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_AssetManager = assets;
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
        const uint64_t id      = static_cast<uint64_t>( handle );
        const bool     skinned = Assets::ContentRegistry::RowOf( ContentKind::SkinnedMesh, id ).has_value();
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

    void MeshService::RequestRead( const Assets::AssetHandle& owner, const std::shared_ptr<Assets::AssetBase>& asset,
                                   Assets::LoadRequest& slot ) const
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
                 if ( outcome == Assets::LoadOutcome::Failed )
                     Fail( owner, "'" + read->GetMetadata().Filepath.string() + "' could not be read: " + error );
             },
             [] {} );
    }

    bool MeshService::Arrived( const Assets::AssetHandle& handle, Entry& entry ) const
    {
        if ( entry.Failed || !entry.Asset )
            return false;

        bool ready = true;
        if ( !entry.Asset->IsReadyForUse() )
        {
            RequestRead( handle, entry.Asset, entry.MeshRead );
            ready = false;
        }
        if ( !entry.Asset->IsSkinned() )
            return ready;

        // THE RIG IS NAMED BY THE REGISTRY, NOT FOUND BY A SEARCH. The mesh row's Rig tag is the signature
        // its header states and the skeleton row states the same one, so the rig is requested beside the
        // mesh, before either file is read, and no other skeleton is touched.
        if ( !entry.Rig )
        {
            const auto row = Assets::ContentRegistry::RowOf( Common::Content::ContentKind::SkinnedMesh,
                                                             static_cast<uint64_t>( handle ) );
            const uint64_t signature = row ? row->RigSignature : 0;
            const auto     rig       = Assets::ContentRegistry::RigRow( signature );
            if ( !rig )
            {
                Fail( handle, fmt::format( "the skinned mesh '{}' names rig signature {} and no Skeleton row of the "
                                           "content registry states it (re-cook the mesh or its skeleton)",
                                           entry.Asset->GetMetadata().Filepath.string(), signature ) );
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

        auto& skinned = static_cast<Assets::SkinnedMeshAsset&>( *entry.Asset );
        if ( !skinned.GetSkeletonDependency().IsValid() )
        {
            if ( const auto manager = m_AssetManager.lock() )
                skinned.ResolveDependencies( *manager );
            if ( !skinned.GetSkeletonDependency().IsValid() )
            {
                Fail( handle, fmt::format( "'{}' is resident with its registry rig '{}' (signature {}) and the two "
                                           "do not match; the registry's Rig tag is stale — re-scan",
                                           entry.Asset->GetMetadata().Filepath.string(),
                                           entry.Rig->GetMetadata().Filepath.string(), entry.Rig->GetSignature() ) );
                return false;
            }
        }
        return true;
    }

    Desert::Mesh* MeshService::Get( const Assets::AssetHandle& handle ) const
    {
        if ( auto it = m_Meshes.find( handle ); it != m_Meshes.end() )
            return it->second.get();

        Entry* entry = FindOrDiscover( handle );
        if ( !entry || !Arrived( handle, *entry ) )
            return nullptr;

        if ( const auto built = BuildAndCache( entry->Asset ); !built )
        {
            Fail( handle, built.GetError() );
            return nullptr;
        }
        return m_Meshes[handle].get();
    }

    Assets::MeshAsset* MeshService::GetAsset( const Assets::AssetHandle& handle ) const
    {
        Entry* entry = FindOrDiscover( handle );
        if ( !entry || !Arrived( handle, *entry ) )
            return nullptr;
        return entry->Asset.get();
    }

    void MeshService::StartRead( const Assets::AssetHandle& handle, std::vector<Assets::AssetHandle>& awaited ) const
    {
        Entry* entry = FindOrDiscover( handle );
        if ( !entry || Arrived( handle, *entry ) )
            return;
        if ( entry->Asset && !entry->Asset->IsReadyForUse() )
            awaited.push_back( entry->Asset->GetMetadata().Handle );
        if ( entry->Rig && !entry->Rig->IsReadyForUse() )
            awaited.push_back( entry->Rig->GetMetadata().Handle );
    }

    std::size_t MeshService::AwaitResident( std::span<const Assets::AssetHandle> handles )
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
        Entry* entry = FindOrDiscover( handle );
        if ( !entry )
            return nullptr;
        (void)Arrived( handle, *entry );
        if ( entry->Asset )
            Assets::AsyncAssetLoader::Get().FlushOne( entry->Asset->GetMetadata().Handle );
        if ( entry->Rig )
            Assets::AsyncAssetLoader::Get().FlushOne( entry->Rig->GetMetadata().Handle );
        return Get( handle );
    }

    bool MeshService::EvictBuilt( const Assets::AssetHandle& handle )
    {
        const auto built = m_Meshes.find( handle );
        if ( built == m_Meshes.end() )
            return false;

        // No shell means no recipe: this is a procedural mesh registered by RegisterProcedural, and the
        // only copy of its geometry is the buffers about to be dropped. Refuse, and say so — a silent
        // "nothing to do" here would read to the caller as "already released".
        if ( m_Entries.find( handle ) == m_Entries.end() )
        {
            LOG_WARN( "[MeshService] eviction asked for mesh {} and it is procedural — no asset shell, so "
                      "nothing could rebuild it. Kept.",
                      static_cast<uint64_t>( handle ) );
            return false;
        }

        m_Meshes.erase( built );
        return true;
    }

    void MeshService::Clear()
    {
        m_Meshes.clear();
        m_Entries.clear();
        m_AssetManager.reset();
    }

    std::optional<bool> MeshService::IsSkinned( const Assets::AssetHandle& handle ) const
    {
        // Cheap query: the concrete asset subclass (Static vs Skinned, chosen by extension at registration)
        // encodes skinned-ness via a virtual constant — no need to parse the .stmesh or build the GPU mesh.
        // This must stay cheap: UI (e.g. the mesh selector popup) calls it for EVERY mesh asset every frame.
        // Building here would force a synchronous load+GPU-build of every mesh in the project → multi-second
        // freeze that looks like a hang.
        if ( auto ait = m_Entries.find( handle ); ait != m_Entries.end() && ait->second.Asset )
            return std::make_optional( ait->second.Asset->IsSkinned() );

        // Procedural meshes have no asset shell — fall back to the already-built runtime mesh if present.
        if ( auto it = m_Meshes.find( handle ); it != m_Meshes.end() )
            return std::make_optional( it->second->IsSkinned() );

        return std::nullopt;
    }

} // namespace Desert::Runtime
