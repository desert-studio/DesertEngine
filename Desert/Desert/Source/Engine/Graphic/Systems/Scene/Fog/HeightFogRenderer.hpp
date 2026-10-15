#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/ECS/ExponentialHeightFogComponent.hpp>
#include <Engine/Graphic/Fog/FogPayload.hpp>
#include <Engine/Graphic/Systems/Scene/Fog/VolumetricFogRenderer.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Graphic::System
{
    /**
     * @brief The atmospheric-fog pass: the sky's AERIAL PERSPECTIVE on opaque geometry, with UE's
     *        Exponential Height Fog composed over it. One closed-form evaluation and one froxel fetch
     *        per pixel, then a single over-composite.
     *
     * The class keeps the fog's name because the fog is what it evaluates; the aerial perspective it
     * also carries was marched elsewhere (SkyboxRenderer's 32x32x16 volume) and is only SAMPLED here.
     * That is UE's own arrangement — its height-fog pixel shader is where the CameraAP volume lands on
     * opaque (research doc section 1.6) — and it is why the two are one pass and not two: they share a
     * depth read, a full-screen dispatch and a composite, and only their order matters.
     *
     * Two stages:
     *
     *   S1  EVALUATE  compute, RGBA16F at the target's own size. Reconstructs each pixel's world
     *                 position from the scene depth (presented by ComputeImageBeginRead — the ONE path
     *                 that works in Forward and Deferred, teamlead decision Q5), evaluates the
     *                 closed-form fog integral of Common/HeightFog.glslh, samples the aerial-perspective
     *                 volume at that pixel's distance, and composes `Fog over AP`. Premultiplied
     *                 inscattering in .rgb, transmittance in .a.
     *   S2  APPLY     a fullscreen triangle (ApplyPass) over the OPAQUE scene: added by SceneRenderer right
     *                 after S1, before the scene copy and every translucent draw, so the refraction reads the
     *                 fogged scene and nothing translucent is fogged by the opaque depth behind it.
     *
     * TRANSLUCENCY (UE's arrangement): glass, water, translucent meshes and particles fog THEMSELVES at their
     * own depth, from the same evaluation (Common/ViewFog.glslh) and the same parameter block, which S1
     * publishes every frame as FrameTransients::FogParams (gates 0 when there is no fog) beside the images it
     * reads (FogAerialPerspective, FogDistantSkyLight, FogVolume; SceneRenderer imports them).
     *
     * WHERE IT RUNS. S1 is an in-frame compute dispatch, after the OPAQUE scene depth is final
     * (SceneRenderer::AddFrameAtmosphericFog, after the sky's AP volume and the volumetric fog's froxels
     * were filled this frame, so the froxels a pixel reads were marched for the camera it was drawn with).
     *
     * ZERO COST WHEN NEITHER IS PRESENT. A scene with no fog component (or the fog disabled) AND no
     * aerial perspective — every SkyModel::ArtisticGradient scene — dispatches nothing, allocates no
     * fog image, and the apply pass draws nothing: the frame is what it was before this system existed
     * (the only cost is the gated-off parameter block the translucent cells declare). Either half alone is
     * enough to run the pass, and the absent half composes as the exact arithmetic identity, so a gradient
     * scene's pixels are unchanged bit for bit. The RGBA16F fog image is a transient of the frame graph
     * (Builder::CreateTexture, sized from this frame's view), created only on a fogged frame and published as
     * FrameTransients::HeightFog for the apply.
     */
    class HeightFogRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;
        ~HeightFogRenderer() override;

        Common::BoolResultStr Initialize() override;

        // The S2 apply as one raster pass on the scene target: its body, pipeline and declared fog read. Not
        // registered with the builder: its place in the frame is the call that adds it
        // (SceneRenderer::AddFrameTranslucency). No target framebuffer when the system failed to initialize.
        SystemRasterPass ApplyPass();

        /**
         * @brief This frame's fog settings, from ECS::HeightFogECSSystem.
         *
         * @param present    false when the scene has no fog component at all. Said explicitly rather
         *                   than by omission — the renderer keeps its state across frames, and a
         *                   component deleted mid-session must take its fog with it.
         * @param fogHeightY the fog entity's TransformComponent Y (world units): the fog floor, owned
         *                   by the transform exactly as UE owns it by the component transform.
         */
        void SetFogSettings( bool present, const ECS::ExponentialHeightFogData& data, float fogHeightY );

        /**
         * @brief Stage S1. Must be called outside any render pass, after the scene depth is final and
         *        after this frame's aerial-perspective volume has been filled. Creates this frame's fog
         *        image in @p graph and stores its ref in @p transients.HeightFog (left invalid when the
         *        pass does not run this frame - the apply then draws nothing). @p volume is this frame's
         *        volumetric fog (VolumetricFogRenderer::GetFrameResult, after its nodes were added): composed in
         *        front of the analytic fog when its volume is set.
         */
        std::vector<ComputeNodeDeclaration> DeclareFrameNodes( RDG::Builder& graph, FrameTransients& transients,
                                                               const VolumetricFogFrameResult& volume );

    private:
        bool CreatePipelines();

        std::shared_ptr<ComputePipeline>  m_FogPipeline;
        std::shared_ptr<GraphicsPipeline> m_ApplyPipeline;
        // The two nodes' binding layouts, re-derived only when the pipeline's shader is another object or
        // reloaded.
        mutable ShaderBindingLayoutCache m_FogLayout;
        mutable ShaderBindingLayoutCache m_ApplyLayout;

        ECS::ExponentialHeightFogData m_Data{};
        float                         m_FogHeightY = 0.0f;
        bool                          m_Present    = false;
    };
} // namespace Desert::Graphic::System
