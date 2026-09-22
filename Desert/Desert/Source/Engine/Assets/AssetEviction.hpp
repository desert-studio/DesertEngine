#pragma once

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetRootSet.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    /**
     * @brief RELEASING WHAT NOTHING STILL NEEDS — the first consumer of the ownership ledger.
     *
     * ── THE DEFECT, MEASURED ──────────────────────────────────────────────────────────────────────────
     *
     * Assets were loaded and never released. Opening sixteen scenes one after another in the editor grew
     * its resident set from 138 MB to 270 MB — and REVISITING a scene cost nothing, because everything it
     * needs is still loaded. The working set was therefore the union of every scene ever opened in the
     * session, not the scene that is open, and it only ever went up. On a big level with loading on the
     * fly that is not a slow leak; it is the end of the session.
     *
     * ── WHEN: ON SCENE CHANGE, AND NOT ON A MEMORY THRESHOLD ──────────────────────────────────────────
     *
     * Four triggers were considered.
     *
     *   * A MEMORY THRESHOLD is the obvious one and it is refused. It makes the moment of eviction depend
     *     on a number nobody authored, so the same project stutters on one machine and not another and no
     *     capture reproduces it; and with no streaming system to hide the reload, the frame that crosses
     *     the line is the frame that pays for a parse. UNREAL DOES NOT DO THIS EITHER, and it is worth
     *     saying because "UE does it" is the argument one would expect: `gc.LowMemory.MemoryThresholdMB`
     *     defaults to 0, i.e. off, and every low-memory path there only shortens the COLLECTION INTERVAL
     *     or raises a per-frame budget. Budget-driven eviction in UE exists solely in texture-mip
     *     streaming — below the object level entirely. Our reason is our own: without streaming, a
     *     threshold converts a memory problem into a frame-time problem at an unpredictable moment.
     *   * A TIMER has the same defect with a different clock, and adds one of its own: it fires while the
     *     user is working, so an asset can be released between the frame that showed it and the click
     *     that edits it.
     *   * MANUALLY is not a policy. It is offered as well — the editor's command palette carries "Action /
     *     Release unused assets", which raises the same request the scene change does — because a person
     *     profiling a level needs to be able to ask without changing scene. But a feature that only works
     *     when somebody remembers it is not a fix for a leak, which is why it is not the trigger.
     *   * A SCENE CHANGE is when the answer actually changes. It is the only moment at which the set of
     *     needed assets is different from what it was, it is already a moment the user waits through, the
     *     device is already idled there (SceneRenderer::RebindScene), and it is precisely the transition
     *     the measurement above indicts. It is also, not coincidentally, UE's dominant trigger in
     *     practice: level streaming out forces a collection (`GLevelStreamingForceGCAfterLevelStreamedOut`
     *     defaults to 1).
     *
     * ── HOW WE KNOW AN ASSET IS NOT NEEDED: REACHABILITY, NOT COUNTING ────────────────────────────────
     *
     * References live as HANDLES — 64-bit numbers in ECS components, in scene settings and inside other
     * assets' files. A number has no reference count, and `shared_ptr::use_count()` on the registry's own
     * pointer is meaningless: the registry holds one for every asset for ever and a component holds none.
     * So the set of needed assets is TRACED from roots (AssetRootSet), expanded through asset-to-asset
     * references, and everything outside the closure is released. Same conclusion as UE's mark and sweep,
     * for the same underlying reason.
     *
     * THE TRACE IS THE PART THAT CAN SILENTLY GO WRONG. One half is held by a census:
     * `Desert/Tests/Engine/AssetRoots` asserts that every `Assets::AssetHandle` member declared by any
     * component in Components.hpp is visited by the root walk. A middle link that silently drops a
     * reference is this project's most repeated defect shape, and an unvisited component would present
     * as "the mesh vanished after opening another level" — with nothing in the log.
     *
     * THE OTHER HALF IS NOT HELD BY A CENSUS, AND THIS PARAGRAPH USED TO SAY IT WAS. It claimed that
     * `Desert/Tests/Engine/AssetEviction` "asserts that every asset class which names another asset is
     * expanded here", and no such assertion existed — the accessors `EdgesOf` reads did not appear in
     * that suite at all. Measured by deleting each of the five edges in turn and running it:
     *
     *     skinned mesh -> skeleton ........ RED     material -> textures ....... RED
     *     mesh -> materials ............... GREEN   retarget -> source rig ..... GREEN
     *     cloud type -> noise volume ...... GREEN
     *
     * Three of five could be removed in silence, under a header promising they could not. The mesh edge
     * now has a test; the last two are owed a fixture and the suite says so where they are missing. The
     * shape this is an instance of has a name in this project — a stated guarantee the tree does not
     * honour — and the rule is the same every time: find the line that DOES the thing.
     *
     * ── WHAT HAPPENS WHEN SOMETHING TOUCHES AN EVICTED ASSET: IT RELOADS ──────────────────────────────
     *
     * Transparently, and it is not new machinery: `MeshService::Get` and `MaterialService::Get` already
     * build on a miss from the registered shell, which is the lazy-registration path the preloader uses.
     * Eviction drops the BUILT object and keeps the shell, so the next `Get` rebuilds it — the same code
     * path as a first use, and the reload is a file read the user already paid for once.
     *
     * WE ARE DELIBERATELY STRICTER THAN UNREAL HERE. `TSoftObjectPtr::Get()` on an unloaded object returns
     * a silent `nullptr`; only an explicit `LoadSynchronous()` reloads. That is §1.4's forbidden shape —
     * an empty successful answer — and the caller cannot tell "no such asset" from "released, ask again".
     * Nothing in this engine may answer that way, so every eviction target is required to have a rebuild
     * path BEFORE it may be evicted, and the ones that do not have one are not evicted at all (below).
     *
     * ── WHAT IS EVICTED, AND WHAT IS NOT, AND WHY ─────────────────────────────────────────────────────
     *
     * EVICTED:
     *   * Every unreachable asset's CPU payload, through `AssetBase::Unload()`. This is the bulk of it —
     *     a mesh's vertex and index vectors are a full second copy of what is already in the device
     *     buffers, a `.dcmv`'s voxels are 4 MiB each, a prefab's entity records are unbounded.
     *   * `MeshService`'s built vertex and index buffers. Rebuilt by `Get` on the next draw.
     *   * `MaterialService`'s built runtime materials, through the EXISTING `Invalidate()` — which parks
     *     them in the graveyard rather than destroying them, because a frame in flight may still be
     *     recording against their descriptor pools, and `CollectGarbage()` destroys them at a safe point
     *     after a device idle. That machinery was already there and is the reason materials are safe to
     *     evict and images are not.
     *
     * NOT EVICTED, each for a stated reason rather than an omission:
     *   * `Graphic::Image` OF ANY KIND, including asset textures. The editor's
     *     `Desert::Editor::UI::UICacheTextureImGui` (Editor/Widgets/UIHelper/) holds a process-wide
     *     `unordered_map<VkImageView, ImTextureID>` that is never cleared and is
     *     keyed on a handle Vulkan is free to recycle. Releasing an image the editor has ever displayed
     *     would leave that map handing a live-looking `ImTextureID` to ImGui. Fixing it needs a deferred
     *     release queue for descriptor sets, which this engine does not have. The cost of the exclusion
     *     here is small and measured: one asset-backed `Image2D` was live across a sixteen-scene session.
     *   * CLOUD NOISE VOLUMES (`Image3D`, 8 MiB each). `CloudNoiseService::Get` has no build-on-miss — it
     *     does not keep the asset shell — so releasing one would make the sky silently fall back to the
     *     default volume. That is the forbidden shape above, so the volume stays until the service can
     *     rebuild. (The `.dcnv`'s CPU voxels ARE released; they are a second copy of what is on the
     *     device.)
     *   * SHADERS. A compiled `Graphic::Shader` is held by every pipeline built from it, so it is
     *     reachable by definition and dropping the service's map entry would free nothing. Shaders are
     *     not eviction's problem; the pipeline cache's growth is, and it is a different one.
     *   * THE ENVIRONMENT BAKE. Rebuilding it is ~450 ms of GPU work, not a file read, and it is claimed
     *     to `ResourceOwner::Environment` in the ledger precisely so that this rule is expressible rather
     *     than remembered.
     *   * ANY ASSET THAT ANSWERS `IsReloadableFromFile()` WITH FALSE — a prefab captured from a live
     *     entity, a procedurally generated clip, the Material Editor's working copy. Releasing one is data
     *     loss, and the asset says so itself; the refusal is counted and reported, never swallowed.
     */

    /**
     * @brief WHAT THE SWEEP DOES TO THE BUILT GPU OBJECTS — behind an interface, on purpose.
     *
     * The DECISION (which assets nothing reaches any more) is pure: a registry, a root set, and a graph
     * walk. The EFFECT reaches `Runtime::*Service`, which reaches Vulkan, which means a device — and a
     * suite that needs a device cannot run in CI and would never have caught the defect this task exists
     * to close.
     *
     * The same split ControlState.hpp records for the editor's snapshot: the one thing only the real
     * subsystem can do is behind the smallest possible interface, and everything after it is pure and
     * assertable. `Desert/Tests/Engine/AssetEviction` drives the whole sweep against a recording
     * implementation of this and asserts exactly which handles were dropped.
     */
    class IEvictionSink
    {
    public:
        virtual ~IEvictionSink() = default;

        /// Drop the built vertex/index buffers for @p handle, keeping the shell so `Get` can rebuild.
        /// Returns true when something was actually dropped.
        virtual bool DropBuiltMesh( const Common::AssetHandle& handle ) = 0;

        /// Is a runtime material built for @p handle? Asked separately from dropping it because the
        /// obvious way to test it — calling Get — BUILDS one.
        [[nodiscard]] virtual bool HasBuiltMaterial( const Common::AssetHandle& handle ) const = 0;

        /// Park @p handle's runtime materials for destruction, keeping the shell.
        virtual void DropBuiltMaterial( const Common::AssetHandle& handle ) = 0;

        /// Destroy what was parked, at a point where no frame is recording.
        virtual void CollectGarbage() = 0;
    };

    /// The real one: `Runtime::MeshService` and `Runtime::MaterialService`. Defined in
    /// AssetEvictionServices.cpp, which is the only translation unit here that reaches the GPU layer.
    [[nodiscard]] IEvictionSink& EngineEvictionSink();

    struct EvictionOutcome
    {
        /// Assets in the registry when the sweep ran.
        uint32_t Registered = 0;
        /// Roots, before the asset-to-asset expansion.
        uint32_t Roots = 0;
        /// The closure: roots plus everything they name, transitively.
        uint32_t Reachable = 0;
        /// Unreachable assets whose payload was released.
        uint32_t Released = 0;
        /// Unreachable, loaded, and `Unload()` said no — an in-memory payload with no file behind it.
        uint32_t Refused = 0;
        /// Unreachable and already not loaded. Counted separately from Released because "nothing to do"
        /// and "did something" must not be one number (§1.4) — a sweep that releases nothing because
        /// everything was already cold looks exactly like a sweep that is broken.
        uint32_t AlreadyCold = 0;
        /// Skipped because the type's lifetime is the PROJECT's and no root walk can reach it
        /// (Assets::IsProjectScopedAsset). Counted rather than passed over in silence: "nothing swept it"
        /// and "it was reachable" are different facts, and a sweep whose numbers do not add up to
        /// Registered is a sweep nobody can check.
        uint32_t ProjectScoped = 0;
        /// Built GPU objects dropped, by kind.
        uint32_t MeshesDropped    = 0;
        uint32_t MaterialsDropped = 0;
        /// Rows in the resource ledger before and after. The number the report quotes.
        uint32_t LedgerRowsBefore = 0;
        uint32_t LedgerRowsAfter  = 0;

        /// Every refusal, in full. They are the only outcome a person has to act on.
        std::vector<std::string> Refusals;

        [[nodiscard]] std::string Describe() const;
    };

    class AssetEviction final
    {
    public:
        /**
         * @brief Release every registered asset the trace from @p roots cannot reach.
         *
         * @param manager the registry to sweep. Nothing is REMOVED from it: an evicted asset keeps its
         *        record, its handle and its place in every lookup — only its payload is released. That is
         *        what makes the reload transparent, and it is why this returns no "forgotten" count.
         * @param roots what something still needs, with a reason per handle. Expanded here through
         *        asset-to-asset references before anything is released.
         * @param sink where the built GPU objects go. `EngineEvictionSink()` in production; a recording
         *        double in the suite.
         */
        [[nodiscard]] static EvictionOutcome Run( AssetManager& manager, const AssetRootSet& roots,
                                                  IEvictionSink& sink );

        /**
         * @brief EVERY ASSET @p handle NAMES — the graph's edges, for one node, in one place.
         *
         * PUBLIC BECAUSE IT HAS A SECOND READER NOW, and the second reader is the reason it is a
         * function rather than a loop body. The cooked asset registry (GAP_ANALYSIS T2.4) stores
         * dependency edges per asset so the cook can decide what travels together and so T2.5's size
         * map has something to rank. Those are the SAME edges the eviction closure walks, and writing
         * them out a second time would be this project's most repeated defect shape — two lists that
         * must agree, with nothing checking that they do.
         *
         * AN UNLOADED ASSET CONTRIBUTES NO EDGES, deliberately, and both callers want that: reading an
         * unloaded asset's references means parsing it, which is the work the demand-driven model
         * exists to avoid. For the sweep that is harmless (an unloaded asset is protecting nothing);
         * for the registry it means the edge column is as complete as the session that cooked it, and
         * nothing in the loader branches on it.
         *
         * @p visit is called once per edge with the handle AND the sentence that explains it,
         * including null handles — filtering them is the caller's, because the sweep wants them marked
         * and the registry does not want them stored. The reason travels with the edge rather than
         * being reconstructed at the call site: it is what an eviction refusal prints, and a second
         * wording of "why is this reachable" is the same drift in prose that the edge list is in code.
         */
        static void EdgesOf( AssetManager& manager, const Common::AssetHandle& handle,
                             const std::function<void( const Common::UUID&, const std::string& )>& visit );

    private:
        /// Add everything the assets already in @p closure name, until it stops growing. The edges come
        /// from `EdgesOf` so `Desert/Tests/Engine/AssetEviction` has one list to hold against the
        /// classes that actually have such a field.
        static void Expand( AssetManager& manager, AssetRootSet& closure );
    };

    /**
     * @brief WHEN THE SWEEP RUNS — the trigger, separated from the sweep itself.
     *
     * `AssetEviction::Run` is pure in the sense that matters: give it a registry and a root set and it
     * does the same thing every time, with no clock and no globals, which is what makes it testable
     * without a device. This is the impure half, and it is deliberately three lines of state.
     *
     * REQUESTED at a scene change (`Core::Scene::Init`), RUN from the frame loop before any command
     * recording begins (`Application::Run`, immediately before `Renderer::BeginFrame`). The two are
     * separate because the request happens while the OLD scene may still be alive — a sweep there would
     * see its assets as reachable and free nothing, which is the failure mode that makes "evict on scene
     * change" quietly do nothing. By the next frame's start the replaced scene has been destroyed and no
     * command buffer is open, which is also what makes it safe for the sweep itself to collect the
     * material graveyard before it reads the ledger back (see AssetEviction::Run).
     *
     * AND IT IS DEBOUNCED BY TWO QUIET FRAMES, which is not a detail — see the constant in
     * AssetEvictionServices.cpp for the measurement that put it there. Loading a level raises the request
     * more than once and on different frames, and a sweep fired on the first of them sees a half-built
     * world.
     */
    class AssetEvictionSchedule final
    {
    public:
        /// Ask for a sweep. Cheap and idempotent — several requests between two frames are one sweep.
        /// @p why is logged with the outcome so a reader knows which scene change caused it.
        static void Request( std::string why );

        /// Run the sweep if one was asked for, over every live registry (`AssetManager::LiveManagers`).
        ///
        /// @p collectRoots is CALLED ONLY WHEN A SWEEP IS DUE, which is why it is a callable and not a
        /// value: walking every live scene's components costs a pass over the world, and the caller is the
        /// frame loop. It is also what keeps the layering straight — the roots come from ECS components,
        /// which are ABOVE this layer, so the asset layer must be handed them rather than fetch them.
        static void RunIfDue( const std::function<AssetRootSet()>& collectRoots );
    };

} // namespace Desert::Assets
