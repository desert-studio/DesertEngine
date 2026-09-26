#pragma once

#include <Editor/Widgets/AssetThumbnailRenderer.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief One thumbnail renderer for the whole editor.
     *
     * Every panel that wanted an asset preview used to build its OWN AssetThumbnailRenderer, and each of
     * those owns a full Graphic::SceneRenderer. Three panels meant three extra scene renderers, each
     * ticking its own single-slot queue, each unaware that the panel next door had already captured the
     * same material. A measured scene render costs ~2.4 ms, so this was not free.
     *
     * It also produced a visible wart: the Details material slot deliberately did no rendering at all
     * (to avoid becoming the fourth renderer) and fell back to a flat colour swatch for any material the
     * asset browser had never happened to show. A material could sit there as a coloured square forever.
     *
     * This service is the single owner. Panels REQUEST and read; EditorLayer ticks it once per frame.
     *
     * IT OWNS TWO QUEUES AND ONLY ONE OF THEM COSTS A RENDERER SLOT. A material and a mesh are
     * PHOTOGRAPHED — an offscreen scene, a camera, one of six slots, ~370 ms. The four cloud formats are
     * PAINTED from their own bytes on a JobSystem worker (Editor/Widgets/CloudThumbnail.hpp), which needs
     * no device at all. Which of the two a format uses is not decided here and not decided at the call
     * site either: it is a column of Editor/Widgets/ThumbnailFormats.hpp, the census that also makes a
     * format with NO producer a red test rather than a silent grey icon.
     *
     * NOBODY HAS TO PRESS ANYTHING. Requests arrive from the panels that draw a tile, and — for every
     * asset in the project, whether or not a panel has ever walked past it — from the background sweep
     * (Editor/Widgets/ThumbnailSweep.hpp), which is what makes a cold cache fill itself after a scene is
     * opened and makes a file dropped into the content directory acquire a picture on its own.
     *
     * Requests are deduplicated across panels and across frames:
     *   - a PNG on disk that is still a picture OF its asset is never re-rendered (that is the persistent
     *     cache). "Still a picture of it" is Editor/Widgets/ThumbnailFreshness.hpp, the SAME rule every
     *     panel uses to decide whether to draw the file — not "the file exists", which is what this gate
     *     used to ask. The two questions differ for exactly the assets that need re-rendering, and while
     *     they differed those assets were neither drawn nor queued, permanently;
     *   - a request already queued or in flight is not queued twice, and one that has become unnecessary
     *     while it waited is dropped at dispatch rather than re-rendered;
     *   - an asset that failed to render is remembered and not retried, so a broken .demat cannot make
     *     the queue spin on it every frame forever. That memory is per-PROCESS on purpose: the usual
     *     reason a capture fails is a shader, a service registration or a device that was not ready yet,
     *     and persisting "this asset is bad" would turn a transient failure into one only a cache wipe
     *     could clear. The thing worth persisting is the picture, and that is what the PNG is.
     *
     * IT SURVIVES ASSET EVICTION, AND THAT IS A PROPERTY OF WHERE THE QUESTION IS ASKED, NOT LUCK. A queued
     * request carries a handle and a path, and a handle can go cold under it: A7 evicts the BUILT object on
     * a scene change and keeps the shell. What saves this queue is that nothing here trusts the handle at
     * queue time — `AssetThumbnailRenderer::RequestMesh` asks `MeshService::Get` at DISPATCH, which is the
     * lazy path that rebuilds from the shell, so an evicted mesh reloads on the way into the capture and an
     * unregistered one is refused with its reason. The dedup and failure sets are keyed on the asset's
     * identity rather than on a pointer, so they mean the same thing on both sides of an eviction.
     *
     * AND IT NEVER TAKES THE LAST RENDERER SLOT. A capture owns a full SceneRenderer, which is one of six
     * (Engine/Core/RendererSlotPool.hpp), and a renderer that finds none free does not fail — it records
     * into slot 0 and shares the main viewport's per-frame state. This queue is background work: nobody
     * clicked for it, and what it produces is the picture a row shows precisely WHILE the person cannot
     * have a live preview. Taking the sixth slot would therefore starve the surface they are opening in
     * order to render its consolation prize. The entitlement is stated once, for both consumers of it, in
     * Engine/Core/ViewBudget.hpp; when it says no, the queue is kept and the refusal is LOGGED,
     * because a queue that quietly stops draining reads exactly like a queue with nothing in it.
     */
    class ThumbnailService
    {
    public:
        static ThumbnailService& Get();

        // Queue a material preview if it is not already cached, queued or known-bad. Returns the PNG path
        // to read (which may not exist yet — draw a placeholder until it does).
        //
        // @p how is the material's shader DOMAIN made into a picture — a ball, a camera-facing card for a
        // cutout, or the sky a Volume-domain material authors. It has NO DEFAULT on purpose: the parameter
        // it replaces (`bool flatPreview = false`) let every caller that had not thought about the
        // question queue a cloud material as a mesh draw, which is what put a Volume-domain refusal in the
        // startup log. `ThumbnailSubject::PreviewRouteFor` is the one place that answers it.
        std::string RequestMaterial( const Assets::AssetHandle& material, const std::string& assetPath,
                                     ThumbnailSubject::Preview how );

        // Queue a mesh preview, optionally with the material to apply to every slot.
        std::string RequestMesh( const Assets::AssetHandle& mesh, const std::string& assetPath,
                                 const Assets::AssetHandle& material = Assets::AssetHandle(
                                      static_cast<uint64_t>( 0 ) ) );

        /**
         * @brief Queue a picture that is PAINTED ON THE CPU from the file's own bytes — the four cloud
         *        formats (Editor/Widgets/CloudThumbnail.hpp).
         *
         * A SECOND QUEUE, AND IT IS NOT SYMMETRY FOR ITS OWN SAKE. The two queues differ in the only
         * thing this class rations: a capture costs one of six renderer slots and drains at roughly one
         * asset per two seconds; a paint costs a file read and a fill on a JobSystem worker, claims no
         * slot at all, and cannot be refused by ViewBudget because it never asks. Putting them in
         * ONE queue would make every cloud asset wait behind whatever mesh happened to be in front of it
         * — and, worse, would make a project with six windows open (where the renderer is refused) stop
         * producing cloud thumbnails for a reason that has nothing to do with them.
         *
         * The two queues share the freshness gate, the identity keys and the failure set, because those
         * are questions about the ASSET rather than about who draws it. What is NOT shared is the
         * renderer, and that is the whole distinction.
         *
         * Takes no handle: the painter opens the file. See CloudThumbnail::Write for why that is what
         * makes it safe on a worker.
         */
        std::string RequestPainted( const std::string& assetPath );

        // Drive the capture state machine. Called ONCE per frame by EditorLayer — not by panels, so a
        // hidden or closed panel neither starves nor double-ticks it.
        void Tick();

        // Forget a cached/failed result, e.g. after the asset was edited.
        void Invalidate( const std::string& assetPath );

        /**
         * @brief Release the renderer NOW, while the device is still alive. Called from
         *        EditorLayer::OnDetach.
         *
         * WHY THIS IS SAID OUT LOUD INSTEAD OF LEFT TO THE DESTRUCTOR. This service is a function-static:
         * it is destroyed at `__cxa_finalize`, after main has returned and after ~Application has taken the
         * device and the VMA allocator with it. ~AssetThumbnailRenderer's first act is
         * Renderer::WaitDeviceIdle(), which then dereferences a null s_RendererAPI and segfaults — measured,
         * exit 139, with the backtrace naming exactly this chain:
         *
         *     Renderer::WaitDeviceIdle <- ~AssetThumbnailRenderer <- ~ThumbnailService
         *     <- __cxa_finalize_ranges <- exit
         *
         * This is the same family 0bfdeccf fixed for the engine-side registries ("process-lifetime caches
         * became the next thing to outlive the device"), and this is the member that fix missed. It was
         * missed for a understandable reason: EditorLayer::OnDetach already names "asset thumbnails" among
         * the GPU objects it tears down, but that comment is about the PANELS, and this service is a peer of
         * the panels rather than one of them — no m_Panels.clear() can reach it.
         *
         * A static destructor cannot be ordered against the device, so ordering is not something to get
         * right here; it is something to stop relying on. After this call the destructor has nothing left to
         * do, which is the point — the release is deterministic, not merely early.
         *
         * NOT a guard inside ~AssetThumbnailRenderer that skips the wait when the device is gone: that would
         * turn a broken teardown order into a silent one, and a leak that never reports itself is worse than
         * the crash that does.
         *
         * Idempotent, and NOT a one-way switch: the service is usable again afterwards, because this is the
         * same release the 300-idle-frame path performs and that path must keep working mid-session.
         */
        void Shutdown();

        [[nodiscard]] bool HasWork() const
        {
            return !m_Queue.empty() || ( m_Renderer && m_Renderer->HasPending() ) || !m_PaintQueue.empty() ||
                   m_PaintInFlight.valid();
        }

    private:
        enum class Kind
        {
            Material,
            Mesh
        };
        struct Request
        {
            Kind                Type = Kind::Material;
            Assets::AssetHandle Handle{ static_cast<uint64_t>( 0 ) };
            Assets::AssetHandle Material{ static_cast<uint64_t>( 0 ) }; // meshes only
            std::string         Identity; // ThumbnailKey::Identity of the asset, NOT a path spelling
            // The asset's own file. Carried so the freshness question can be asked AGAIN at dispatch: a
            // request can sit in this queue for seconds, and in that time the capture it asks for may
            // already have been done (two panels showing one asset) or made unnecessary.
            std::string Source;
            std::string Png;
            // Materials only: which of the three pictures this is. See ThumbnailSubject::Preview.
            ThumbnailSubject::Preview How = ThumbnailSubject::Preview::Sphere;
        };

        // Shared by both Request* entry points: decides whether the work is needed at all. Takes the
        // asset's IDENTITY (ThumbnailKey::Identity), never a raw path — the sets below are keyed on it.
        bool ShouldQueue( const std::string& identity, const std::string& png, const std::string& source );

        // The identity-free half of the question: is the PICTURE on disk missing or out of date? Split out
        // because dispatch asks it a second time, when the dedup sets deliberately still hold the entry.
        static bool NeedsCapture( const std::string& png, const std::string& source );

        /**
         * @brief Build the renderer — but only if a background job is entitled to a slot right now.
         *
         * The whole reason this is a function and not two lines in Tick(): the rule that a capture must
         * never take the LAST free renderer slot is a standing condition, and a condition written at the
         * one call site it happens to have today is a condition the second call site will not have. False
         * means "not now"; the queue is left standing and the refusal is logged, because a queue that
         * silently stops draining looks exactly like a queue with nothing in it.
         */
        bool AcquireRenderer();

        /// Dispatch and collect the CPU-painted queue. Split from Tick() so the two queues' state
        /// machines cannot come to share an early return — the renderer's `HasPending`/`AcquireRenderer`
        /// guards are about a device, and every one of them would silently stall the paint queue too.
        void TickPainted();

        // Created lazily — a session may never preview — and RELEASED again once the queue has been idle
        // for a while, because it owns a full SceneRenderer and therefore one of the six renderer slots
        // (Engine/Core/RendererSlotPool.hpp). Holding it for the rest of the session after one thumbnail
        // meant an editor that had ever shown the asset browser had five slots, not six, for the surfaces
        // the user actually opens; past six, a scene view records into slot 0 and shares the main
        // viewport's camera with no error message at all.
        std::unique_ptr<AssetThumbnailRenderer> m_Renderer;
        std::vector<Request>                    m_Queue;
        // Keyed on ThumbnailKey::Identity, not on a path spelling, so two panels naming one asset
        // differently cannot each hold their own entry (see Invalidate).
        std::unordered_set<std::string>         m_Queued;  // asset identities currently queued or in flight
        std::unordered_set<std::string>         m_Failed;  // gave up: do not retry every frame
        // The dispatched capture, kept past a give-up so a late PNG still gets its record (TH1c).
        ThumbnailFreshness::Capture                    m_Capture;
        int                                            m_InFlightTicks = 0;
        int                                            m_IdleTicks     = 0; // consecutive frames with no work
        // Already said out loud that there was no slot to spare. Latched so the warning is one line per
        // stretch of scarcity rather than one per frame, and cleared — with its own line — the moment one
        // comes free, because "it is running again" is as much news as "it stopped".
        bool m_BudgetRefused = false;

        // What this run of the queue did, reported once when it drains. A capture that succeeds used to
        // say nothing at all, so "the editor is rendering previews" and "the editor has stopped bothering"
        // looked identical in a log — and the second is what M8 was reported as.
        int m_Captured = 0;
        int m_Skipped  = 0; // queued, then found already fresh before it was dispatched

        // ── THE SLOT-FREE HALF ────────────────────────────────────────────────────────────────────────
        //
        // Requests whose picture is computed from the file's own bytes. They run on the JobSystem — NOT
        // on a std::thread and NOT on a bare std::async, which is the rule Г9 enforced for the cloud bake
        // and for the same two reasons: work outside the pool is invisible to the profiler and obeys no
        // shared budget. `JobSystem::Async` returns a future, and this holds exactly one at a time.
        //
        // ONE AT A TIME, deliberately, even though the work is thread-safe and the pool has cores to
        // spare. A paint reads a whole container — 8 MiB for a `.dcnv` — so a project with two hundred of
        // them dispatched at once is 1.6 GB of transient buffers and a saturated disk queue, to produce
        // pictures for tiles nobody is looking at yet. The sweep is background work; its throughput has
        // never been the scarce thing.
        struct PaintRequest
        {
            std::string Identity;
            std::string Source;
            std::string Png;
        };
        std::vector<PaintRequest>          m_PaintQueue;
        std::future<Common::BoolResultStr> m_PaintInFlight;
        std::string                        m_PaintInFlightIdentity;
        std::string                        m_PaintInFlightSource;
        int                                m_Painted = 0; ///< reported with m_Captured when the queue drains
    };
} // namespace Desert::Editor
