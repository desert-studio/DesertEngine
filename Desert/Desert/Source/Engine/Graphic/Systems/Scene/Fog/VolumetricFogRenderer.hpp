#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/ECS/ExponentialHeightFogComponent.hpp>
#include <Engine/Graphic/Fog/VolumetricFogPayload.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RenderPassDeclaration.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/View/PassHistory.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Desert::Graphic
{
    struct ViewFrame;
}

namespace Desert::Graphic::System
{
    // What the frame's volumetric fog left for the height fog pass to compose (HeightFogRenderer): the integrated
    // volume and the grid it is cut by. Integrated is null when the volumetric fog did not run this frame.
    struct VolumetricFogFrameResult
    {
        std::shared_ptr<Image> Integrated;
        glm::uvec3             GridSize{ 1u };
        glm::vec4              GridZParams{ 0.0f }; // MakeVolumetricFogGridZParams
        float                  GridPixels = 1.0f;   // froxel edge, render pixels
        float                  Distance   = 0.0f;   // the grid's far end (cm): the analytic fog starts here
    };

    // The sun and its cascades as this frame's view sees them, gathered by SceneRenderer (which owns the mesh
    // renderer that fitted the cascades and the directional light list) and handed in explicitly.
    struct VolumetricFogSunInput
    {
        bool      Present = false;
        glm::vec3 TowardSun{ 0.0f, 1.0f, 0.0f };
        // The light's colour x intensity as it reaches the ground (the atmosphere's transmittance applied, the
        // same value the surfaces are lit with) x its VolumetricScatteringIntensity.
        glm::vec3                Radiance{ 0.0f };
        uint32_t                 CascadeCount = 0;
        std::array<glm::mat4, 4> CascadeViewProj{};
        glm::vec4                CascadeWorldPerTexel{ 0.0f };
        glm::vec4                CascadeDepthRange{ 1.0f };
    };

    /**
     * @brief UE's Volumetric Fog (VolumetricFog.cpp / VolumetricFog.usf, reimplemented): the exponential height
     *        fog's medium evaluated per FROXEL of a camera-aligned grid and lit by every light, so that fog shows
     *        light shafts from the sun, halos around lamps and the cones of spot lights.
     *
     * Two compute nodes per frame, both on volumes this system owns:
     *
     *   1. "VolumetricFog: LightScattering" (VolumetricFogLightScattering.shader): per froxel, the medium at a
     *      jittered point inside it, lit by the sun through its cascades, by the point and spot lights of the
     *      view's light grid through the local shadow atlas, and by the sky; blended with last frame's froxel at
     *      the same world point (temporal reprojection). Writes one of a PING-PONG pair: the other is the history.
     *   2. "VolumetricFog: Integration" (VolumetricFogIntegration.shader): front-to-back integral along every
     *      froxel column, into the volume the height fog pass samples at each pixel's depth (GetFrameResult).
     *
     * PER VIEW, PER FRAME. One instance per SceneRenderer (one per view), so the history is this view's; its
     * validity is checked every frame against the view's frame index, its history-reset reason and the grid size.
     * Settings arrive each frame (SetFogSettings, SetQuality) and nothing is cached across scenes.
     *
     * ZERO COST WHEN OFF: no fog component, fog disabled, VolumetricFog unchecked, or the machine's
     * Effects.VolumetricFog at 0 - no node, no allocation, and the height fog stays purely analytic.
     */
    class VolumetricFogRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;
        ~VolumetricFogRenderer() override;

        Common::BoolResultStr Initialize() override;

        // The light-scattering history is the only state that outlives a frame (IRenderSystem::OnSceneReplaced,
        // kind 1): a new scene must not reproject the old one's fog, nor a camera cut the shot before it. The
        // volumes themselves are content-keyed (the grid size) and the settings are pushed every frame.
        void OnSceneReplaced() override
        {
            m_History.Invalidate();
        }
        void OnTemporalHistoryReset() override
        {
            m_History.Invalidate();
        }

        // This frame's fog (the same values HeightFogRenderer gets): @p fogHeightY is the fog entity's Y (cm).
        void SetFogSettings( bool present, const ECS::ExponentialHeightFogData& data, float fogHeightY );

        // The machine's Effects.VolumetricFog* rows (SceneRenderer::BeginScene).
        void SetQuality( const VolumetricFogQuality& quality );

        // Whether the nodes will run this frame (authored on, machine allows it).
        [[nodiscard]] bool IsActive() const;

        /**
         * @brief The two compute nodes of this frame, after the light grid and the shadow depths were added and
         *        before the height fog pass. Empty when the fog is off or an input is missing (said in the log).
         *
         * @param refs  this graph's transients (cascades, local shadow atlas, light buffers) and system textures.
         * @param frame this view's frame (camera now and last frame, history reset reason).
         * @param sun   the sun and its cascades.
         */
        std::vector<ComputeNodeDeclaration> DeclareFrameNodes( const FrameGraphRefs& refs, const ViewFrame& frame,
                                                               const VolumetricFogSunInput& sun );

        // The integrated volume of THIS frame for the height fog pass (Integrated null when no node was declared).
        [[nodiscard]] const VolumetricFogFrameResult& GetFrameResult() const
        {
            return m_Result;
        }

    private:
        bool CreatePipelines();
        bool EnsureVolumes( const glm::uvec3& gridSize );

        std::shared_ptr<ComputePipeline> m_ScatteringPipeline;
        std::shared_ptr<ComputePipeline> m_IntegrationPipeline;
        mutable ShaderBindingLayoutCache m_ScatteringLayout;
        mutable ShaderBindingLayoutCache m_IntegrationLayout;

        std::shared_ptr<ShaderResources::StorageBuffer> m_ParamsBuffer;

        // The lit froxels, ping-pong: this frame writes [m_Current], [1 - m_Current] is last frame's (the
        // history).
        std::array<std::shared_ptr<Image3D>, 2> m_LightScattering{};
        std::shared_ptr<Image3D>                m_Integrated;
        glm::uvec3                              m_VolumeSize{ 0u };
        uint32_t                                m_Current = 0;

        // Which view frame wrote the history (PassHistory.hpp): readable only in the view's next frame, and
        // dropped by a scene swap, a camera cut and a new grid.
        PassHistoryStamp m_History;

        VolumetricFogFrameResult m_Result;

        ECS::ExponentialHeightFogData m_Data{};
        float                         m_FogHeightY = 0.0f;
        bool                          m_Present    = false;
        VolumetricFogQuality          m_Quality{};
    };
} // namespace Desert::Graphic::System
