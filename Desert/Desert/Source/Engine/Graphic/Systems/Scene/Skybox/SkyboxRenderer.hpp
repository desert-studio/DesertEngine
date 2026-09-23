#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/AtmosphereEnv.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>
#include <Engine/Graphic/Materials/Skybox/MaterialSkybox.hpp>
#include <Engine/Graphic/Materials/Skybox/MaterialProceduralSky.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/SkyRules.hpp>
#include <Engine/Graphic/SkySettings.hpp>
#include <Engine/Graphic/SunLightFx.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::System
{
    class SkyboxRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        void PrepareCamera( Core::Camera* camera );
        void PrepareMaterial( const std::shared_ptr<MaterialSkybox>& material, const SkyLook& look );

        // When enabled, the Sky pass renders the engine-generated atmosphere instead of the HDR cubemap.
        // @p sunDir is the direction TOWARD the sun, normalized. @p bakeNow is the editor's one-shot
        // request. Everything else the sky needs — palette, sun radiance, planet radius, bake knobs — is
        // in @p sky, which MakeSkySettings produced from the component.
        // @p fx is the chosen sun light's evaluated slice: its "Affected By Atmosphere Transmittance" decides
        // whether this frame's AtmosphereEnv publishes a ground transmittance or the identity — both
        // inside the same evaluation everything else reads.
        void SetProceduralSky( bool enabled, const glm::vec3& sunDir, bool bakeNow, const SkySettings& sky,
                               const SunLightFx& fx );

        // Bakes / rebakes the procedural-sky IBL when the rule says so (see ShouldRebakeSkyEnvironment
        // for WHETHER, SkyEnvironmentRebakeMayRun for WHEN). Call once per frame from a
        // frame-boundary-safe point (BEFORE the render graph records), NOT from inside a pass — the bake
        // idles the device. @p deltaSeconds drives the debounce that keeps a drag from baking on every
        // frame it crosses the angular threshold.
        void EnsureProceduralEnvironment( float deltaSeconds );

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
        void ExecuteAtmosphereLuts();

        // Make this scene's HDR cubemap describe the look its SkyboxComponent asks for, rebaking it if
        // it does not. Called from the same pre-graph slot as EnsureProceduralEnvironment and for the
        // same reason: the bake idles the device, which is safe only outside the recorded graph.
        //
        // SETTLE-GATED by the SAME rule the sun uses (SkyRules::SkyEnvironmentRebakeMayRun), so dragging
        // the rotation slider collapses into one bake when the drag ends rather than one per frame.
        //
        // THE MATERIAL IS SHARED BETWEEN VIEWS and the look is per scene, so two scenes pointing at one
        // `.hdr` with different rotations will rebake alternately. That is visible rather than silent:
        // every bake prints its cost with the look it baked for, so a thrash reads as a thrash in the log.
        void EnsureHdrEnvironment( float deltaSeconds );

        const std::optional<Environment> GetEnvironment() const
        {
            // While the procedural sky is active, the baked atmosphere IBL drives ambient/reflections.
            if ( m_UseProceduralSky && m_ProceduralEnv )
                return m_ProceduralEnv;

            if ( const auto& material = m_MaterialSkybox.lock() )
            {
                return material->GetEnvironment();
            }
            return std::nullopt;
        }

        // The evaluated per-frame sky, for renderers that must agree with it (the atmospheric fog).
        // Valid == false when no enabled atmosphere is driving this frame.
        const AtmosphereEnv& GetAtmosphere() const
        {
            return m_Atmosphere;
        }

        void RegisterPasses( RenderGraphBuilder& builder ) override;

    private:
        void Render();

        // Writes the packed parameter block into the SSBO. One buffer serves the graphics pass and the
        // bake's compute dispatch, so both are guaranteed to describe the same sky.
        void UploadSkyParams();

        // Everything the two LUT passes read. When any of it moves, both LUTs are re-dispatched; while
        // it holds still they are not touched at all — the fingerprint arrangement the sky
        // weather map uses, for the same reason. Deliberately NOT the whole SkySettings: the palette,
        // the sun and the bake knobs change constantly and none of them is a LUT input. MieAnisotropy
        // is also absent — it rides in the payload for the Phase 2 integrator, but no LUT texel depends
        // on it, so a g-drag must not re-march 1024 texels for nothing. The three aerial-perspective
        // knobs are absent for a stronger reason still: the volume they steer is refilled EVERY frame
        // (it follows the camera), so caching them would be caching nothing.
        struct AtmosphereLutFingerprint
        {
            glm::vec3 RayleighScattering{ 0.0f };
            float     RayleighExpDistributionKm = 0.0f;
            glm::vec3 MieScattering{ 0.0f };
            glm::vec3 MieAbsorption{ 0.0f };
            float     MieExpDistributionKm = 0.0f;
            glm::vec3 OzoneAbsorption{ 0.0f };
            float     OzoneTipAltitudeKm = 0.0f;
            float     OzoneTipValue      = 0.0f;
            float     OzoneTentWidthKm   = 0.0f;
            glm::vec3 GroundAlbedo{ 0.0f };
            float     AtmosphereHeightKm    = 0.0f;
            float     MultiScatteringFactor = 0.0f;
            float     PlanetRadius          = 0.0f; // world units, as the payload carries it

            bool operator==( const AtmosphereLutFingerprint& ) const = default;
        };

        static AtmosphereLutFingerprint LutFingerprintOf( const SkySettings& sky );

        // Lazily creates the two cached LUT images (latched on failure, one MiB log line on success).
        bool EnsureAtmosphereLutResources();
        // Same arrangement for the per-view Sky-View LUT image.
        bool EnsureSkyViewLutResources();
        // Same arrangement for the per-view aerial-perspective volume.
        bool EnsureAerialPerspectiveResources();
        // Same arrangement for the one-texel distant sky light.
        bool EnsureDistantLightResources();

        // The cached pair, in dependency order (the multi-scattering march samples the transmittance).
        // @p inFrame picks the recording path: true records into the current frame's command buffer
        // (ExecuteAtmosphereLuts' slot), false submits immediate dispatches — the bake path, which runs
        // OUTSIDE a frame and cannot wait for the in-frame slot that only comes later.
        void DispatchCachedAtmosphereLuts( bool inFrame );
        void DispatchSkyViewLut();
        void DispatchAerialPerspectiveLut();
        void DispatchDistantLight();

        // The bake path's guarantee: when the physical model is active, the cached LUTs hold valid
        // texels BEFORE CreateProcedural marches them. Returns false when the LUTs cannot exist.
        bool EnsureCachedLutsForBake();

    private:
        std::weak_ptr<MaterialSkybox> m_MaterialSkybox;

        Core::Camera*                     m_ActiveCamera    = nullptr;
        // The look this scene asks its HDR sky for. Held rather than applied on the spot: PrepareMaterial
        // can run before the camera exists, and the rebake it may imply idles the device, so the work
        // belongs in the pre-graph slot (EnsureHdrEnvironment) and not in a command's Execute.
        SkyLook m_SkyboxLook{};
        // The look seen LAST FRAME, and how long it has held still — the settle half of the rebake gate.
        SkyLook m_LastSeenLook{};
        float   m_SecondsSinceLookChanged = 0.0f;
        // How long the cubes have been known stale. The deferral half: a look that never stops changing
        // must still refresh, or an animated tint would freeze the sky at the value it opened on.
        float                             m_SecondsSinceHdrStale = 0.0f;
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        std::shared_ptr<Shader>           m_Shader;

        // Procedural sky (engine-generated atmosphere) — alternative to the HDR cubemap, same Sky pass.
        std::shared_ptr<GraphicsPipeline>      m_ProceduralPipeline;
        std::shared_ptr<Shader>                m_ProceduralShader;
        std::shared_ptr<MaterialProceduralSky> m_ProceduralMaterial;

        // The sky parameter block, created NON-PERSISTENT so the backend keeps one copy per
        // (frame in flight x renderer slot). A persistent buffer would be shared by every live
        // SceneRenderer by design, and the mesh preview would overwrite the viewport's sky.
        std::shared_ptr<ShaderResources::StorageBuffer> m_SkyParams;

        // Physical-atmosphere LUTs (SkyModel::PhysicalAtmosphere only; see ExecuteAtmosphereLuts).
        // Per renderer, not per frame in flight: the cached pair is rewritten only on a parameter
        // edit, and the Sky-View LUT is written and read on the same renderer's timeline (write in the
        // in-frame compute slot, read by the next frame's Sky pass — never two frames in flight
        // writing it at once). 256x64 + 32x32 + 192x104 RGBA16F is ~292 KiB — not worth cross-renderer
        // sharing complexity.
        //
        // The aerial-perspective volume sits with them for the same reasons: 32x32x16 RGBA16F is 64 KiB,
        // it is written and read on THIS renderer's timeline inside one frame, and its froxels are the
        // camera's own frustum, so sharing it across renderers would be sharing a camera.
        std::shared_ptr<ComputePipeline> m_TransmittanceLutPipeline;
        std::shared_ptr<ComputePipeline> m_MultiScatterLutPipeline;
        std::shared_ptr<ComputePipeline> m_SkyViewLutPipeline;
        std::shared_ptr<ComputePipeline> m_AerialPerspectivePipeline;
        // The distant sky light — ONE RGBA32F texel, refilled every frame because it follows the sun.
        // It sits with the per-view resources rather than with the cached pair even though its value
        // depends on nothing about this view: an image is written by a dispatch on this renderer's
        // timeline, and sharing one texel across renderers would buy nothing and cost a synchronisation
        // rule.
        std::shared_ptr<ComputePipeline> m_DistantLightPipeline;
        std::shared_ptr<Image2D>         m_TransmittanceLut;
        std::shared_ptr<Image2D>         m_MultiScatterLut;
        std::shared_ptr<Image2D>         m_SkyViewLut;
        std::shared_ptr<Image3D>         m_AerialPerspectiveLut;
        std::shared_ptr<Image2D>         m_DistantLight;
        AtmosphereLutFingerprint         m_LutBaked;
        bool                             m_LutsValid                        = false;
        bool                             m_LutResourcesFailed               = false;
        bool                             m_SkyViewResourcesFailed           = false;
        bool                             m_AerialPerspectiveResourcesFailed = false;
        bool                             m_DistantLightResourcesFailed      = false;

        bool          m_UseProceduralSky = false;
        glm::vec3     m_SunDir           = glm::vec3( 0.0f, 1.0f, 0.0f ); // TOWARD the sun, normalized
        SkySettings   m_Sky;
        AtmosphereEnv m_Atmosphere;

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
        // PERSISTENT IS STILL WRONG for them, for the ordinary reason: several SceneRenderers are live at
        // once (Docs/RENDERER_FRAME_STATE.md), and a shared block would let a thumbnail's sky bake with a
        // preview's clouds.
        std::shared_ptr<ShaderResources::StorageBuffer> m_CloudBakeParams;
        std::shared_ptr<ShaderResources::StorageBuffer> m_CloudBakeAuthored;
        /// The authored medium's own parameter block, here for exactly the two reasons above and no new
        /// one. Bound only when the medium carries values, because only then does the program compiled
        /// from it declare the block.
        std::shared_ptr<ShaderResources::StorageBuffer> m_CloudBakeMediumParams;

        // Baked sky IBL (radiance/irradiance/prefiltered cubes) generated from the procedural atmosphere.
        Environment m_ProceduralEnv;
        glm::vec3   m_BakedSunDir = glm::vec3( 0.0f, 1.0f, 0.0f );
        bool        m_BakeRequested = false;

        // What the environment on the device was baked from, on the CLOUD side —
        // Graphic::CloudEnvironmentFingerprint of the field the panorama saw. 0 means "no clouds", which
        // is also the value of a scene that has never had any, so the first bake of a cloudless sky is not
        // charged for one. See ShouldRebakeSkyEnvironment: this is one of the two keys the sun is not.
        uint64_t m_BakedCloudFingerprint = 0;

        // ...and the other one: what it was baked from on the SKY side, Graphic::SkyBakeFingerprint of the
        // packed block plus the panorama's extent. Stamped only by a bake that SUCCEEDED, exactly like
        // m_BakedSunDir above — a failed bake that stamped its inputs would look up to date forever.
        uint64_t m_BakedSkyFingerprint = 0;

        // Debounce state (see SkyEnvironmentRebakeMayRun). m_SecondsSinceSunMoved is how long the sun has
        // held still, m_SecondsSinceStale how long a rebake has been wanted; the first collapses a drag
        // into one bake, the second stops a sun that never stops from deferring it forever.
        glm::vec3 m_LastSeenSunDir       = glm::vec3( 0.0f, 1.0f, 0.0f );
        float     m_SecondsSinceSunMoved = 0.0f;
        float     m_SecondsSinceStale    = 0.0f;

        // The High-resolution memory report is emitted once per renderer, not once per bake: with the
        // time-of-day driver a bake can happen every few seconds, and a cost that is announced every time
        // stops being read. Same reasoning for the below-horizon warning, which is a per-frame condition.
        bool m_HighResCostLogged  = false;
        bool m_BelowHorizonLogged = false;
    };
} // namespace Desert::Graphic::System
