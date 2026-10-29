#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
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
#include <string>
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

    // The LightScattering shader's parameters (Programs/Fog/VolumetricFogLightScattering.shader): the one spelling
    // the froxel lighting node binds and Initialize checks. The parameter block (VolumetricFogParamsBuffer) is the
    // pipeline's own (SetStorageBuffer), not a graph binding. Two permutations, picked by Graphic::LocalShadowsVirtual:
    // kAll is the atlas one's (the local lights read LocalShadowAtlasTexture / LocalShadowViewsUB through
    // PointClampSampler), kVirtualLocal the DESERT_LOCAL_SHADOW_VSM one's (they read the view's virtual shadow map,
    // the kVsm*Block tables of FrameGraphRefs.hpp, in place of all three).
    struct VolumetricFogLightScatteringShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, LightScatteringTexture );
        DESERT_SHADER_PARAMETER( Texture, LightScatteringHistoryTexture );
        DESERT_SHADER_PARAMETER( Texture, ShadowCascadesTexture );
        DESERT_SHADER_PARAMETER( Texture, LocalShadowAtlasTexture );
        DESERT_SHADER_PARAMETER( Texture, DistantSkyLightTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( StorageBuffer, PointLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, SpotLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LocalShadowViewsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridCellsUB );
        static constexpr std::array kAll{
             RDG::Erase( LightScatteringTexture ), RDG::Erase( LightScatteringHistoryTexture ),
             RDG::Erase( ShadowCascadesTexture ),  RDG::Erase( LocalShadowAtlasTexture ),
             RDG::Erase( DistantSkyLightTexture ), RDG::Erase( LinearClampSampler ),
             RDG::Erase( LinearRepeatSampler ),    RDG::Erase( PointClampSampler ),
             RDG::Erase( PointLightsUB ),          RDG::Erase( SpotLightsUB ),
             RDG::Erase( LocalShadowViewsUB ),     RDG::Erase( LightGridParamsUB ),
             RDG::Erase( LightGridCellsUB ) };
        static constexpr std::array kVirtualLocal{
             RDG::Erase( LightScatteringTexture ), RDG::Erase( LightScatteringHistoryTexture ),
             RDG::Erase( ShadowCascadesTexture ),  RDG::Erase( DistantSkyLightTexture ),
             RDG::Erase( LinearClampSampler ),     RDG::Erase( LinearRepeatSampler ),
             RDG::Erase( PointLightsUB ),          RDG::Erase( SpotLightsUB ),
             RDG::Erase( LightGridParamsUB ),      RDG::Erase( LightGridCellsUB ),
             RDG::Erase( kVsmFrameDataBlock ),     RDG::Erase( kVsmPageTableBlock ),
             RDG::Erase( kVsmPhysicalPool ),       RDG::Erase( kVsmLocalLightsBlock ) };
    };

    // The Integration shader's parameters (Programs/Fog/VolumetricFogIntegration.shader).
    struct VolumetricFogIntegrationShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, IntegratedLightScatteringTexture );
        DESERT_SHADER_PARAMETER( Texture, LightScatteringTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        static constexpr std::array kAll{ RDG::Erase( IntegratedLightScatteringTexture ),
                                          RDG::Erase( LightScatteringTexture ), RDG::Erase( PointClampSampler ) };
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
     *      view's light grid through the local shadow atlas - or, under Shadows.Method VirtualMaps, through their
     *      virtual shadow maps (the DESERT_LOCAL_SHADOW_VSM permutation; UE's froxels sample a local light's
     *      VSM) -, and by the sky; blended with last frame's froxel at
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

        // The LightScattering program's DESERT_LOCAL_SHADOW_VSM permutation, ShippedByCook: the one statement
        // EnsureScatteringVsmPipeline acquires and the package cook lists (PackageCook CookShaderVariants).
        [[nodiscard]] static Core::ShaderVariant ScatteringVsmVariant();
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants();

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
         * @param virtualLocalShadows the view's local lights shadow through their virtual maps (SceneRenderer::
         *        LocalShadowsAreVirtual - Graphic::LocalShadowsVirtual, the rule the opaque composite reads): the
         *        DESERT_LOCAL_SHADOW_VSM permutation, reading refs' Transients.Vsm; false = the local shadow atlas.
         */
        std::vector<ComputeNodeDeclaration> DeclareFrameNodes( const FrameGraphRefs& refs, const ViewFrame& frame,
                                                               const VolumetricFogSunInput& sun,
                                                               bool virtualLocalShadows );

        // The integrated volume of THIS frame for the height fog pass (Integrated null when no node was declared).
        [[nodiscard]] const VolumetricFogFrameResult& GetFrameResult() const
        {
            return m_Result;
        }

    private:
        bool CreatePipelines();
        bool EnsureVolumes( const glm::uvec3& gridSize );
        // The VSM permutation's pipeline, acquired the first frame a view of this renderer runs VirtualMaps (a
        // machine that never does compiles nothing); a refusal is kept and named once - the fog then stays analytic
        // under VirtualMaps rather than reading an atlas nobody drew.
        [[nodiscard]] Common::BoolResultStr EnsureScatteringVsmPipeline();

        std::shared_ptr<ComputePipeline> m_ScatteringPipeline;
        std::shared_ptr<ComputePipeline> m_ScatteringVsmPipeline;
        mutable ShaderBindingLayoutCache m_ScatteringVsmLayout;
        std::string                      m_ScatteringVsmRefusal;
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
