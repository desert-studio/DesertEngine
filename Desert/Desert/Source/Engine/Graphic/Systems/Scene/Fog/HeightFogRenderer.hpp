#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/ECS/ExponentialHeightFogComponent.hpp>
#include <Engine/Graphic/Fog/FogPayload.hpp>
#include <Engine/Graphic/Materials/Fog/MaterialHeightFog.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

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
     *   S2  APPLY     a fullscreen quad registered in RenderPhase::Transparency at
     *                 RenderPassOrder::AtmosphericFog — below everything else the phase composites, so
     *                 every particle lands OVER the fogged scene rather than under it.
     *
     * WHERE IT RUNS. S1 is an in-frame compute dispatch and must be issued OUTSIDE an open render pass,
     * after the scene depth is finished. SceneRenderer::ExecuteAtmosphericFog() calls ExecuteInFrame()
     * between the deferred block and ExecuteTransparency() —
     * immediately AFTER SkyboxRenderer::ExecuteAtmosphereLuts filled this frame's AP volume, so the
     * froxels a pixel reads were marched for the camera that pixel was drawn with.
     *
     * ZERO COST WHEN NEITHER IS PRESENT. A scene with no fog component (or the fog disabled) AND no
     * aerial perspective — every SkyModel::ArtisticGradient scene — dispatches nothing, allocates no
     * target, and the apply pass draws nothing: the frame is what it was before this system existed.
     * Either half alone is enough to run the pass, and the absent half composes as the exact arithmetic
     * identity, so a gradient scene's pixels are unchanged bit for bit. What Initialize does build
     * regardless is the two pipelines and the 80-byte parameter buffer
     * exactly; the per-view RGBA16F target is the only real memory, and it is allocated lazily on the
     * first fogged frame with the failure latched.
     */
    class HeightFogRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;
        ~HeightFogRenderer() override;

        Common::BoolResultStr Initialize() override;
        void                  RegisterPasses( RenderGraphBuilder& builder ) override;

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
         *        after this frame's aerial-perspective volume has been filled.
         */
        std::vector<ComputeNodeDeclaration> DeclareFrameNodes();

    private:
        bool CreatePipelines();
        // Allocates (or reallocates) the fog image for @p width x @p height. Returns false having
        // logged the reason and latched the failure.
        bool EnsureResources( uint32_t width, uint32_t height );

        std::shared_ptr<ComputePipeline>  m_FogPipeline;
        std::shared_ptr<GraphicsPipeline> m_ApplyPipeline;

        std::unique_ptr<MaterialHeightFog> m_ApplyMaterial;

        std::shared_ptr<Image2D>                        m_FogImage;
        std::shared_ptr<ShaderResources::StorageBuffer> m_ParamsBuffer;

        ECS::ExponentialHeightFogData m_Data{};
        float                         m_FogHeightY = 0.0f;
        bool                          m_Present    = false;

        uint32_t m_FogWidth  = 0;
        uint32_t m_FogHeight = 0;

        bool m_ResourcesFailed = false;

        // True while the last ExecuteInFrame actually produced a fog image. The apply draws nothing
        // without it, rather than compositing a target from three frames ago.
        bool m_HasFrameResult = false;
    };
} // namespace Desert::Graphic::System
