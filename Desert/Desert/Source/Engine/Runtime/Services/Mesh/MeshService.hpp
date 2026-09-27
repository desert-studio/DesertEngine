#pragma once

#include <Engine/Geometry/Mesh.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/AsyncAssetLoader.hpp>
#include <Engine/Runtime/Services/Mesh/MeshUploader.hpp>

#include <span>

namespace Desert::Runtime
{
    class MeshService
    {
    public:
        /// @p uploader is the device half (MeshUploader.hpp): the engine passes MakeGpuMeshUploader(), a
        /// suite passes a fake and runs everything else in this class without a GPU.
        explicit MeshService( std::unique_ptr<IMeshUploader> uploader );
        ~MeshService();
        MeshService( const MeshService& )            = delete;
        MeshService& operator=( const MeshService& ) = delete;

        // EAGER: PARSE (IF NEEDED) AND BUILD THE GPU MESH NOW — and the parenthesis is the fix, not a
        // clarification. This line has said "parse + build" since it was written and the body only ever
        // built: it called MeshFactory::Create on whatever the asset held at that instant. Every call site
        // handed it an asset it believed was loaded, and one of them was wrong in a way nothing could see —
        // `AssetManager::CreateAsset` DEDUPLICATES on a spelling-independent key, so a scene naming
        // `Cooked/Meshes/base.stmesh` got back AssetPreloader's UNPARSED shell for the absolute spelling of
        // the same file, this built a StaticMesh from 0 vertices / 0 submeshes, cached it under the handle,
        // and the `Load()` on the next line filled the ASSET while the cached MESH stayed empty forever.
        // Measured on the reproducer scene: file carries 1 submesh, the asset ends with 1, `Get` answered 0
        // ninety-one times in one 90-frame run and the frame was empty.
        //
        // So the load is inside now. Success means the built mesh matches the asset it was built from
        // (BuildAndCache asserts exactly that); failure names the file and the reason and caches NOTHING.
        //
        // THE ROUTE THAT DELIVERED THAT SHELL IS CLOSED SEPARATELY, and this does not depend on it. The
        // registry used to answer "is this file registered" two ways — `CreateAsset` on the identity key,
        // `FindByPath` on a verbatim path compare — so the scene's caller missed, created, and was handed
        // the preloader's shell believing it was fresh. `FindByPath` asks the same question now
        // (Desert/Tests/Engine/AssetPathIdentity), which makes that arrival rare rather than routine. It
        // does not make it impossible: a mesh the preloader genuinely never saw still reaches here as a
        // shell, and building one eagerly without parsing it is a defect whatever handed it over.
        NO_DISCARD Common::BoolResultStr Register( const std::shared_ptr<Assets::MeshAsset>& meshAsset );

        // Lazy: register the asset SHELL only — the .stmesh/.skmesh parse + GPU build are deferred to the
        // first Get. The shell must already carry its path-derived handle (set in the mesh-asset ctor).
        //
        // `resolveAgainst` IS REQUIRED, AND THAT IS THE POINT. Deferring the parse also defers everything
        // the parse tells the asset about its own dependencies, so the deferred load has to be able to
        // re-resolve them (AssetBase::EnsureLoaded). Passing the manager here rather than through a separate
        // setter means a caller cannot register a lazy shell and forget to say what it resolves against —
        // which is the mistake the compiler can catch and a comment cannot.
        Common::BoolResultStr RegisterAsset( const std::shared_ptr<Assets::MeshAsset>&  meshAsset,
                                             const std::weak_ptr<Assets::AssetManager>& resolveAgainst );

        Assets::AssetHandle RegisterProcedural( const std::shared_ptr<Mesh>& mesh );

        /// The manager a mesh named only by its handle is discovered in (its content-registry row).
        /// Bound by ResourceRegistry::BindOnDemandAssets.
        void BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets );

        // NEVER READS A FILE (AL1-5, plan §2.3). A mesh whose file (or, for a skinned one, whose rig) is not
        // resident is PENDING: the first ask requests it from AsyncAssetLoader and answers nullptr, which
        // the draw path already treats as "do not draw"; the frame after it lands, `Get` builds the GPU mesh
        // (an upload, not a read). A handle the service has no shell for is discovered from its registry row.
        Mesh*              Get( const Assets::AssetHandle& handle ) const;
        Assets::MeshAsset* GetAsset( const Assets::AssetHandle& handle ) const; // parsed, or nullptr (pending)

        /// THE SCENE-OPEN DOOR (plan §2.4(b)): request every mesh in @p handles and its rig, block until the
        /// WORKERS have read them (AsyncAssetLoader::AwaitOne, so no in-frame load is counted), and build
        /// them, so the scene's first frame is complete. Returns how many are drawable.
        std::size_t AwaitResident( std::span<const Assets::AssetHandle> handles );

        /// ONE ROW OF A CLOSURE (AL1-8b): start the worker reads of the mesh @p handle names and of its rig,
        /// appending the loader handles to wait on to @p awaited. Never reads on this thread; builds nothing.
        void StartRead( const Assets::AssetHandle& handle, std::vector<Assets::AssetHandle>& awaited ) const;

        /// Is @p handle a mesh this service has, or can discover from the content registry? Creates the
        /// shell on discovery; reads nothing.
        bool Discover( const Assets::AssetHandle& handle ) const
        {
            return FindOrDiscover( handle ) != nullptr;
        }

        /// THE EDITOR-TOOL DOOR (plan §2.4(c)): the user is waiting on this one mesh (a thumbnail, a
        /// command), so it is read on the calling thread through AsyncAssetLoader::FlushOne, where
        /// SyncLoadLedger counts it. nullptr with the reason logged when it cannot be built.
        Mesh* LoadNow( const Assets::AssetHandle& handle );

        // IS A SHELL ON RECORD FOR @p handle? A map lookup and NOTHING ELSE, which is the whole point of
        // it existing beside `GetAsset`.
        //
        // The scene parse asks this once per mesh reference, before deciding whether it owes the service a
        // registration, so the question must cost less than the answer it guards. Neither existing
        // accessor can be used for it: `GetAsset` parses the `.stmesh` through EnsureLoaded and `Get`
        // builds the GPU mesh, so the obvious ways to ask "is it registered?" are the two ways to
        // guarantee the answer is yes at full price. (`MaterialService::HasBuiltMaterial` exists for the
        // same reason, one axis over — see its comment.)
        [[nodiscard]] bool HasAsset( const Assets::AssetHandle& handle ) const
        {
            return m_Entries.find( handle ) != m_Entries.end();
        }

        void                Clear();
        std::optional<bool> IsSkinned( const Assets::AssetHandle& handle ) const;

        // DROP THE BUILT GPU MESH, KEEP THE SHELL. Returns true when something was actually dropped.
        //
        // The shell is what makes this safe to do at all: `Get()` builds on a miss from `m_Entries`, so
        // the next draw rebuilds the vertex and index buffers through exactly the path a first use takes.
        // Forgetting the shell as well would turn the next `Get()` into a null — which is the silent empty
        // answer §1.4 forbids, and the reason this is not called `Release`.
        //
        // A PROCEDURAL MESH IS NOT DROPPED, whatever the caller asks. It was registered with no asset
        // behind it (`RegisterProcedural`), so there is nothing to rebuild it from and releasing it is
        // data loss rather than eviction. The ledger says the same thing in its own vocabulary by
        // claiming those buffers to `ResourceOwner::Procedural`; this is the enforcement.
        bool EvictBuilt( const Assets::AssetHandle& handle );

    private:
        // Load a shell that has not been parsed yet AND re-resolve what the parse just revealed. Both, or
        // neither: see AssetBase::EnsureLoaded.
        Common::BoolResultStr EnsureLoaded( const std::shared_ptr<Assets::MeshAsset>& meshAsset ) const;

        // THE ONLY PLACE A RUNTIME MESH IS BUILT FROM AN ASSET, so the relation below is asserted once
        // instead of in each of the two entry points — a check written into one of them is a check the
        // other silently skips, and that is precisely how the eager path shipped an empty mesh while the
        // lazy path was correct.
        //
        // The relation: A MESH BUILT FROM AN ASSET CARRIES THAT ASSET'S SUBMESHES. Both halves are
        // individually plausible — an asset with 1 submesh is a correct asset, a Mesh with 0 submeshes is a
        // constructible Mesh — so only their agreement catches the failure. On disagreement nothing is
        // cached and the caller is told, because a cached empty is permanent: `Get` answers from the cache
        // and never consults the asset again.
        Common::BoolResultStr BuildAndCache( const std::shared_ptr<Assets::MeshAsset>& meshAsset ) const;

        mutable std::unordered_map<Assets::AssetHandle, std::shared_ptr<Mesh>>      m_Meshes;
        // One mesh the service knows: its shell, its rig (skinned only; found by the registry's Rig tag), the
        // loader requests that read them, and whether it failed — a failure is logged once and not retried.
        struct Entry
        {
            std::shared_ptr<Assets::MeshAsset>   Asset;
            Assets::Asset<Assets::SkeletonAsset> Rig;
            Assets::LoadRequest                  MeshRead;
            Assets::LoadRequest                  RigRead;
            bool                                 Failed = false;
        };

        Entry* FindOrDiscover( const Assets::AssetHandle& handle ) const;
        // Requests what is missing and answers whether the mesh (and its rig, resolved) is ready to build.
        bool Arrived( const Assets::AssetHandle& handle, Entry& entry ) const;
        void RequestRead( const Assets::AssetHandle& owner, const std::shared_ptr<Assets::AssetBase>& asset,
                          Assets::LoadRequest& slot ) const;
        void Fail( const Assets::AssetHandle& owner, const std::string& reason ) const;

        mutable std::unordered_map<Assets::AssetHandle, Entry> m_Entries;

        // The manager the deferred loads above resolve against. Weak, because the service is a
        // function-local static that outlives every project the editor opens, and a project switch must
        // leave a dead reference rather than a dangling one.
        std::weak_ptr<Assets::AssetManager> m_AssetManager;
        std::unique_ptr<IMeshUploader>      m_Uploader;
    };
} // namespace Desert::Runtime