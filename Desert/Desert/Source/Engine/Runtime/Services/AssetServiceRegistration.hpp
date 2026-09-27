#pragma once

#include <Engine/Assets/Common.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Core/Core.hpp>

namespace Desert::Core
{
    class Scene;
}

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Assets::ContentRegistry
{
    struct ClosureRow;
}

namespace Desert::Assets
{
    class AssetManager;
    class MaterialAsset;
    class MeshAsset;
    class SkyboxAsset;
} // namespace Desert::Assets

namespace Desert::Runtime
{
    /**
     * @file
     * @brief THE ONE PLACE AN ALREADY-EXISTING ASSET RECORD IS HANDED TO ITS RUNTIME SERVICE.
     *
     * WHAT THIS EXISTS TO STOP, stated as the relation it enforces:
     *
     *     EVERY ROUTE THAT RESOLVES A REFERENCE TO AN ASSET REGISTERS IT — whether the record was FOUND
     *     in the registry or CREATED by this call.
     *
     * The shape it replaces was written independently in three files and each of them got a DIFFERENT
     * part of it wrong, which is the argument for one implementation rather than three careful ones:
     *
     *   - `ComponentRegistry.cpp` (the scene parse) registered only what it created, so a mesh or
     *     material another scene or the preloader had already made resolved to a live handle no service
     *     could answer for.
     *   - `MeshDnD.cpp` (drag-and-drop import) did the same on both of its reuse paths, under the comment
     *     "Already cooked + registered?" — a question mark in a comment where a check should be.
     *   - `ThumbnailSubject.cpp` did the same, and then blamed the consequence on the wrong cause: the
     *     capture refused with "built no drawable geometry", which is a true sentence about a skinned
     *     mesh and a false one about a mesh nothing had tried to build.
     *
     * None of the three ever visibly broke, because `AssetPreloader` registers everything under the two
     * content roots it walks before a scene may load or a browser tile may be drawn. THAT IS A SAFETY NET
     * AND NOT A GUARANTEE: it is stated nowhere the callers can read, it covers only those roots, and it
     * leaves with the first refactor by somebody who does not know it is load-bearing. Measured (Ф6): with
     * the preloader's registration loop switched off, the scene path still renders byte-identically
     * because the parse registers what it names — and did not, before that change.
     *
     * WHY THE GUARD IS `HasAsset` AND NOT `Get`. All three files asked "is it registered?" by calling the
     * service's `Get`, which BUILDS on a miss: the guard performed the work it was written to avoid. A
     * scene naming forty materials built forty runtime materials during parsing to decide whether it
     * needed to register them.
     *
     * WHY REGISTRATION IS LAZY. The services register a SHELL and build on the first `Get` that asks; that
     * is what `AssetPreloader` does for everything it scans, and it means a reference costs one map lookup
     * at resolve time. A caller that needs the mesh BUILT NOW (an import that must refuse a bad cook, a
     * thumbnail that is about to photograph it) asks for that explicitly through `EnsureMeshDrawable`,
     * which is the same registration followed by the build — so the two needs share one registration and
     * cannot drift apart.
     *
     * MAIN THREAD ONLY: every function here touches the runtime services.
     */

    /// Register @p material with the MaterialService if it is not already there. Idempotent; reads
    /// nothing — a file-backed shell carries its external id from its header GUID. A refusal (another `.demat`
    /// already holds this MaterialId) is logged with both filenames — the slot is about to fall back to the
    /// default material and this is the only place that knows why.
    void EnsureMaterialRegistered( const Assets::Asset<Assets::MaterialAsset>& material );

    /// Register @p mesh with the MeshService as a LAZY SHELL if it is not already there. Idempotent.
    ///
    /// @p registry travels with the shell and is not optional: a deferred `.stmesh`/`.skmesh` parse is the
    /// first moment a skinned mesh learns which skeleton it needs, so the service must be able to ask the
    /// registry again later. Ф6 measured what its absence costs — a mesh registered without one draws
    /// nothing and prints "needs a deferred load but no AssetManager is bound" once per frame.
    void EnsureMeshRegistered( const Assets::Asset<Assets::MeshAsset>& mesh, Assets::AssetManager& registry );

    /// What waiting for a closure did: its rows, the worker reads it waited for (0 when everything was
    /// resident already) and how many of its meshes are drawable afterwards.
    struct ClosureResidency
    {
        std::size_t Rows           = 0;
        std::size_t Reads          = 0;
        std::size_t DrawableMeshes = 0;
    };

    /// THE SCENE'S DEPENDENCIES, FROM THE REGISTRY (AL1-8b, plan §2.4(b)). The roots are what the scene's
    /// components name — every static, skinned and instanced mesh, every material slot, each cloud layer's
    /// material — and the rest is the registry's `deps` column walked transitively
    /// (Assets::ContentRegistry::Closure): a mesh's own materials, an instance's parent, textures, shaders,
    /// cloud assets. Nothing is read to learn it.
    std::vector<Assets::ContentRegistry::ClosureRow> SceneDependencies( const Core::Scene& scene );

    /// THE ONE WAIT FOR A CLOSURE: every row's read is started on the loader's WORKERS first (meshes and their
    /// rigs, materials, textures — the kinds whose services read on demand; shaders and cloud assets have
    /// their own residency), then this thread blocks until each has landed (AsyncAssetLoader::AwaitOne, so
    /// SyncLoadLedger counts no in-frame load), then the meshes are built so the first frame draws them.
    ClosureResidency AwaitClosure( const std::vector<Assets::ContentRegistry::ClosureRow>& closure );

    /// Scene open: `AwaitClosure( SceneDependencies( scene ) )`, before the scene is initialised.
    ClosureResidency AwaitSceneClosure( const Core::Scene& scene );

    /// One asset and everything its row depends on — a dropped mesh, a material about to be photographed.
    ClosureResidency AwaitAssetClosure( const Assets::AssetHandle& handle, Common::Content::ContentKind kind );

    /// A mesh named only by its handle (a scene's MeshGuid): known to MeshService, or discoverable from its
    /// content-registry row. Creates the shell; reads nothing.
    NO_DISCARD bool DiscoverMesh( const Assets::AssetHandle& handle );

    /// Register the texture behind @p handle with the TextureService as a lazy shell. Idempotent; a
    /// zero handle, or one the registry does not hold, is a no-op. The GPU upload stays deferred to the
    /// first draw — building here would move every texture in a scene onto the load.
    void EnsureTextureRegistered( const Assets::AssetManager& registry, uint64_t handle );

    /// Register @p skybox with the SkyboxService if it is not already there, loading it first. Idempotent.
    ///
    /// UNLIKE THE OTHER THREE THIS IS EAGER: `SkyboxService::Register` constructs the MaterialSkybox, whose
    /// constructor runs `EnvironmentManager::Create` — the panorama upload and the radiance, irradiance and
    /// prefilter bakes. The boot does not do it for every `.hdr` (AssetPreloader::PreloadSkyboxes only
    /// scans), so a scene reference that resolves WITHOUT coming through here leaves the service empty,
    /// the SkyboxCommand carries no cube and DeferredLighting shades with the black EMPTY environment.
    /// That is what the GUID spelling of a skybox reference did from SCNE 31 on: it found the scanned
    /// record and returned its handle, and nothing ever baked the sky. A refusal is logged with the file.
    void EnsureSkyboxRegistered( const Assets::Asset<Assets::SkyboxAsset>& skybox );

    /// WHAT A MESH IS ONCE SOMEBODY HAS ASKED FOR IT — the four states, told apart in one place.
    ///
    /// They exist as an enum because the three of them that are refusals were previously ONE message, and
    /// that message named the rarest cause. A caller that cannot tell "nothing registered it" from "it
    /// built nothing" from "it built an empty static buffer, which is what a skinned mesh is" reports the
    /// wrong one, and the next person debugs the wrong thing (DC §1.4: an answer that misnames its own
    /// reason is worse than no answer, because it is followed).
    enum class MeshReadiness
    {
        Drawable,      ///< built, and it has geometry in it
        NotRegistered, ///< the service refused the registration — nothing will ever build this
        NotBuilt,      ///< registered, but the build produced nothing (MeshService has logged why, with the file)
        NoSubmeshes    ///< built, and empty. A skinned mesh's STATIC buffer is empty by design, so this is
                       ///< a refusal for a capture and an acceptable state for an import.
    };

    /// The state that these three facts describe, and nothing else — no service, no device, no asset.
    /// Pure so that the mapping is a thing a test can hold to (Desert/Tests/Engine/SceneAssetRegistration).
    NO_DISCARD constexpr MeshReadiness ClassifyMeshReadiness( bool registered, bool built, size_t submeshes )
    {
        if ( !registered )
            return MeshReadiness::NotRegistered;
        if ( !built )
            return MeshReadiness::NotBuilt;
        if ( submeshes == 0 )
            return MeshReadiness::NoSubmeshes;
        return MeshReadiness::Drawable;
    }

    /// Register @p mesh (exactly as EnsureMeshRegistered does) and then BUILD it, reporting which of the
    /// four states resulted. For the callers that cannot defer: an import that must refuse a cook which
    /// produced nothing rather than place an entity that draws air, and a capture that must not file a
    /// photograph of empty sky as the picture of an asset.
    ///
    /// The `Get` inside is a BUILD, not a probe — the caller has already decided it needs the geometry —
    /// which is the distinction the `Get`-as-a-guard sites this file replaced never made.
    NO_DISCARD MeshReadiness EnsureMeshDrawable( const Assets::Asset<Assets::MeshAsset>& mesh,
                                                 Assets::AssetManager&                   registry );

    /// The sentence for a state, naming @p file. Pure, and the only wording of these four facts, so a
    /// caller cannot report one state with another's words — which is the defect this replaces.
    ///
    /// INLINE, LIKE `ClassifyMeshReadiness` ABOVE, so that the pair can be asserted by a suite that links
    /// nothing: putting it in the translation unit would make a test of the WORDING drag in
    /// `ResourceRegistry` and with it the whole renderer, and a claim about a message is not a claim that
    /// should need a device to check.
    NO_DISCARD inline std::string ExplainMeshReadiness( MeshReadiness state, const std::string& file )
    {
        switch ( state )
        {
            case MeshReadiness::Drawable:
                return "'" + file + "' is built and has geometry";
            case MeshReadiness::NotRegistered:
                return "'" + file +
                       "' could not be registered with the mesh service, so nothing will ever build it";
            case MeshReadiness::NotBuilt:
                return "'" + file +
                       "' is registered but built nothing — the mesh service has logged the reason against "
                       "this file";
            case MeshReadiness::NoSubmeshes:
                return "'" + file +
                       "' built no drawable geometry (a skinned mesh's static buffer is empty by design)";
        }
        // Unreachable for any enumerator above, and NOT a silent default: a state added without a sentence
        // must say so rather than borrow the nearest one, which is the whole defect this enum replaces.
        return "'" + file + "' is in a mesh-readiness state this build has no sentence for";
    }

} // namespace Desert::Runtime
