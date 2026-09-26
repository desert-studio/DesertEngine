#pragma once

#include <Engine/Assets/Common.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/SkyAtmosphereComponent.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>

#include <Editor/Widgets/PreviewInput.hpp>

#include <ImGui/imgui.h>

#include <functional>
#include <memory>
#include <vector>

namespace Desert::Editor::UI
{
    class UIHelper;
}

namespace Desert::Editor::Render
{
    class EditorCubemapPreviewPass;
    class EditorGridPass;
}

namespace Desert::Graphic
{
    class ImageCube;
}

namespace Desert::Editor
{
    // A LIVE asset preview: its own tiny scene (target + key light + procedural sky) rendered offscreen
    // every frame and shown as an image you can orbit. The Details panel's answer to "what does this mesh
    // / material actually look like".
    //
    // Not the same job as AssetThumbnailRenderer, which captures a PNG over several frames for the asset
    // browser grid and cannot move its camera (the scene's auto-created main camera is the input-driven
    // EditorCamera, so thumbnails are framed by SCALING the object). This one installs its own
    // GameplayCamera as the scene's active camera and drives it from orbit angles, so it can be turned,
    // zoomed and framed properly.
    //
    // FRAME ORDERING IS PART OF THE CONTRACT: Update() records a scene render and must run from a panel's
    // OnPreUpdate(); Draw() only shows the last image and handles input, from OnUIRender(). Rendering from
    // inside the ImGui pass would destroy descriptor pools whose sets are bound to the recording command
    // buffer — the editor has been bitten by exactly that (see ViewportPanel's deferred resize).
    class PreviewViewport
    {
    public:
        // OUT OF LINE, both of them, and the constructor for a reason that is not obvious: m_CubemapPass
        // is a unique_ptr to a type this header only forward-declares, and a DEFAULTED-INLINE constructor
        // instantiates that member's ~unique_ptr right here (the compiler needs it to unwind if the
        // constructor throws). Every panel that says `make_unique<PreviewViewport>()` then needs the
        // complete pass type, which is exactly the include this forward declaration exists to avoid.
        PreviewViewport();
        // Waits for the GPU before releasing the scene: this owns pipelines, framebuffers and descriptor
        // pools that a submitted frame may still be executing against.
        ~PreviewViewport();

        PreviewViewport( const PreviewViewport& )            = delete;
        PreviewViewport& operator=( const PreviewViewport& ) = delete;

        enum class Shape
        {
            Sphere, // material preview default
            Cube,
            Plane,   // right for cutout / foliage materials — a grass card garbles on a sphere
            Cylinder // a curved surface with a seam and two caps: what a tiling material reads wrong on
        };

        // WHAT FILLS THE PANE — the widget's real extension axis, named instead of inferred. It used to
        // be deduced from which members happened to be set (a mesh handle, a cubemap pass), and that was
        // fine while there were two kinds; the sky dome is a third whose CAMERA is different from both,
        // and a camera chosen by "no mesh handle and no cubemap pass" is a rule nobody can read.
        enum class Fill
        {
            Empty,
            Object,  // a primitive or a measured mesh, orbited. Surface domain.
            Cubemap, // an HDR environment wrapped on a ball, orbited. Skybox domain.
            SkyDome  // a sky the material itself authors, looked AT from the ground. Volume domain.
        };

        // THE PREVIEW'S OWN WORLD, as distinct from the material on show — the distinction screen 16 of
        // the mock is entirely about. These are settings of the SCENE the asset is shown in: which sky,
        // where the sun is, whether there is a floor under the object. They are NOT part of the material
        // and are deliberately not saved with it.
        //
        // Held here rather than in the panel because the widget is what owns the entities they drive AND
        // what receives the mouse: the light is turned by dragging inside the image (hold L), so a copy
        // living in the panel would have to be written back from Draw(). One value, two readers.
        struct SceneSetup
        {
            // ── Environment ───────────────────────────────────────────────────────────────────────────
            // The palette, from the SHARED table (Graphic::kSkyPresets) an artist also picks from in the
            // Details panel. Not hand-authored numbers: that is what StudioNeutral was extracted for.
            ECS::SkyPreset Sky = ECS::SkyPreset::StudioNeutral;
            // Multiplies the preset's own SkyBrightness. A separate field rather than an edit of the
            // palette, so switching preset does not silently discard it.
            float SkyIntensity = 1.0f;

            // THE GRADE THIS PANE VIEWS THE ASSET THROUGH, and it is here because it was the single largest
            // way the preview disagreed with the scene — larger than every march and bake budget put
            // together by a factor of twenty-two.
            //
            // It used to be nothing at all: the preview scene took Core::SceneSettings::Exposure's struct
            // default of 1.0, which is not a choice anybody made. Fifty of the fifty-one scenes in this
            // repository that carry a cloud layer author 0.26 (the one that does not is Clouds_Sunset), so
            // a cloud material was being authored at four times the exposure every level but one shows it
            // at. Measured on Clouds_ShadowsOnGround from one camera: 100 % of pixels differ, mean 78.0 of
            // 255, frame mean 134.6 against 212.6, against a repeat-shot floor of exactly 0.
            //
            // 1.0 STAYS THE DEFAULT AND THE DOME CHANGES IT, which is the arrangement LightIntensity is
            // already on two fields below: a studio ball under a neutral dome is lit and graded as a studio
            // subject, and a sky is not. ShowCloudMaterial writes the outdoor grade for the same reason it
            // writes the outdoor sun, and a person who is matching a particular level edits this row.
            float Exposure = 1.0f;

            // ── The key light, which is ALSO the sun in the sky ────────────────────────────────────────
            //
            // ONE VECTOR, and the mock is emphatic about it: the light that lights the object and the sun
            // baked into the sky are the same thing here (SkyboxECSSystem::ResolveAtmosphereSun reads the
            // directional light's transform). Two independent controls would let an artist light a
            // material from the left under a sun visibly on the right — and then trust it.
            //
            // Authored as the two angles a person can read and a drag can move; the travel vector the ECS
            // wants is derived in LightTravel(). Yaw is a compass bearing about +Y, pitch the sun's
            // ELEVATION above the horizon.
            float     SunYawDegrees   = -22.0f;
            float     SunPitchDegrees = 42.0f;
            float     LightIntensity  = 3.5f;
            glm::vec3 LightColor{ 1.0f, 0.97f, 0.92f };

            // ── The floor ─────────────────────────────────────────────────────────────────────────────
            //
            // The cast shadow is the point of it, and the only part that is not free — see
            // Graphic::ShadowQuality for what it costs and why it could not be had before.
            bool  ShowFloor           = true;
            bool  FloorCastsShadow    = false; // the floor itself is not a caster; the SUBJECT is
            bool  FloorReceivesShadow = true;
            float FloorSize           = 400.0f; // world units (cm), so 4 m
            // AUTHORED, because the engine's default material is white and a white Lambertian floor under
            // a noon sun clips — and a shadow on a clipped surface is not visible, which would have made
            // "the floor is for the shadow" a claim the frame did not support. 0.30 sits near a mid grey
            // once the sun and the sky ambient are added, which is where a shadow reads.
            glm::vec3 FloorColour{ 0.30f, 0.30f, 0.31f };

            bool ShowGrid = false;

            // ── The sky dome's own budget (Fill::SkyDome only) ─────────────────────────────────────────
            //
            // A cloud march on every frame the window is open is the dome's whole cost, and MaxSteps /
            // StopTransmittance are the two numbers that decide it. They are component fields
            // (ECS::VolumetricCloudData), so this is a smaller default rather than a new mechanism.
            int32_t CloudMaxSteps          = 96;
            float   CloudStopTransmittance = 0.03f;

            // THE DOME'S OTHER BUDGET, AND UNTIL O8 IT DID NOT EXIST. The two numbers above bound the MARCH
            // — what a frame costs. About half of a cloud material's parameters are inputs to a BAKE of the
            // modelling volume instead, and that one is not a frame: it is a loop over side x side columns
            // on a worker, and a 512-pixel preview pane was running exactly the one a whole level runs.
            // Measured on this machine, Debug: dragging Coverage twenty times in 1.02 s put 15.42 s between
            // the artist's last edit and the sky that showed it, which is what the owner reported twice as
            // "the cloud preview still doesn't update straight away".
            //
            // 128 AND NOT 64, AND THE NUMBER IS A MEASUREMENT RATHER THAN AN ARGUMENT. 128 quarters the
            // bake — 229 ms against 961 ms on this machine, Debug, minimum of six, re-measured after Г10
            // put the bake on the whole pool; it was 1 461 against 5 915 on one worker — and the six-point
            // sweep against the shipped 256 is the same clouds in the same places with softer edges (mean
            // 1.1 to 6.2 of 255, against a repeat floor of exactly zero). 64 would quarter it again and
            // does not survive the same check: it covers four more points of the sky than 256 does, and in
            // the frame a cumulus has swollen and a gap between two lobes has closed. The table and the
            // mechanism are on Assets::kCloudProceduralVolumeSideMin, which is that refusal expressed as a
            // bound — so this default deliberately SITS ON the floor: the cheapest grid measured honest.
            //
            // A FIELD RATHER THAN A CONSTANT, on exactly the terms the two above are already on: it is
            // ECS::VolumetricCloudData::VolumeResolution and ApplySetup writes it onto the layer every
            // frame.
            //
            // IT HAS NO ROW IN THE PREVIEW SCENE TAB, AND SINCE O13 IT NEEDS NONE — which is a stronger
            // answer than the row this comment used to owe. The row was owed because 128 could show a
            // different sky from the level's 256 and an artist had no way to ask for the level's; it is
            // not owed now, because 128 can no longer show a different sky. Graphic::CloudBakeSideForSpecies
            // raises the grid back to 256 for exactly the types the cheap one could not carry — two of the
            // nine shipped ones — and leaves the other seven on O8's measured saving. A row would be
            // offering a person the chance to pick the wrong answer.
            //
            // SO IT IS A FLOOR RATHER THAN THE SIDE. The value IS applied and it IS the cheapest grid the
            // pane will bake at; what it is not is a promise that the bake will use it.
            int32_t CloudVolumeResolution = 128;

            // Direction the light TRAVELS (sun -> scene), which is what TransformComponent::Translation on
            // a directional light means. Derived, never stored: two copies of one direction is how a sky
            // ends up lit from below.
            [[nodiscard]] glm::vec3 LightTravel() const;

            bool operator==( const SceneSetup& ) const = default;
        };

        // Show a mesh, auto-framed by its bounds. `materials` is applied slot-by-slot (pass the entity's
        // slots so the preview shows the real look); empty = the default material.
        void SetMesh( const Assets::AssetHandle& mesh, const std::vector<Assets::AssetHandle>& materials = {} );

        // Pin the shown mesh's LOD (-1 = auto by distance): writes the preview entity's
        // StaticMeshComponent::ForcedLOD, the one field the renderer reads. SetMesh resets it to auto, because
        // the target entity is reused across previews.
        void SetForcedLOD( int lod );

        // Show a material on a primitive.
        void SetMaterial( const Assets::AssetHandle& material, Shape shape = Shape::Sphere );

        // Show a CUBEMAP as an orbitable ball — the pane's content for a Skybox-domain material.
        //
        // THE EXTENSION POINT OF THIS WIDGET IS "WHAT FILLS THE PANE", NOT THE SHAPE LIST ABOVE. The
        // Sphere/Cube/Plane choice is a particularity of the surface domain (whose materials ride real
        // mesh geometry through the slot route); a domain the mesh path cannot draw brings its own
        // draw and reuses only what is genuinely domain-agnostic here — the scene, the orbit camera,
        // the framing and the renderer slot. This entry is the cubemap domain's draw (an external pass
        // ray-tracing the ball, EditorCubemapPreviewPass); a future Volume domain plugs in the same
        // way with a march, not by growing the Shape enum.
        //
        // @p resolveCube is called EVERY frame — the widget keeps no copy of the subject material's
        // state, so a cubemap dropped onto the material shows next frame with no invalidation call.
        // Resolving to null draws an empty pane; the REFUSAL prose for that state belongs to the
        // panel, which knows why (no slot in the schema vs nothing bound vs a dangling handle).
        void SetCubemapMaterial( std::function<Graphic::SampledCube()> resolveCube );

        // After SetCubemapMaterial: draw the cube as the pane's BACKGROUND too, not only on the ball — the
        // skybox viewer, where orbiting is looking around the sky. Any later Set*/Clear drops it again.
        void SetCubemapBackdrop( bool cubeIsBackdrop );

        // Show a VOLUME-domain material as the sky it authors: a preview world with ground, a sun and a
        // wide enough vertical lens that horizon, mid-elevation and zenith are in one frame.
        //
        // The third entry through the extension point described above, and the one that most needs it —
        // there is no shape here at all. A cloud material describes a MEDIUM: its layout is a painting on
        // a sky map, its vertical profile is base and top in kilometres on the CloudType assets it points
        // at, and one cell of its weather lattice is about 3 km wide. None of those has a meaning on a
        // one-metre ball, and wrapping them onto one is the same class of scale error as the "clouds hang
        // too low" complaint (a cell width disagreeing with a layer thickness).
        //
        // THE GROUND IS NOT SCENERY. A cloud deck's shadow, and the light it throws back down, are part of
        // what the material looks like — the engine has already shipped a defect where that shadow reached
        // one lit path and not the other two — so a dome with nothing under it would hide half of what is
        // being edited.
        //
        // @p material is the handle the layer resolves its values from every frame (the document's working
        // copy, so an edit shows without an Apply).
        void SetVolumeMaterial( const Assets::AssetHandle& material );

        void Clear();

        // What is filling the pane right now. The panel reads it to label its own controls (a Shape combo
        // means nothing for a dome) and a test reads it to pin the routing.
        [[nodiscard]] Fill GetFill() const
        {
            return m_Fill;
        }

        // The preview world's settings, for a panel to draw controls over. Mutable on purpose: the widget
        // owns the entities and applies whatever it finds here on the next Update(), so there is one copy
        // of each value and no push call to forget.
        [[nodiscard]] SceneSetup&       Setup();
        [[nodiscard]] const SceneSetup& Setup() const;

        /**
         * @brief IS THERE ANYTHING TO SHOW — not "was anything assigned".
         *
         * The two are different for exactly one kind of content, and it is the kind that matters. SetMesh
         * raises m_HasContent from a HANDLE, before anything has looked to see whether that handle names
         * geometry; the material and primitive paths hand over something drawable in the same breath, so
         * for them the flag alone is the whole answer.
         *
         * A mesh therefore has to be asked about, and the question is asked of the RUNTIME mesh — the object
         * the renderer would draw — not of the asset behind it. Out of line because it needs the mesh
         * service; on a hit that is one map lookup, the same one TryFrameMesh already makes every frame
         * until it succeeds, and asking it every frame is what lets a mesh the service produces LATE turn
         * this true the frame it lands instead of being written off at selection time.
         *
         * NOT m_Framed, WHICH LOOKS LIKE THE SAME QUESTION AND IS NOT. Framing succeeds off the ASSET's
         * vertex array when it can, so a mesh whose CPU data loaded and whose runtime submeshes did not is
         * "framed" — the camera knows exactly how big the nothing it is pointing at is. Measured on
         * `Cooked/Meshes/base.stmesh` in this tree: framed at half-extent 0.6 x 0.9 x 0.2, drawn as an
         * empty pane. Reaching for m_Framed here would have reproduced the very defect this fixes, one
         * level down, which is why the flag is named in this comment rather than used.
         *
         * WHAT IT WAS COSTING. Every caller reads this to choose between the live preview and the cached
         * thumbnail, and "something was assigned" always won: the Details 3D Model row would show an empty
         * lit pane for a mesh with no geometry in preference to a perfectly good picture of it on disk. The
         * fallback existed, was correct, and was unreachable for the one case it was written for.
         */
        [[nodiscard]] bool HasContent() const;

        // Is this preview's sky being rebuilt right now?
        //
        // WHAT IT IS FOR. About half of a cloud material's parameters — Coverage, the seed, the placement
        // four, the weather tile, the painted layout, the cloud types — are inputs to a BAKE of the
        // modelling volume rather than to the march, so moving one of them costs seconds on a worker while
        // this pane goes on showing the PREVIOUS volume. Draw() paints its own badge from this, which is
        // why the widget needs no cooperation from the panel; the accessor is public because a panel may
        // reasonably want to say the same thing in its own status line, and because a test can assert the
        // route exists without an editor.
        //
        // The five that answer within the frame and cost nothing — Extinction Scale, Phase G, Detail
        // Strength, Scattering Albedo, Density Scale — never make this true, which is the whole point of
        // it: an artist who sees no badge has just moved a cheap knob and the picture is already right.
        [[nodiscard]] bool IsSkyRebuilding() const;

        // Records this frame's offscreen render at the requested size. Call ONCE per frame from
        // OnPreUpdate(), and only while the preview is actually visible — a collapsed section or a
        // scrolled-away row should skip it so an inspector full of assets doesn't render them all.
        void Update( uint32_t width, uint32_t height );

        // Draws the last rendered image and handles interaction as @p mode says (Editor/Widgets/PreviewInput.hpp):
        // Interactive — LMB-drag orbits, RMB-drag pans, wheel zooms, double-click re-frames; Static — the
        // camera never moves and double-click reports Open, which the CALLER turns into opening the asset,
        // because only the caller knows which asset the picture stands for. The result says what happened
        // this frame (Interacting while the user is manipulating it).
        PreviewInputResult Draw( UI::UIHelper& uiHelper, const ImVec2& size, PreviewInteraction mode );

        // Re-frame on the current content's bounds (what an Interactive double-click does).
        void ResetView();

        // Point the orbit somewhere specific (radians; pitch clamped to the same limit the mouse has).
        // Exists for unattended evidence: macOS refuses synthetic input (StartupOptions.hpp measured
        // it), so "the same preview from the other side" must be a flag, and the flag needs a setter.
        void SetOrbit( float yawRadians, float pitchRadians );

        // Drop the pipelines THIS preview cached from @p shader, so the next frame rebuilds them against
        // the shader's new modules.
        //
        // Needed because a recompile is only half-published: the Shader object is shared and reloads
        // itself, but pipelines are cached PER SceneRenderer, and AssetHotReload::PollShaders invalidates
        // only the cache of the scene handed to Tick — the main one. A preview that never heard about the
        // rebuild would go on drawing the old modules while the viewport drew the new ones, which is a
        // preview disagreeing with the game: the precise failure Docs/MaterialEditor/STAGE1_END_TO_END.md
        // was written about, in different clothes. Safe to call with a shader this preview never used.
        void InvalidatePipelines( const void* shader );

    private:
        // @p extent is the widget's size when the caller knows it (Render); the setters that build the preview
        // before its first frame do not, and the first Render resizes it.
        void EnsureInit( const Graphic::ViewExtent& extent = Graphic::kUnsizedViewExtent );
        void ApplyCamera( uint32_t width, uint32_t height );
        // The orbit distance at which the current content fits a pane of @p aspect (width / height) whole.
        float FittedDistance( float aspect ) const;
        // Write m_Setup onto the scene's entities. Called from Update(), every frame: the writes are a
        // handful of component fields, and doing them unconditionally is what removes the "the panel
        // edited the struct but forgot to push it" failure entirely.
        //
        // The ONE thing it does conditionally is ask for an environment re-bake, and only when the setup
        // actually changed and no drag is in progress: the sky itself follows the sun on the same frame
        // (the sky pass evaluates it), but the IBL cubes behind the ambient have to be re-baked, and that
        // is not something to run sixty times a second while somebody is swinging the sun around.
        void ApplySetup();
        // Bounds of the current mesh handle, if the MeshService already has it. False while it is still
        // loading — Update() keeps retrying so a mesh that arrives a few frames later still gets framed
        // instead of being previewed against a guessed radius.
        bool TryFrameMesh();

        std::unique_ptr<Graphic::SceneRenderer> m_Renderer;
        // The cubemap domain's draw (see SetCubemapMaterial). Created on first use, source-cleared by
        // every other Set*/Clear so exactly one kind of content fills the pane at a time.
        std::unique_ptr<Render::EditorCubemapPreviewPass> m_CubemapPass;
        // The authoring grid, installed on FIRST USE and never before. It is one blended fullscreen quad
        // per frame and genuinely cheap to draw, but installing it builds a pipeline and a material — and
        // the row that switches it on is off by default, so most preview windows would be paying for a
        // pass they never show. SceneSettings::ShowGrid gates the draw once it exists.
        std::unique_ptr<Render::EditorGridPass> m_GridPass;
        // Fully qualified: a Desert::Editor::Core namespace also exists, so an unqualified Core::Scene
        // would resolve there in TUs that see it.
        std::shared_ptr<::Desert::Core::Scene> m_Scene;
        // Our own camera, driven by the orbit state (not the scene's input-driven EditorCamera).
        std::shared_ptr<::Desert::Core::GameplayCamera> m_Camera;
        ECS::Entity                                     m_Target;
        // The three entities the SceneSetup drives. Created once with the scene and then only written to
        // — a floor that is switched off is an entity with no mesh in its slot, not an entity destroyed
        // and rebuilt, because rebuilding it every toggle would churn the mesh service for a checkbox.
        ECS::Entity m_Light;
        ECS::Entity m_Floor;
        ECS::Entity m_Sky;
        // The volumetric layer, present only once a Volume-domain material has asked for it: it carries a
        // modelling volume of 8 MiB and a sky-occlusion volume of 2 MiB per renderer, which no material
        // preview should pay for by existing.
        ECS::Entity m_CloudLayer;

        Fill                m_Fill       = Fill::Empty;
        bool                m_Inited     = false;
        bool                m_HasContent = false;
        Assets::AssetHandle m_MeshHandle{ static_cast<uint64_t>( 0 ) }; // non-zero while previewing a mesh
        bool                m_Framed = false;                           // bounds resolved -> view is correct

        // Orbit state, persisted per widget instance so a preview keeps its angle across frames (and, since
        // the panel owns the widget, across selections of the same kind).
        float     m_Yaw      = -0.6f; // radians
        float     m_Pitch    = 0.5f;
        float     m_Zoom     = 1.0f; // the wheel's multiple of the fitted distance; 1 = the subject exactly fits
        glm::vec3 m_Focus{ 0.0f };
        float     m_FrameRadius = 1.0f;                  // bounding radius of the current content
        glm::vec3 m_FrameHalfExtent{ 0.5f, 0.5f, 0.5f }; // half-size of its box, for the exact fit
        // Round content (the sphere primitive) is bounded by its own radius from every angle, so it fits
        // tighter than its box would. Everything else — a cube, a card, a measured mesh — is fitted by the
        // corners of that box. Using one rule for both leaves the other one small in frame.
        bool m_FrameIsRound = false;

        // NOTE: the preview renders every frame ON PURPOSE. Skipping frames when nothing changed was tried
        // and reverted: the target does not survive as a still image between frames, so the preview simply
        // went blank. It also was not worth it — the second scene render measured ~6% of the frame, while
        // the editor's real cost at the time was the Logs panel rebuilding its row list per frame.
        // Anything reviving this must first make the last rendered image persist across skipped frames.
        //
        // What the revert left behind was a RequestRender() with an empty body and four callers marking the
        // moments the cached image went stale — a dirty flag for a cache that no longer exists. Removed:
        // five deletions, no behaviour change, because there was no behaviour. The knowledge above is worth
        // keeping; a function that performs it is not.

        // The preview world. Public through Setup(); see SceneSetup for why it lives here.
        SceneSetup m_Setup;
        // What was last written onto the entities, so ApplySetup can tell "nothing moved" from "the sun
        // moved" without the panel having to say so. Not a dirty FLAG: a flag has to be raised by every
        // writer, and the writers are a tab full of widgets plus two drag handlers.
        SceneSetup m_AppliedSetup;
        bool       m_AppliedSetupValid = false;
        // True while a light drag is in flight — the one thing that defers the environment re-bake.
        bool m_DraggingLight = false;

        uint32_t m_Width = 0, m_Height = 0;
    };
} // namespace Desert::Editor
