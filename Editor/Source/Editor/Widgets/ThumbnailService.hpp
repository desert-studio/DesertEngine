#pragma once

#include <Editor/Widgets/ThumbnailWarmup.hpp>
#include <Editor/Widgets/AssetThumbnailRenderer.hpp>
#include <Editor/Widgets/ThumbnailEncode.hpp>
#include <Editor/Widgets/ThumbnailFreshness.hpp>
#include <Editor/Widgets/ThumbnailPreview.hpp>

#include <Engine/Assets/AssetRootPin.hpp>

#include <Common/Core/ResultStr.hpp>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
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
     * site either: it is the chain extension -> FileType -> ThumbnailProducers::Producer
     * (Editor/Widgets/ThumbnailProducers.hpp), whose census makes a format with NO row a red test rather
     * than a silent grey icon.
     *
     * ONLY WHAT IS ON SCREEN IS CAPTURED, as in UE's content browser. Requests arrive from the panels
     * that draw a tile and from nowhere else: there is no project-wide sweep any more (owner decision В4,
     * TH2), because a capture costs a renderer slot and ~370 ms, and a picture for an asset nobody is
     * looking at is work bought for a screen that does not exist. A cached PNG is a different matter — it
     * is decoded ahead on a worker by Editor/Widgets/ThumbnailPrefetch.hpp, even while the splash is up.
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
     * AND IT NEVER TAKES THE LAST OF THE VIEW BUDGET. A capture owns a full SceneRenderer — a view whose
     * targets and per-view copies are paid from the byte budget (Engine/Core/ViewBudget.hpp). This queue
     * is background work: nobody clicked for it, and what it produces is the picture a row shows
     * precisely WHILE the person cannot have a live preview. Taking the last of the budget would therefore
     * starve the surface they are opening in order to render its consolation prize. The entitlement is stated
     * once, for both consumers of it, in Engine/Core/ViewBudget.hpp; when it says no, the queue is kept and the
     * refusal is LOGGED, because a queue that quietly stops draining reads exactly like a queue with nothing in
     * it.
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
        // The resolved subject whole (ThumbnailSubject::ResolveMaterial): the only form that can carry a
        // preview mesh, so the only one a Preview::Mesh material is photographed through. The three-argument
        // form with How == Mesh is refused at dispatch, by name.
        std::string RequestMaterial( const ThumbnailSubject::Material& material, const std::string& assetPath );
        // A material a panel already holds LOADED (Details slot, mesh row): the route and PreviewMesh are
        // resolved (ThumbnailSubject::ResolveLoadedMaterial) only when a capture is owed, so a fresh picture
        // costs no record read per frame. A refusal is logged once and answers "" (no rendered picture).
        std::string RequestLoadedMaterial( Assets::AssetManager&                                manager,
                                           const std::shared_ptr<Assets::SurfaceMaterialAsset>& asset,
                                           const std::string&                                   assetPath );

        // An asset a panel could not even ROUTE (ThumbnailSubject refused it: a domain no producer draws, an
        // instance whose chain names no template, an unreadable file). Logged ONCE with @p reason and entered
        // in the failure set, so the card keeps its type icon for a stated reason — never silently (THM1n-10).
        void Refuse( const std::string& assetPath, const std::string& reason );

        // Queue a mesh preview, optionally with the material to apply to every slot.
        std::string
        RequestMesh( const Assets::AssetHandle& mesh, const std::string& assetPath,
                     const Assets::AssetHandle& material = Assets::AssetHandle( static_cast<uint64_t>( 0 ) ) );
        // Queue a posed picture (THM1n-6, THM-FIXB), keyed and judged like RequestMesh on @p pose.CookedPath —
        // the subject's own file (.skmesh, .skeleton, .anim; ThumbnailPose::ResolvePoseSubject) — the preview
        // mesh in pose.Handle, the clip (null: bind pose) in pose.Clip. The same enqueue as RequestMesh.
        std::string RequestPose( const ThumbnailSubject::Mesh& pose );

        /// Queue a SKYBOX picture (ThumbnailProducers::Producer::RenderedSky): the panorama `.detex` @p assetPath
        /// (the skybox row's file) drawn by the scene's skybox under the dome camera. Keyed on the asset path
        /// and judged by its content hash, like a material; every shower (Content Browser tile, Details Skybox
        /// row) asks this and draws the PNG exactly when JudgeSkyboxPicture says Show.
        std::string RequestSkybox( const Assets::AssetHandle& skybox, const std::string& assetPath );
        [[nodiscard]] static ThumbnailFreshness::Verdict JudgeSkyboxPicture( const std::string& assetPath );

        /// THE ONE JUDGEMENT OF A MESH PICTURE (UE: ThumbnailTools' one freshness answer per asset key). Every
        /// shower of a static, skinned or posed mesh picture — the Content Browser tile, the Details slots —
        /// asks this and draws the PNG exactly when it says Show; RequestMesh/RequestPose gate their enqueue on
        /// the same key (MeshRequestOf) and the same hash (SourceHash), so a shower that says Capture is a
        /// service that queues, and a repeated request of a queued picture is a no-op.
        [[nodiscard]] static ThumbnailFreshness::Verdict JudgeMeshPicture( const std::string& cookedPath );
        /// The same for a material picture: RequestMaterial's key (the asset path) and hash (its content).
        [[nodiscard]] static ThumbnailFreshness::Verdict JudgeMaterialPicture( const std::string& assetPath );

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

        // Called ONCE per frame by EditorLayer — not by panels, so a hidden or closed panel neither
        // starves nor double-ticks either half. TWO halves because they answer to different gates:
        //
        // TickDiskAndDecode — collect and start worker decodes of PNGs already in the disk cache
        //   (ThumbnailPrefetch). No renderer, no device: allowed while the splash is up
        //   (Splash::ThumbnailDiskDecodeAllowed), so the first frame after the window is shown only uploads.
        // TickCapture — the renderer capture queue and the CPU cloud paint. Waits for the window
        //   (Splash::ThumbnailCaptureAllowed): a capture takes a renderer slot and the settle's frames.
        static void TickDiskAndDecode();
        // @p scope: Everything after the hand-over; SceneWarmOnly on the splash (THUMB3), where only what
        // WarmMaterial queued may be dispatched and the folder's requests wait behind it for the reveal.
        void TickCapture( ThumbnailWarmup::CaptureScope scope );

        /// Queue a capture of a material the OPEN SCENE uses, ahead of everything the browser asked for
        /// (THUMB3; UE renders what is on screen first). Already queued -> moved forward; already fresh on
        /// disk, failed or in flight -> nothing. These are the only captures the splash may run.
        void WarmMaterial( const ThumbnailSubject::Material& material, const std::string& assetPath );
        /// The same for a mesh (THM1m): the open scene's meshes and the opening folder's uncaptured mesh tiles
        /// are photographed on the splash too, keyed on the cooked form as RequestMesh keys them.
        void WarmMesh( const ThumbnailSubject::Mesh& mesh );
        /// The same for a skinned mesh's pose (ThumbnailPose::ResolveSkinnedMesh's answer).
        void WarmPose( const ThumbnailSubject::Mesh& mesh );
        /// The same for a skybox (THM-FIXH): RequestSkybox's request — key the `.detex` path, freshness its
        /// content hash — at the front, so the splash photographs every skybox of the project as it does every
        /// material.
        void WarmSkybox( const Assets::AssetHandle& skybox, const std::string& assetPath );
        /// THM1n-13: a painted picture of the project warmed on the splash — RequestPainted, counted in
        /// SceneWarmPending until it lands, and painted before the hand-over (TickCapture(SceneWarmOnly) runs the
        /// paint queue while its front is a warm one).
        void WarmPainted( const std::string& assetPath );
        /// Scene-warm captures still queued or in flight: what holds the hand-over within its budget.
        [[nodiscard]] std::size_t SceneWarmPending() const;

        /**
         * @brief THE LIVE PREVIEW of Edit Thumbnail (UE renders the tile in real time with the orbit being
         *        dragged): a capture of the subject seen from @p orbit, an orbit NOT stated anywhere yet.
         *
         * Through the same renderer and the same dispatch as every capture - no second renderer - but into
         * ThumbnailKey::PreviewPath, never recorded, never judged fresh and never the cached thumbnail. One slot,
         * the last request wins (ThumbnailPreview::Slot), dispatched ahead of the background queue because a
         * person is dragging. Returns the preview PNG to draw once it exists (ThumbnailCache re-decodes it on
         * every rewrite). The gesture's end is EndPreview; the orbit it settles on is written by
         * ThumbnailEdit::EditOrbit and re-shot by freshness like any edit.
         */
        std::string RequestPreviewMaterial( const ThumbnailSubject::Material& material,
                                            const std::string& assetPath, const Assets::ThumbnailOrbit& orbit );
        std::string RequestPreviewMesh( const ThumbnailSubject::Mesh& mesh, const Assets::ThumbnailOrbit& orbit );
        /// The same for a posed picture (a skinned source's .skmesh, ThumbnailPose::ResolvePoseSubject): captured
        /// as RequestPose captures it, keyed on @p pose.CookedPath.
        std::string RequestPreviewPose( const ThumbnailSubject::Mesh& pose, const Assets::ThumbnailOrbit& orbit );
        /// The gesture on @p assetPath ended: a waiting preview is dropped (one in flight still lands).
        void EndPreview( const std::string& assetPath );
        /// A preview of @p assetPath has landed since its gesture began: the PreviewPath file is this gesture's.
        [[nodiscard]] bool PreviewLanded( const std::string& assetPath ) const;

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
            return CaptureOwed() || ( m_Renderer && m_Renderer->HasPending() ) || !m_PaintQueue.empty() ||
                   m_PaintInFlight.valid();
        }

    private:
        enum class Kind
        {
            Material,
            Mesh,
            Pose,  // a skinned mesh posed (AssetThumbnailRenderer::RequestPose)
            Skybox // a skybox's HDR under the dome camera (AssetThumbnailRenderer::RequestSkybox)
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
            // Materials with How == Mesh only: the mesh they are photographed on (ThumbnailSubject::Material).
            Assets::AssetHandle PreviewMesh{ static_cast<uint64_t>( 0 ) };
            // THE ASSET'S THUMBNAIL INFO, carried to the renderer (the only thing it frames by): a material's
            // whole info, a mesh's orbit in Thumbnail.Orbit (its primitive and PreviewMesh unused).
            Assets::ThumbnailInfo Thumbnail;
            // Poses only: the clip whose middle frame is photographed (an .anim's); null for the bind pose.
            // Held here, so the clip is resident until the capture is taken.
            std::shared_ptr<Assets::AnimationAsset> Clip;
        };

        // The renderer's own dispatch of @p req (a mesh, a material, a material on its preview mesh).
        Common::BoolResultStr Dispatch( const Request& req );
        // The live preview (RequestPreview*): one slot, last wins, never recorded.
        ThumbnailPreview::Slot<Request> m_Preview;
        // Dispatch or settle the preview. True when it used this tick's renderer turn.
        bool TickPreview( ThumbnailWarmup::CaptureScope scope );
        // Anything the renderer still owes: the background queue or the preview.
        [[nodiscard]] bool CaptureOwed() const
        {
            return !m_Queue.empty() || m_Preview.Waiting() || m_Preview.InFlight();
        }

        // Shared by both Request* entry points: decides whether the work is needed at all. Takes the
        // asset's IDENTITY (ThumbnailKey::Identity), never a raw path — the sets below are keyed on it.
        bool ShouldQueue( const std::string& identity, const std::string& png, std::optional<uint64_t> current );
        // THE ONE REQUEST SHAPE of a mesh-like capture (Mesh, Pose): keyed and judged on the cooked file, so the
        // browser tile, the splash and every kind ask for one picture of one file.
        static Request MeshRequestOf( Kind kind, const Assets::AssetHandle& mesh, const std::string& cookedPath,
                                      const Assets::AssetHandle& material );
        // THE ONE REQUEST SHAPE of a material capture (queued, warmed, previewed): its preview primitive or mesh
        // and its whole thumbnail info, keyed under @p identity and written to @p png.
        static Request MaterialRequestOf( const ThumbnailSubject::Material& material, std::string identity,
                                          std::string source, std::string png );
        // THE ORBIT FROM THE MESH'S PACKAGE (its import record) into @p req, read only when a capture is owed;
        // an unreadable record is said and the asset marked failed (false).
        bool ReadMeshOrbit( Request& req );
        // RequestMesh and RequestPose: one enqueue, one deduplication.
        std::string EnqueueMeshLike( Request req );
        // Identities WarmMaterial queued; an entry leaves with its m_Queued one (settled, failed or skipped).
        std::unordered_set<std::string> m_SceneWarm;

        // THE SUBJECT OF A QUEUED CAPTURE IS HELD RESIDENT, as UE's thumbnail renderer holds the object it
        // photographs (THM1n). A request names handles, and the eviction sweep that follows a scene load
        // releases every asset no scene names — which is every folder tile and a warmed mesh the scene does
        // not place: base/base_basic_pbr/base_basic_shaded were resolved on the splash, dropped by the sweep
        // ("Dropped 3 built mesh(es)") and then refused at dispatch as "not built in the MeshService", for
        // the rest of the session. One set of pins per identity, from the moment it is queued until it leaves
        // m_Queued (settled, failed, skipped or invalidated) — reconciled by HoldSubjects.
        std::unordered_map<std::string, std::vector<std::unique_ptr<Assets::AssetRootPin>>> m_Held;
        void                                                                                HoldSubjects();
        // The one queue-front insertion both Warm* entry points share.
        void Warm( Request req );

        // The identity-free half of the question: is the PICTURE on disk missing or out of date? Split out
        // because dispatch asks it a second time, when the dedup sets deliberately still hold the entry.
        static bool NeedsCapture( const std::string& png, std::optional<uint64_t> current );

        // WHAT A PICTURE OF @p source IS JUDGED AGAINST: the file's bytes, and for a mesh also the orbit its
        // import record states (MeshThumbnailFreshness) - an edit of the record re-shoots it (UE: Edit Thumbnail).
        static std::optional<uint64_t> SourceHash( Kind type, const std::string& source );

        /**
         * @brief Build the renderer — but only if a background job is entitled to a slot right now.
         *
         * The whole reason this is a function and not two lines in TickCapture(): the rule that a capture must
         * never take the LAST free renderer slot is a standing condition, and a condition written at the
         * one call site it happens to have today is a condition the second call site will not have. False
         * means "not now"; the queue is left standing and the refusal is logged, because a queue that
         * silently stops draining looks exactly like a queue with nothing in it.
         */
        bool AcquireRenderer();

        /// Dispatch and collect the CPU-painted queue. Split from TickCapture() so the two queues' state
        /// machines cannot come to share an early return — the renderer's `HasPending`/`AcquireRenderer`
        /// guards are about a device, and every one of them would silently stall the paint queue too.
        void TickPainted();

        // Created lazily — a session may never preview — and RELEASED again once the queue has been idle
        // for a while, because it owns a full SceneRenderer and therefore a view's worth of the byte budget
        // (Engine/Core/ViewBudget.hpp). Holding it for the rest of the session after one thumbnail
        // meant an editor that had ever shown the asset browser had that much less for the surfaces
        // the user actually opens; past the budget, a scene view is refused instead of sharing the main
        // viewport's camera with no error message at all.
        std::unique_ptr<AssetThumbnailRenderer> m_Renderer;
        std::vector<Request>                    m_Queue;
        // Keyed on ThumbnailKey::Identity, not on a path spelling, so two panels naming one asset
        // differently cannot each hold their own entry (see Invalidate).
        std::unordered_set<std::string> m_Queued; // asset identities currently queued or in flight
        std::unordered_set<std::string>
             m_Failed; // the renderer refused or wrote nothing: do not retry every frame
        // The dispatched capture, from dispatch until the renderer answers (ThumbnailFreshness::Capture).
        ThumbnailFreshness::Capture    m_Capture;
        int                            m_IdleTicks     = 0; // consecutive frames with no work
        ThumbnailEncode::CaptureBudget m_Budget;            // paces dispatch by main-thread ms (TH3)
        // Already said out loud that there was no slot to spare. Latched so the warning is one line per
        // stretch of scarcity rather than one per frame, and cleared — with its own line — the moment one
        // comes free, because "it is running again" is as much news as "it stopped".
        bool m_BudgetRefused = false;

        // What this run of the queue did, reported once when it drains. A capture that succeeds used to
        // say nothing at all, so "the editor is rendering previews" and "the editor has stopped bothering"
        // looked identical in a log — and the second is what M8 was reported as.
        int m_Captured = 0;
        int m_Skipped  = 0; // queued, then found already fresh before it was dispatched
        // The run's pace, for the one line that reports it: when its first capture was dispatched, and the
        // longest the queue waited on the budget with nothing in flight (CaptureBudget::kMaxWaitFrames bounds it).
        std::optional<std::chrono::steady_clock::time_point> m_RunBegan;
        int                                                  m_BudgetWaitFrames  = 0;
        int                                                  m_LongestBudgetWait = 0;

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
