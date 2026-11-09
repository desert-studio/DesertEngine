#pragma once

#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/AtmosphereEnv.hpp>
#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>
#include <Engine/Graphic/Environment/OwnedEnvironment.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp>
#include <Engine/Graphic/Materials/Skybox/MaterialProceduralSky.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/PostProcessing/AutoExposureRules.hpp>
#include <Engine/Graphic/SkyPayload.hpp>
#include <Engine/Graphic/SkyRules.hpp>
#include <Engine/Graphic/SkySettings.hpp>
#include <Engine/Graphic/SunLightFx.hpp>
#include <Engine/Graphic/Systems/Scene/Skybox/SceneAtmosphere.hpp>
#include <Engine/Graphic/Systems/Scene/Skybox/SceneSkyLight.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/glm.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace Desert::Graphic::System
{
    // The Procedural shader's parameters: the one spelling the pass binds and Initialize checks.
    struct SkyboxProceduralShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, TransmittanceLutTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Texture, SkyViewLutTexture );
        // Atmosphere light 1's (the moon's) Sky-View LUT; System.Black while the scene has none.
        DESERT_SHADER_PARAMETER( Texture, SkyViewLut1Texture );
        static constexpr std::array kAll{ RDG::Erase( TransmittanceLutTexture ), RDG::Erase( LinearClampSampler ),
                                          RDG::Erase( SkyViewLutTexture ), RDG::Erase( SkyViewLut1Texture ) };
    };

    class SkyboxRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        void PrepareCamera( Core::Camera* camera );
        void PrepareMaterial( const std::shared_ptr<MaterialSkybox>& material, const SkyLook& look );
        // Whether the Sky pass draws the sky BEHIND the geometry. The environment keeps lighting the scene
        // either way (GetEnvironment does not read this): hiding the backdrop is a view flag, the way UE's
        // preview scene hides its environment sphere and keeps its image-based light.
        void SetBackdropVisible( bool visible )
        {
            m_BackdropVisible = visible;
        }

        // When enabled, the Sky pass renders the engine-generated atmosphere instead of the HDR cubemap.
        // @p sunDir is the direction TOWARD the sun, normalized. @p bakeNow is the editor's one-shot
        // request. Everything else the sky needs — palette, sun radiance, planet radius, bake knobs — is
        // in @p sky, which MakeSkySettings produced from the component.
        // @p fx is the chosen sun light's evaluated slice: its "Affected By Atmosphere Transmittance" decides
        // whether this frame's AtmosphereEnv publishes a ground transmittance or the identity — both
        // inside the same evaluation everything else reads.
        void SetProceduralSky( bool enabled, const glm::vec3& sunDir, bool bakeNow, const SkySettings& sky,
                               const SunLightFx& fx, const AtmosphereLight1& light1 );

        // The physical sky's on-screen luminance ceiling for this view's exposure (Graphic::
        // SkyLuminanceCeiling). Set from the post-process settings; reaches the GPU with the frame's block.
        void SetSkyLuminanceCeiling( float ceiling );

        // Writes this frame's packed sky block into THIS view's copy of the parameter buffer. Must run inside
        // the view's ActiveViewScope (SceneRenderer::OnUpdate calls it every frame, before the sky light and
        // the graph read it): the buffer is per (frame in flight x view), so a write outside the scope lands
        // in the frame context's copy and the sky keeps an old block. Both setters above only change state.
        void UploadFrameSkyParams();

        // The sky light of the scene this view renders (Core::Scene::GetSkyLight), handed in by
        // SceneRenderer::BeginScene. Held weakly: the renderer may outlive the scene, and GetEnvironment is asked
        // between frames too (the reflection captures compare its identity before any view opens).
        void SetSceneSkyLight( std::weak_ptr<SceneSkyLight> skyLight )
        {
            m_SceneSkyLight = std::move( skyLight );
        }
        // The atmosphere of that scene (Core::Scene::GetAtmosphere): the LUT programs and the view-independent
        // LUTs, recorded by the frame's first view, read by this one. Held weakly for the sky light's reason.
        void SetSceneAtmosphere( std::weak_ptr<SceneAtmosphere> atmosphere )
        {
            m_SceneAtmosphere = std::move( atmosphere );
        }

        // Begins a capture of the scene's sky light when the rule says so (see ShouldRebakeSkyEnvironment for
        // WHETHER, SkyEnvironmentRebakeMayRun for WHEN). Call once per frame BEFORE the render graph is built: it
        // decides and allocates, AddSkyCapturePasses records. ONE CAPTURE PER SCENE (SceneSkyLight): only the
        // first view of the frame decides and records; the others read the scene's environment. The scene's
        // FIRST capture is whole -- recorded complete in this frame's graph, so the frame lights its scene with
        // its sky -- and every later one is time-sliced over the next frames while the previous environment keeps
        // lighting the scene (SkyCapture).
        // @p deltaSeconds drives the debounce that keeps a drag from recapturing on every frame it crosses the
        // angular threshold.
        void EnsureProceduralEnvironment( float deltaSeconds );

        // GRAPH BUILD, before any lit pass and before the view imports its environment
        // (SceneRenderer::AddFrameSkyLight): the atmosphere's cached LUT nodes it marches must already be added
        // (DeclareAtmosphereCachedLutNodes). Records this frame's slice of the running capture; the frame its last
        // step is recorded, GetEnvironment answers the new cubes -- for this view and every view after it.
        void AddSkyCapturePasses( RDG::Builder& graph, FrameTextures& textures );

        /// THE VIEW LIGHTS WITH ANOTHER VIEW'S SKY LIGHT and bakes none of its own (UE: a reflection capture reads
        /// the scene's sky light; it never captures a sky of its own for it). A capture's face view is handed the
        /// scene's environment before each of its frames (ReflectionCaptureRenderer::RenderCaptureViews): six face
        /// views of one capture point used to bake six copies of one sky, each landing seconds after its submit,
        /// and the cube waited on all six. From the first call this view's EnsureProceduralEnvironment bakes
        /// nothing and GetEnvironment answers @p environment (std::nullopt: the scene has no sky light yet). The
        /// cubes stay the lending view's: they are read by this frame only, before that view can rebake.
        void BorrowEnvironment( std::optional<Environment> environment )
        {
            m_BorrowsEnvironment  = true;
            m_BorrowedEnvironment = std::move( environment );
        }

        // The physical atmosphere's LUTs (Hillaire 2020): the cached pair — transmittance 256x64 +
        // multi-scattering 32x32 — the per-view Sky-View LUT, 192x104, the per-view camera
        // aerial-perspective volume, 32x32x16, all RGBA16F, and the one-texel distant sky light. Call
        // once per
        // frame from the in-frame compute point (outside any open render pass — the slot
        // ExecuteAtmosphericFog dispatches from). Does NOTHING unless SkyModel::PhysicalAtmosphere
        // is active: gradient scenes never allocate the images and never dispatch, so the old sky pays
        // zero. When active, the cached pair re-runs only when the atmosphere parameter fingerprint
        // moves (a texel depends on the medium alone), while the Sky-View LUT is refilled EVERY frame —
        // it depends on the camera's altitude and the sun. The sky pass samples the fill of the
        // PREVIOUS frame (this slot runs after the graph recorded the Sky pass); at 192x104 of
        // slowly-varying sky that one-frame latency is invisible, and the first physical frame renders
        // black sky for exactly one frame.
        //
        // The AERIAL-PERSPECTIVE volume is refilled every frame for the same reason and has NO such
        // latency: its consumer, the atmospheric-fog pass, is dispatched later in this very frame
        // (SceneRenderer runs this slot immediately before ExecuteAtmosphericFog), so the froxels a
        // pixel reads were marched for the camera that pixel was drawn with. The DISTANT SKY LIGHT is
        // refilled here for the third time on the same grounds — same frame, same consumer, no latency.
        //
        // SPLIT IN TWO SLOTS: the nodes that depend on the sky alone -- the cached pair when stale, and the
        // distant sky light -- are DeclareAtmosphereCachedLutNodes, added at the head of the graph because the sky
        // capture marches them in the same frame (AddSkyCapturePasses). The per-view ones -- the Sky-View LUT and
        // the aerial-perspective volume -- are DeclareAtmosphereLutNodes, in the in-frame compute slot above.
        // @p arena: the frame graph's (RDG::Builder::GetFrameArena), the nodes' lists live in it (both Declare*).
        ComputeNodeList DeclareAtmosphereCachedLutNodes( Common::Memory::FrameArena& arena );
        // The cached LUT pair counts as baked only once the frame graph accepted the nodes that bake it.
        void                                SettleAtmosphereCachedLutNodes( bool accepted );
        // @p refs carries the frame's cloud shadow map (CloudShadowMapOrWhite) and @p cloudShadow what was gathered
        // about it (SceneRenderer::GetCloudShadowInput): both fills march the sun's single scattering through it.
        ComputeNodeList DeclareAtmosphereLutNodes( Common::Memory::FrameArena& arena, const FrameGraphRefs& refs,
                                                   const CloudShadowInput& cloudShadow );
        // The sky-view LUT counts as filled only once the frame graph accepted its node.
        void SettleAtmosphereLutNodes( bool accepted );
        // The sky pass samples the transmittance / sky-view LUTs this frame: the procedural backdrop is drawn and
        // an earlier frame's nodes have written both. SceneRenderer::ImportSceneViewTextures asks it before any
        // node is added (the SkyboxPass declares before the LUT nodes, so they cannot publish a transient it
        // sees) and imports the pair as FrameTransients::SkyTransmittanceLut / SkyViewLut.
        [[nodiscard]] bool SkyPassSamplesLuts() const
        {
            return m_BackdropVisible && m_UseProceduralSky && m_LutsValid && m_SkyViewLutFilled &&
                   m_TransmittanceLut && m_SkyViewLut;
        }
        [[nodiscard]] const std::shared_ptr<Image2D>& GetTransmittanceLut() const
        {
            return m_TransmittanceLut;
        }
        [[nodiscard]] const std::shared_ptr<Image2D>& GetSkyViewLut() const
        {
            return m_SkyViewLut;
        }
        // Atmosphere light 1's Sky-View LUT is sampled this frame: the pair above is, the scene has a light 1,
        // and an earlier frame's node has written its LUT. Imported as FrameTransients::SkyViewLut1.
        [[nodiscard]] bool SkyPassSamplesLight1Lut() const
        {
            return SkyPassSamplesLuts() && m_Light1.Present && m_SkyViewLut1Filled && m_SkyViewLut1;
        }
        [[nodiscard]] const std::shared_ptr<Image2D>& GetSkyViewLut1() const
        {
            return m_SkyViewLut1;
        }

        const std::optional<Environment> GetEnvironment() const
        {
            if ( m_BorrowsEnvironment )
                return m_BorrowedEnvironment;
            // While the procedural sky is active, the scene's captured sky light drives ambient/reflections.
            if ( m_UseProceduralSky )
                if ( const std::shared_ptr<SceneSkyLight> skyLight = m_SceneSkyLight.lock();
                     skyLight && skyLight->HasEnvironment() )
                    return skyLight->GetEnvironment();

            if ( const auto& material = m_MaterialSkybox.lock() )
            {
                // A SPECIFIED CUBEMAP THE CACHE DID NOT HOLD has no cubes until this frame's graph converts it
                // (AddSkyCapturePasses): until then the scene keeps lighting with the skybox it had, or with none.
                if ( !material->GetEnvironment() )
                {
                    if ( !m_PreviousMaterialSkybox )
                        return std::nullopt;
                    Environment previous = m_PreviousMaterialSkybox->GetEnvironment();
                    previous.Look        = m_PreviousSkyboxLook;
                    return previous;
                }
                // THE ONE PLACE THE LOOK JOINS THE CUBES. The material's cubes are the file as authored
                // (identity); the scene's rotation and gain ride along here, so every consumer that
                // resolves this environment — the forward materials, the deferred composite — reads the
                // same value the backdrop is drawn with, and nothing is rebaked when it changes.
                Environment environment = material->GetEnvironment();
                environment.Look        = m_SkyboxLook;
                return environment;
            }
            return std::nullopt;
        }

        /// Whether this view has a sky light (SkyLightState), by the branches GetEnvironment() takes. A
        /// procedural sky is Ready from its view's first frame, because its first capture is recorded whole in
        /// that frame's graph before anything reads it (a recapture keeps the previous environment, still Ready).
        /// An HDR skybox is Ready once it has cubes -- converted ahead of every reader in the frame graph of the
        /// frame it arrives on a cache miss -- or while the skybox it replaced still lights the scene; asked
        /// before that graph with neither, it is NoSky for that one question. A Ready sky whose cubes do not
        /// resolve is therefore a fault, which the deferred composite reports as one.
        [[nodiscard]] SkyLightState GetSkyLightState() const
        {
            if ( m_UseProceduralSky )
                return SkyLightState::Ready;
            if ( const std::shared_ptr<MaterialSkybox> material = m_MaterialSkybox.lock();
                 material && ( material->GetEnvironment() || m_PreviousMaterialSkybox ) )
                return SkyLightState::Ready;
            return SkyLightState::NoSky;
        }

        /// Whether the scene's sky light is being, or is about to be, captured again for a sun TOWARD @p sunDir:
        /// a capture in flight, or Auto Rebake on and @p sunDir past the rebake threshold from the sun the
        /// environment was captured under (ShouldRebakeSkyEnvironment's sun test, without the debounce: a sun
        /// that keeps moving is captured anyway within kSkyRebakeMaxDeferSeconds). Asked BEFORE this frame's
        /// EnsureProceduralEnvironment has seen @p sunDir, by what is lit by sun AND sky together (the reflection
        /// captures): it waits for the sky that will be captured for this sun instead of rendering the new sun
        /// under the old sky. False without a procedural sky, a lent one, no environment yet, Auto Rebake off,
        /// or a physical sky whose LUTs failed -- the sky will not follow the sun then.
        [[nodiscard]] bool SkyLightRecapturesFor( const glm::vec3& sunDir ) const
        {
            if ( !m_UseProceduralSky || m_BorrowsEnvironment )
                return false;
            const std::shared_ptr<SceneSkyLight> skyLight = m_SceneSkyLight.lock();
            if ( !skyLight || !skyLight->HasEnvironment() )
                return false;
            if ( skyLight->IsCapturing() )
                return true;
            if ( !m_Sky.AutoRebakeEnvironment )
                return false;
            if ( m_Sky.Model == ECS::SkyModel::PhysicalAtmosphere )
            {
                const std::shared_ptr<SceneAtmosphere> atmosphere = m_SceneAtmosphere.lock();
                if ( !atmosphere || !atmosphere->CanBakeLuts() )
                    return false;
            }
            return ShouldRebakeSkyEnvironment( skyLight->GetCaptured().SunDir, sunDir,
                                               m_Sky.RebakeSunAngleThreshold, true, true, false, 0, 0, 0, 0 );
        }

        // How the sky's fullscreen triangle meets the scene depth.
        enum class SkyDepth : uint8_t
        {
            // Every texel, depth test off: the forward path's first base-pass node (it CLEARS the scene target and
            // the geometry after it draws over the sky).
            Fill,
            // Only where the scene depth is the far plane (CloserOrEqual at depth 0, no write): the deferred path,
            // after the G-buffer depth reached the scene target (UE: the sky after the base pass), so a texel
            // geometry covers is never shaded by the sky.
            BehindScene,
        };
        // The sky draw on the scene target: SkyDepth::Fill (the forward base pass's first node) and
        // SkyDepth::BehindScene (the deferred sky after the G-buffer depth). Empty (no TargetFramebuffer) without
        // a target.
        [[nodiscard]] SystemRasterPass SkyPass();
        [[nodiscard]] SystemRasterPass SkyBehindScenePass();

        // The evaluated per-frame sky, for renderers that must agree with it (the atmospheric fog).
        // Valid == false when no enabled atmosphere is driving this frame.
        const AtmosphereEnv& GetAtmosphere() const
        {
            return m_Atmosphere;
        }

    private:
        // The SkyboxPass's setup: picks the draw (procedural sky or the skybox material), feeds its material
        // and declares its one binding block (block 0) - the procedural sky's LUTs are block entries. Records
        // the choice in m_SkyDraw; Render records exactly that.
        [[nodiscard]] SystemRasterPass SkyPassOf( SkyDepth depth );
        void DeclareSkyDraw( RenderPassDeclaration& declared, const FrameGraphRefs& refs, SkyDepth depth );
        [[nodiscard]] Common::BoolResultStr Render( const RDG::PassContext& context ) const;

        // Lazily creates the per-view Sky-View LUT image (latched on failure, one MiB log line on success).
        bool EnsureSkyViewLutResources();
        // Same arrangement for the per-view aerial-perspective volume.
        bool EnsureAerialPerspectiveResources();
        // The per-view fills, recorded by their graph nodes only, with the scene atmosphere's @p pipeline (it owns
        // every LUT program; the images are this view's, they follow its camera). The graph places every barrier.
        // @p cameraAndLight: xyz the camera position the fill marches from (world units), w the atmosphere light
        // it integrates in that light's own frame (0 = sun, 1 = light 1). Formed when the node is declared, so the
        // dispatch reads exactly the value its input fingerprint was taken over.
        [[nodiscard]] Common::BoolResultStr DispatchSkyViewLut( const RDG::PassContext& context,
                                                                ComputePipeline&        pipeline,
                                                                const glm::vec4&        cameraAndLight );
        [[nodiscard]] Common::BoolResultStr DispatchAerialPerspectiveLut( const RDG::PassContext& context,
                                                                          ComputePipeline&        pipeline );

        // Each LUT node's binding block 0: @p pipeline's shader layout (kept in @p layout), the sky payload
        // buffer through the pipeline route, and @p pushBytes of push constants.
        RenderPassDeclaration::BlockDeclaration DeclareLutBlock( RenderPassDeclaration&    declared,
                                                                 ComputePipeline*          pipeline,
                                                                 ShaderBindingLayoutCache& layout,
                                                                 uint32_t                  pushBytes );

    private:
        std::weak_ptr<MaterialSkybox> m_MaterialSkybox;
        // THE SKYBOX BEING REPLACED, held (its cubes alive) while the new one waits for its
        // conversion in the frame
        // graph (UE keeps the previous convolved sky until the new one is ready): GetEnvironment lights with it,
        // in the look it was drawn with, until the new material has cubes. Empty otherwise.
        std::shared_ptr<MaterialSkybox> m_PreviousMaterialSkybox;
        SkyLook                         m_PreviousSkyboxLook{};

        Core::Camera*                     m_ActiveCamera    = nullptr;
        // The look this scene asks its HDR sky for — the single source of it for this renderer's frame.
        // Applied where the cubes are sampled (GetEnvironment, Render), never baked, so a change costs a
        // uniform write and not a convolution chain.
        SkyLook                           m_SkyboxLook{};
        bool                              m_BackdropVisible = true;
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        // m_Pipeline's state for SkyDepth::BehindScene (CloserOrEqual, no depth write); built with it.
        std::shared_ptr<GraphicsPipeline> m_BehindPipeline;
        std::shared_ptr<Shader>           m_Shader;
        ShaderBindingLayoutCache          m_SkyLayout; // the Sky pass block, keyed on m_Pipeline's shader

        // Procedural sky (engine-generated atmosphere) — alternative to the HDR cubemap, same Sky pass.
        std::shared_ptr<GraphicsPipeline>      m_ProceduralPipeline;
        std::shared_ptr<GraphicsPipeline> m_ProceduralBehindPipeline; // SkyDepth::BehindScene's, built with it
        std::shared_ptr<Shader>                m_ProceduralShader;
        std::shared_ptr<MaterialProceduralSky> m_ProceduralMaterial;
        ShaderBindingLayoutCache               m_ProceduralLayout; // keyed on m_ProceduralPipeline's shader

        // The sky parameter block, created NON-PERSISTENT so the backend keeps one copy per
        // (frame in flight x renderer slot). A persistent buffer would be shared by every live
        // SceneRenderer by design, and the mesh preview would overwrite the viewport's sky.
        std::shared_ptr<ShaderResources::StorageBuffer> m_SkyParams;
        // The per-view fills' CloudShadowUniforms (kSkyCloudShadowBinding), written by DeclareAtmosphereLutNodes
        // every frame the fills are declared; non-persistent for m_SkyParams' reason (one copy per frame in flight
        // x renderer slot).
        std::shared_ptr<ShaderResources::StorageBuffer> m_SkyCloudShadow;

        // THE SCENE'S ATMOSPHERE LUTS AS THIS VIEW READS THEM THIS FRAME (SceneAtmosphere owns them and every LUT
        // program): refreshed by SettleAtmosphereCachedLutNodes, after the frame's first view recorded them.
        std::shared_ptr<Image2D> m_TransmittanceLut;
        std::shared_ptr<Image2D> m_MultiScatterLut;
        bool                     m_LutsValid         = false;
        uint64_t                 m_LutBakeGeneration = 0;
        // The per-view fills' block layouts, keyed on the scene's programs' shaders (kept, not re-derived).
        ShaderBindingLayoutCache m_SkyViewLutLayout;
        ShaderBindingLayoutCache m_AerialPerspectiveLayout;
        // The per-view LUTs, written and read on THIS renderer's timeline inside one frame: the Sky-View LUT is a
        // function of the camera's position and the sun, and the aerial-perspective volume's froxels are the
        // camera's own frustum, so sharing either across views would be sharing a camera.
        std::shared_ptr<Image2D>         m_SkyViewLut;
        // Atmosphere light 1's (the moon's) Sky-View LUT: the same fill around that light's direction.
        std::shared_ptr<Image2D>         m_SkyViewLut1;
        std::shared_ptr<Image3D>         m_AerialPerspectiveLut;
        // The sky-view LUT has been written by an earlier frame's SkyAtmosphereLuts node (refilled after that
        // whenever its inputs move, m_SkyViewLutInputs). The SkyboxPass samples the LUTs only once this
        // holds, see DeclareSkyDraw.
        bool m_SkyViewLutFilled = false;
        // DeclareAtmosphereLutNodes declared the SkyViewLut node this frame, until SettleAtmosphereLutNodes.
        bool m_SkyViewLutFillPending = false;
        // The same pair for m_SkyViewLut1, declared only while the scene has an atmosphere light 1.
        bool m_SkyViewLut1Filled      = false;
        bool m_SkyViewLut1FillPending = false;
        // SkyLutFillInputsOf per fill: what the image holds (settled) and what this frame's node writes (pending,
        // until the Settle that hears the graph). Empty = never filled, or the block's upload was refused.
        std::optional<uint64_t> m_SkyViewLutInputs;
        std::optional<uint64_t> m_SkyViewLutInputsPending;
        std::optional<uint64_t> m_SkyViewLut1Inputs;
        std::optional<uint64_t> m_SkyViewLut1InputsPending;
        // The block UploadFrameSkyParams sent this frame, hashed (SkyLutFillInputsOf); empty when it was refused,
        // which re-fills every dependant rather than trusting a block the GPU may not hold.
        std::optional<uint64_t> m_UploadedSkyBlockInputs;
        // What the SkyboxPass's setup (DeclareSkyDraw) chose this frame: the pipeline and material its block 0 was
        // declared against, or a refusal the exec returns (a skybox material without its pipeline). Empty when
        // nothing draws.
        struct SkyDraw
        {
            const GraphicsPipeline* Pipeline = nullptr;
            const MaterialExecutor* Executor = nullptr;
            const char*             Fault    = nullptr;
        };
        SkyDraw m_SkyDraw;
        bool                             m_SkyViewResourcesFailed           = false;
        bool                             m_AerialPerspectiveResourcesFailed = false;

        bool          m_UseProceduralSky = false;
        glm::vec3     m_SunDir           = glm::vec3( 0.0f, 1.0f, 0.0f ); // TOWARD the sun, normalized
        SkySettings   m_Sky;
        AtmosphereEnv m_Atmosphere;

        // The physical sky's sun as PackSky takes it: the atmosphere sun light's outer-space illuminance and
        // disc scale (this frame's SunLightFx), and the exposure-derived ceiling.
        // Atmosphere light 1 (the moon) as PackSky takes it; Present == false packs a slot every pass skips.
        AtmosphereLight1 m_Light1;

        SkySunInputs m_SunInputs{ .OuterSpaceIlluminance = glm::vec3( 0.0f ),
                                  .DiskColorScale        = glm::vec3( 1.0f ),
                                  .LuminanceCeiling      = kSkyExposedLuminanceCeiling };

        // THE CLOUD LAYER'S TWO BLOCKS, AS THE BAKE'S DISPATCH READS THEM.
        //
        // The bytes are the VolumetricCloudRenderer's — it packs them from the component, and the bake
        // marches the same block the screen march does. The BUFFERS are this renderer's, and that is not
        // duplicated state but a consequence of when the two dispatches run: the cloud renderer's own
        // parameter buffer is non-persistent, so it carries one copy per (frame x renderer slot), and it
        // is written by the passes INSIDE the frame — this bake is issued before them, from
        // SceneRenderer::OnUpdate. Writing that buffer here would put two values into one frame's copy
        // with a dispatch reading it in between.
        //
        // PERSISTENT, AND TWO SETS. A recapture renders its cube faces over several frames (one face per
        // slice) and every face reads these blocks, which are written once, when the capture begins. A
        // per-frame-in-flight buffer gives each later frame a copy nobody wrote this capture -- measured on
        // Clouds_Protocol, the faces after the first marched clouds from a stale block and the clouds' share of
        // the ground's sky light fell from 12.5 to 1.9 grey levels (SKY-CAPTURE-CLOUDS). One device buffer per
        // block holds the bytes for every slice; the two sets alternate per capture, so a capture's upload never
        // lands in a buffer the previous capture's last face, possibly still in flight, reads. Each view's
        // renderer has its own sets, so no other view's sky is written into them.
        std::array<std::shared_ptr<ShaderResources::StorageBuffer>, 2> m_CloudBakeParams;
        std::array<std::shared_ptr<ShaderResources::StorageBuffer>, 2> m_CloudBakeAuthored;
        /// The authored medium's own parameter block, here for exactly the two reasons above and no new
        /// one. Bound only when the medium carries values, because only then does the program compiled
        /// from it declare the block.
        std::array<std::shared_ptr<ShaderResources::StorageBuffer>, 2> m_CloudBakeMediumParams;
        // The set the most recent capture's upload went to.
        uint32_t m_CloudBakeSet = 0u;

        // The scene's sky light: its capture, its environment and the stamps it was captured from. The Bake
        // button's request waits here until this view's next EnsureProceduralEnvironment hands it on.
        std::weak_ptr<SceneSkyLight> m_SceneSkyLight;
        // The scene's atmosphere: its LUT programs and view-independent LUTs (SetSceneAtmosphere).
        std::weak_ptr<SceneAtmosphere> m_SceneAtmosphere;
        // BorrowEnvironment: this view lights with the environment it is handed and never captures one.
        bool                       m_BorrowsEnvironment = false;
        std::optional<Environment> m_BorrowedEnvironment;
        bool                       m_BakeRequested    = false;
        bool                       m_NoSkyLightLogged = false;

        // The High-resolution memory report is emitted once per renderer, not once per bake: with the
        // time-of-day driver a bake can happen every few seconds, and a cost that is announced every time
        // stops being read. Same reasoning for the below-horizon warning, which is a per-frame condition.
        bool m_HighResCostLogged  = false;
        bool m_BelowHorizonLogged = false;

        bool m_CaptureLutsMissingLogged = false;
    };
} // namespace Desert::Graphic::System
