#pragma once

#include <Editor/Widgets/ThumbnailSubject.hpp>

#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/Assets/Common.hpp>

#include <memory>
#include <string>

namespace Desert::Editor
{
    // Renders small offscreen previews of assets (a material on a sphere) and writes them to PNG files on
    // disk, shown in the asset browser grid via the normal ThumbnailCache (persist across restarts).
    //
    // The offscreen render is recorded into the editor's in-flight frame command buffer, so a CPU readback
    // of "this frame's" render races the GPU. The reliable pattern (verified): render the SAME material for
    // two consecutive frames and read back on the second frame — the readback (after WaitDeviceIdle) returns
    // the FIRST frame's already-submitted render. One capture is in flight at a time.
    class AssetThumbnailRenderer
    {
    public:
        // Waits for the GPU before releasing the scene, then the renderer that owns its passes. The same
        // order and the same reason as ~PreviewViewport: this is destroyed when the thumbnail queue has been
        // idle for a while (so the renderer slot goes back), which can happen while the last frame this
        // recorded into is still executing against its pipelines and descriptor pools.
        ~AssetThumbnailRenderer();

        /**
         * @brief Queue a material to be captured to outPng. Drives forward via Tick().
         *
         * Refuses (with the reason) when the handle is null or a capture is already in flight — see
         * RequestMesh for why these answer instead of returning void.
         *
         * @p how is NOT a preference and is not chosen here: it is the material's shader DOMAIN, decided
         * once by `ThumbnailSubject::PreviewRouteFor`. A ball, a camera-facing card, or the SKY the
         * material authors. This entry point used to take `bool flatPreview` and therefore had no way to
         * express the third picture, so every Volume-domain material in the project was queued as a mesh
         * draw and photographed as an empty sphere.
         */
        [[nodiscard]] Common::BoolResultStr RequestMaterial( const Assets::AssetHandle& materialHandle,
                                                             const std::string&         outPng,
                                                             ThumbnailSubject::Preview  how );

        /**
         * @brief Queue a mesh, auto-framed by its bounds, to outPng. If `material` is non-null it is applied
         *        to every slot; otherwise the mesh's own submesh materials are used.
         *
         * IT ANSWERS, AND THAT IS THE POINT. The mesh has to be BUILT in the MeshService — the handle alone
         * is not enough — and until now a handle the service did not have was accepted in silence: Tick()
         * cleared the material slots, framed a unit box around nothing, and captured the empty backdrop.
         * A 200 KB PNG of blank sky was then written, its modification time moved, and every layer above
         * read that as success: ThumbnailService counted "1 captured", the freshness rule called the file a
         * current picture of the asset, and the row drew a square of sky forever. Measured on this tree,
         * with a real 44 MB mesh a scene had referenced but nothing had loaded.
         *
         * That is the contract's §1.4 exactly — an empty successful answer is a silent wrong answer — and
         * it cannot be fixed after the render, because a picture of an empty scene is a legitimate picture
         * of some assets. It has to be refused BEFORE the capture, where the reason is still known.
         */
        [[nodiscard]] Common::BoolResultStr
        RequestMesh( const Assets::AssetHandle& meshHandle, const std::string& outPng,
                     const Assets::AssetHandle& material = Assets::AssetHandle( static_cast<uint64_t>( 0 ) ) );

        // Is a capture in flight? Gates requests to one at a time.
        [[nodiscard]] bool HasPending() const { return m_Phase != 0; }

        // Advance the capture state machine. Call ONCE per frame. Renders the pending material; on the
        // second frame it reads back the first frame's render and writes the PNG.
        void Tick();

    private:
        void EnsureInit();
        void FitTarget( const glm::vec3& center, float worldSize );
        void RecordRender();

        /// Put the scene into the shape this capture needs — the object on its ball, or the cloud layer
        /// under the dome camera — and take the other one down. Called every tick of a capture, because
        /// the scene is shared between the three pictures and only one of them may be standing.
        void StageSubject();

        /// True while the dome must keep rendering without counting a warm-up frame: the modelling volume
        /// bakes on a worker and the march accumulates over frames, so an early readback photographs the
        /// dither rather than the cloud (desert-engine-verify §1).
        [[nodiscard]] bool DomeIsStillSettling();

        std::unique_ptr<Graphic::SceneRenderer> m_Renderer;
        // Fully qualified: a Desert::Editor::Core namespace also exists (ViewportMode/FoliagePaint), so an
        // unqualified Core::Scene would wrongly resolve there in TUs that see it.
        std::shared_ptr<::Desert::Core::Scene>  m_Scene;
        // No camera entity: an OBJECT capture goes through the scene's own EditorCamera, which Scene::Init
        // publishes as the main camera. See EnsureInit for why a CameraComponent here read as load-bearing
        // and was not.
        ECS::Entity                             m_Target;
        bool                                    m_Inited = false;

        // ── THE DOME'S OWN FURNITURE, created on the first Volume capture and never before ─────────────
        //
        // A cloud layer brings a modelling volume of 8 MiB and a sky-occlusion volume of 2 MiB with it,
        // and its first bake blocks a worker — so a project with no cloud material must not be paying for
        // one by existing. Same argument, and the same lazy creation, as PreviewViewport's layer.
        ECS::Entity m_CloudLayer;
        // The dome does not orbit and has nothing to fit: it stands on a rise and LOOKS. That is a
        // different camera from the object capture's, not a different pose of it — 96 degrees of vertical
        // field and a 60 km far plane against a fitted subject at arm's length.
        std::shared_ptr<::Desert::Core::GameplayCamera> m_DomeCamera;
        // The camera Scene::Init made, kept so an object capture can be given it back by NAME. Reading
        // it out of the scene again after a dome capture would get the dome's.
        std::shared_ptr<::Desert::Core::Camera> m_ObjectCamera;

        Assets::AssetHandle m_PendingHandle{ static_cast<uint64_t>( 0 ) };
        Assets::AssetHandle m_PendingMaterial{ static_cast<uint64_t>( 0 ) }; // mesh's linked material (0 = default)
        std::string         m_PendingPng;
        // WHAT IS BEING PHOTOGRAPHED. A named question rather than the bool this replaced: "not a mesh"
        // is not the same statement as "a material", and the branch that reads it should not have to know
        // that they coincide.
        enum class Subject
        {
            Material,
            Mesh
        };
        Subject m_PendingSubject = Subject::Material;
        // How a MATERIAL capture is drawn. Meaningless unless m_PendingSubject is Material.
        ThumbnailSubject::Preview m_PendingPreview = ThumbnailSubject::Preview::Sphere;
        int                 m_Phase = 0; // 0 = idle, else = remaining render frames (capture on the last)

        // Frames the dome has left to settle before the warm-up counts. Reset whenever a bake is seen
        // running, so the window is measured from the END of the bake rather than from the request.
        int m_DomeSettle = 0;
        // Total frames this dome capture has spent settling. A bound, not defensive programming: a bake
        // that never reports done would otherwise hold the single capture slot for the whole session and
        // stop every other thumbnail in the project.
        int m_DomeFrames = 0;

        // THE PNG IS THE DISPLAY SIZE, and this used to be four times larger than anything could show.
        //
        // The old rule was "hi-res on disk, decoupled from the tiny on-screen size" — 1024 px written,
        // 2048 px rendered. But ThumbnailCache::Get is the ONLY reader of these files and it box-averages
        // every one of them down to kThumbMaxDim before it uploads anything, so the extra pixels were not
        // stored for later: they were decoded and thrown away on every load, in every session, forever.
        // Measured on this tree (Debug, and the machine was shared):
        //
        //   capture, final frame     2823 ms = 892 device idle + 1289 readback (16 MB) + 114 downscale
        //                            + 528 png encode
        //   cache HIT, per thumbnail   38 ms = 31 png decode (1024x1024) + 7 box filter and upload
        //   on disk                   961 KB per material, 106 materials in this project alone
        //
        // Two thirds of a capture and all of a cache hit were paid for resolution that never reached a
        // pixel. Matching kSize to kThumbMaxDim removes the load-time box filter entirely (the decode
        // lands at the size it is uploaded at) and quarters both the readback and the encode.
        //
        // NOT smaller than the display, which is the failure in the other direction: v3 exists because
        // 128 px "looked like 240p" in the grid. kThumbMaxDim was raised 256 -> 512 in this same change
        // (the largest grid card is 528 physical pixels on a 2x display — see ThumbnailCache.hpp for the
        // arithmetic), so what reaches the screen gets SHARPER here, not softer. This is the first version
        // in which the pixels stored are the pixels drawn.
        static constexpr uint32_t kSize         = 512;       // output PNG size == ThumbnailCache::kThumbMaxDim
        static constexpr uint32_t kRenderSize   = kSize * 2; // offscreen render size (2x supersample -> kSize)
        static constexpr int      kRenderFrames = 5;         // warm-up render frames before the capture readback

        // ── THE DOME'S FRAME BUDGET, and 5 warm-up frames is nowhere near enough for it ────────────────
        //
        // The volumetric march ACCUMULATES: below the convergence window a shot is a picture of the
        // dither, and the engine's own verification rule puts that window at ~10 frames and prescribes 90
        // for a cloud scene. 24 is measured against the middle of that: the temporal resolve has settled
        // and a background capture that cannot be seen is not worth three times the frames.
        //
        // COUNTED FROM THE END OF THE BAKE, not from the request, because the modelling volume is built
        // on a worker while these frames run and the march before it lands is a march through nothing.
        static constexpr int kDomeSettleFrames = 24;

        // The bound on that wait. A bake reported as running for ever would otherwise hold the editor's
        // ONE capture slot for the rest of the session, which stops every other thumbnail in the project —
        // so the capture proceeds and says so rather than disappearing.
        static constexpr int kDomeMaxSettleFrames = 900;
    };
} // namespace Desert::Editor
