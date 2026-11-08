#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RenderPassDeclaration.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/Scene/Fog/VolumetricFogRenderer.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    struct ViewFrame;

    // The fill's parameter block (Programs/Translucency/TranslucencyLightingVolume.shader
    // TranslucencyLightingVolumeParams), member for member, std430.
    struct TranslucencyLightingVolumeGpuPayload
    {
        glm::mat4                ViewProjection{ 1.0f };
        std::array<glm::mat4, 4> CascadeViewProj{};
        glm::vec4                Camera{ 0.0f };       // xyz = camera (cm), w = cells per cascade edge
        glm::vec4                RenderSize{ 0.0f };   // xy = render pixels
        glm::vec4                SunDirection{ 0.0f }; // xyz = toward the sun, w = 1 when present
        glm::vec4                SunRadiance{ 0.0f };  // rgb = irradiance, w = cascades
        glm::vec4                CascadeBias{ 0.0f };
        glm::mat4                CloudShadowWorldToMap{ 1.0f };
        glm::vec4                CloudShadowParams{ 0.0f };
        glm::mat4                RainOcclusionWorldToMap{ 1.0f };
        glm::vec4                RainOcclusionParams{ 0.0f };
    };
    static_assert( offsetof( TranslucencyLightingVolumeGpuPayload, Camera ) == 320 );
    static_assert( offsetof( TranslucencyLightingVolumeGpuPayload, CloudShadowWorldToMap ) == 400 );
    static_assert( offsetof( TranslucencyLightingVolumeGpuPayload, RainOcclusionWorldToMap ) == 480 );
    static_assert( sizeof( TranslucencyLightingVolumeGpuPayload ) == 560 );

    // Must equal the shader's `binding = 1` (SetStorageBuffer takes the number, not reflection).
    inline constexpr uint32_t kTranslucencyLightingVolumeParamsBinding = 1;

    // The machine's Effects.TranslucencyLightingVolumeDim (UE r.TranslucencyLightingVolumeDim): cells per cascade
    // edge, even (Common/TranslucencyLightingVolume.glslh centres a cascade on the camera's cell).
    struct TranslucencyLightingVolumeQuality
    {
        uint32_t Dim = 64;
    };
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    // The fill's parameters (Programs/Translucency/TranslucencyLightingVolume.shader). Two permutations, picked
    // by Graphic::LocalShadowsVirtual exactly as the volumetric fog's froxels: kAll reads the local shadow atlas,
    // kVirtualLocal (DESERT_LOCAL_SHADOW_VSM) the view's virtual shadow map tables in its place.
    struct TranslucencyLightingVolumeShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, TranslucencyLightingVolumeOutput );
        DESERT_SHADER_PARAMETER( Texture, ShadowCascadesTexture );
        DESERT_SHADER_PARAMETER( Texture, LocalShadowAtlasTexture );
        DESERT_SHADER_PARAMETER( Texture, EnvironmentIrradianceTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        DESERT_SHADER_PARAMETER( StorageBuffer, PointLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, SpotLightsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LocalShadowViewsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridParamsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, LightGridCellsUB );
        DESERT_SHADER_PARAMETER( Texture, CloudShadowMapTexture );
        DESERT_SHADER_PARAMETER( Texture, RainOcclusionMapTexture );
        static constexpr std::array kAll{ RDG::Erase( TranslucencyLightingVolumeOutput ),
                                          RDG::Erase( ShadowCascadesTexture ),
                                          RDG::Erase( LocalShadowAtlasTexture ),
                                          RDG::Erase( EnvironmentIrradianceTexture ),
                                          RDG::Erase( LinearClampSampler ),
                                          RDG::Erase( LinearRepeatSampler ),
                                          RDG::Erase( PointClampSampler ),
                                          RDG::Erase( PointLightsUB ),
                                          RDG::Erase( SpotLightsUB ),
                                          RDG::Erase( LocalShadowViewsUB ),
                                          RDG::Erase( LightGridParamsUB ),
                                          RDG::Erase( LightGridCellsUB ),
                                          RDG::Erase( CloudShadowMapTexture ),
                                          RDG::Erase( RainOcclusionMapTexture ) };
        static constexpr std::array kVirtualLocal{ RDG::Erase( TranslucencyLightingVolumeOutput ),
                                                   RDG::Erase( ShadowCascadesTexture ),
                                                   RDG::Erase( EnvironmentIrradianceTexture ),
                                                   RDG::Erase( LinearClampSampler ),
                                                   RDG::Erase( LinearRepeatSampler ),
                                                   RDG::Erase( PointLightsUB ),
                                                   RDG::Erase( SpotLightsUB ),
                                                   RDG::Erase( LightGridParamsUB ),
                                                   RDG::Erase( LightGridCellsUB ),
                                                   RDG::Erase( kVsmFrameDataBlock ),
                                                   RDG::Erase( kVsmPageTableBlock ),
                                                   RDG::Erase( kVsmPhysicalPool ),
                                                   RDG::Erase( kVsmLocalLightsBlock ),
                                                   RDG::Erase( CloudShadowMapTexture ),
                                                   RDG::Erase( RainOcclusionMapTexture ) };
    };

    /**
     * @brief UE's Translucency Lighting Volume (TranslucentLighting.cpp, reimplemented as a per-cell gather): the
     *        light a lit translucent particle receives, held in two camera-centred cascades of world cells
     *        (Common/TranslucencyLightingVolume.glslh lays them out) and filled once per frame from the sun
     *        (cascades + cloud shadow), the view's local lights (light grid + their shadows) and the sky light.
     *
     * One compute node, "Translucency: Lighting Volume", after the light grid, the shadow depths and the cloud
     * shadow map and before the translucency. PER VIEW, PER FRAME: one instance per SceneRenderer; no history.
     *
     * ZERO COST WHEN UNUSED: SceneRenderer declares the node only on a frame whose view draws a lit particle
     * (ParticleRenderer::DrawsLitSprites) - no node, and the volume is not touched.
     */
    class TranslucencyLightingVolumeRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;
        ~TranslucencyLightingVolumeRenderer() override;

        Common::BoolResultStr Initialize() override;

        // The fill's DESERT_LOCAL_SHADOW_VSM permutation, ShippedByCook (PackageCook CookShaderVariants).
        [[nodiscard]] static Core::ShaderVariant                     VsmVariant();
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants();

        // The machine's Effects.TranslucencyLightingVolumeDim (SceneRenderer::BeginScene).
        void SetQuality( const TranslucencyLightingVolumeQuality& quality );

        /**
         * @brief The fill node of this frame. Empty when an input is missing (said in the log).
         *
         * @param refs   this graph's transients (cascades, light grid, local shadows, sky light).
         * @param frame  this view's frame (camera, render extent).
         * @param sun    the sun and its cascades as the surfaces receive them (Radiance = their irradiance).
         * @param virtualLocalShadows Graphic::LocalShadowsVirtual (SceneRenderer::LocalShadowsAreVirtual).
         */
        ComputeNodeList DeclareFrameNodes( Common::Memory::FrameArena& arena, const FrameGraphRefs& refs,
                                           const ViewFrame& frame, const VolumetricFogSunInput& sun,
                                           bool virtualLocalShadows );

        // The volume this frame's node wrote (null when no node was declared) and its graph name.
        [[nodiscard]] const std::shared_ptr<Image3D>& GetFrameVolume() const
        {
            return m_FrameVolume;
        }
        static constexpr const char* kVolumeName = "Translucency.LightingVolume";

    private:
        bool                                EnsureVolume( uint32_t dim );
        [[nodiscard]] Common::BoolResultStr EnsureVsmPipeline();

        std::shared_ptr<ComputePipeline> m_Pipeline;
        std::shared_ptr<ComputePipeline> m_VsmPipeline;
        mutable ShaderBindingLayoutCache m_Layout;
        mutable ShaderBindingLayoutCache m_VsmLayout;
        std::string                      m_VsmRefusal;

        std::shared_ptr<ShaderResources::StorageBuffer> m_ParamsBuffer;
        std::shared_ptr<Image3D>                        m_Volume;
        std::shared_ptr<Image3D>                        m_FrameVolume;
        uint32_t                                        m_VolumeDim = 0;
        TranslucencyLightingVolumeQuality               m_Quality{};
    };
} // namespace Desert::Graphic::System
