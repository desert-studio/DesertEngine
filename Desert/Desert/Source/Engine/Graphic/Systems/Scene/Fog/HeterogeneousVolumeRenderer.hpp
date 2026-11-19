#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <Engine/Graphic/Fog/HeterogeneousVolumePayload.hpp>
#include <Engine/Graphic/Fog/VolumetricFogPayload.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/Materials/Clouds/MaterialCloudComposite.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RenderPassDeclaration.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/SystemRasterPass.hpp>
#include <Engine/Graphic/Systems/Scene/Fog/VolumetricFogRenderer.hpp>
#include <Engine/Graphic/View/PassHistory.hpp>
#include <Engine/ShaderResources/StorageBuffer.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    struct ViewFrame;
}

namespace Desert::Graphic::System
{
    // The lighting cache program's parameters
    // (Programs/HeterogeneousVolumes/HeterogeneousVolumeLightingCache.shader), the froxels' lighting inputs under
    // the same names (VolumetricFogLightScatteringShaderParameters): kAll is the atlas permutation, kVirtualLocal
    // the DESERT_LOCAL_SHADOW_VSM one. The parameter block (kVolumetricFogParamsBinding) and the volumes
    // (kHeterogeneousVolumesBinding) are the pipeline's own buffers.
    struct HeterogeneousVolumeLightingCacheShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, LightingCacheTexture );
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
        DESERT_SHADER_PARAMETER( Texture, CloudShadowMapTexture );
        DESERT_SHADER_PARAMETER( Texture, RainOcclusionMapTexture );
        static constexpr std::array kAll{ RDG::Erase( LightingCacheTexture ),
                                          RDG::Erase( ShadowCascadesTexture ),
                                          RDG::Erase( LocalShadowAtlasTexture ),
                                          RDG::Erase( DistantSkyLightTexture ),
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
        static constexpr std::array kVirtualLocal{
             RDG::Erase( LightingCacheTexture ),   RDG::Erase( ShadowCascadesTexture ),
             RDG::Erase( DistantSkyLightTexture ), RDG::Erase( LinearClampSampler ),
             RDG::Erase( LinearRepeatSampler ),    RDG::Erase( PointLightsUB ),
             RDG::Erase( SpotLightsUB ),           RDG::Erase( LightGridParamsUB ),
             RDG::Erase( LightGridCellsUB ),       RDG::Erase( kVsmFrameDataBlock ),
             RDG::Erase( kVsmPageTableBlock ),     RDG::Erase( kVsmPhysicalPool ),
             RDG::Erase( kVsmLocalLightsBlock ),   RDG::Erase( CloudShadowMapTexture ),
             RDG::Erase( RainOcclusionMapTexture ) };
    };

    // The march program's parameters (HeterogeneousVolumeMarch.shader).
    struct HeterogeneousVolumeMarchShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, MarchTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, MarchGuideTexture );
        DESERT_SHADER_PARAMETER( Texture, MarchHistoryTexture );
        DESERT_SHADER_PARAMETER( Texture, LightingCacheTexture );
        DESERT_SHADER_PARAMETER( Texture, SceneDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearClampSampler );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        static constexpr std::array kAll{ RDG::Erase( MarchTexture ),        RDG::Erase( MarchGuideTexture ),
                                          RDG::Erase( MarchHistoryTexture ), RDG::Erase( LightingCacheTexture ),
                                          RDG::Erase( SceneDepthTexture ),   RDG::Erase( LinearClampSampler ),
                                          RDG::Erase( PointClampSampler ) };
    };

    // The upsample program's parameters (HeterogeneousVolumeUpsample.shader).
    struct HeterogeneousVolumeUpsampleShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageTexture, UpsampledTexture );
        DESERT_SHADER_PARAMETER( Texture, MarchTexture );
        DESERT_SHADER_PARAMETER( Texture, MarchGuideTexture );
        DESERT_SHADER_PARAMETER( Texture, SceneDepthTexture );
        DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
        static constexpr std::array kAll{ RDG::Erase( UpsampledTexture ), RDG::Erase( MarchTexture ),
                                          RDG::Erase( MarchGuideTexture ), RDG::Erase( SceneDepthTexture ),
                                          RDG::Erase( PointClampSampler ) };
    };

    /**
     * @brief UE 5.3+ Heterogeneous Volumes (HeterogeneousVolumes.cpp, reimplemented): boxes of smoke, steam and
     * fire with a varying density (ECS::HeterogeneousVolumeComponent), ray-marched at half resolution and lit by
     *        the froxels' own lighting (Common/VolumeLighting.glslh over
     * VolumetricFogRenderer::PackVolumeLighting).
     *
     * Per frame, per view:
     *   1. "HeterogeneousVolumes: LightingCache" (DeclareLightingNodes, before the froxels): 32^3 voxels per
     *      on-screen volume, lit by the sun (cascades, cloud layer and the volume's own transmittance), the local
     *      lights and the sky. The cache's .a shadows the volumetric fog (GetShadowInput).
     *   2. "HeterogeneousVolumes: March" (DeclareViewNodes): half-res lit march against the scene depth, temporal
     *      reprojection over a ping-pong pair.
     *   3. "HeterogeneousVolumes: Upsample": depth-aware to full resolution.
     *   4. "HeterogeneousVolumes: Composite" (CompositePass): over the scene colour in the translucency stage,
     * after the clouds' composite (the smoke is nearer than the sky).
     *
     * ZERO COST WITHOUT A VOLUME ON SCREEN: no volume gathered or none whose box reaches the view frustum - no
     * node, no upload, no allocation (the images are created the first frame a volume is seen).
     */
    class HeterogeneousVolumeRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;
        ~HeterogeneousVolumeRenderer() override;

        Common::BoolResultStr Initialize() override;

        [[nodiscard]] static Core::ShaderVariant                     LightingCacheVsmVariant();
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants();

        void OnSceneReplaced() override
        {
            m_History.Invalidate();
        }
        void OnTemporalHistoryReset() override
        {
            m_History.Invalidate();
        }

        /**
         * @brief Culls @p volumes against this view and lights the survivors' cache. Declared once per view frame
         *        (a second call in the same frame returns no node); empty when no volume is on screen.
         */
        ComputeNodeList DeclareLightingNodes( Common::Memory::FrameArena& arena, const FrameGraphRefs& refs,
                                              const ViewFrame& frame, const VolumetricFogSunInput& sun,
                                              bool                                    virtualLocalShadows,
                                              std::span<const HeterogeneousVolumeGpu> volumes );

        // Valid after DeclareLightingNodes of this frame (Count 0 otherwise).
        [[nodiscard]] const HeterogeneousVolumeShadowInput& GetShadowInput() const
        {
            return m_ShadowInput;
        }

        // The march and the upsample, after the scene depth is final. Empty when the cache was not lit this frame.
        ComputeNodeList DeclareViewNodes( Common::Memory::FrameArena& arena, const FrameGraphRefs& refs,
                                          const ViewFrame& frame );

        // The full-screen blend of the upsampled image over the scene colour; inactive (no Execute) on a frame
        // without a volume on screen.
        SystemRasterPass CompositePass();

    private:
        bool                                CreatePipelines();
        [[nodiscard]] Common::BoolResultStr EnsureLightingCacheVsmPipeline();
        bool                                EnsureCache();
        bool                                EnsureViewImages( uint32_t width, uint32_t height );

        std::shared_ptr<ComputePipeline>  m_CachePipeline;
        std::shared_ptr<ComputePipeline>  m_CacheVsmPipeline;
        std::string                       m_CacheVsmRefusal;
        std::shared_ptr<ComputePipeline>  m_MarchPipeline;
        std::shared_ptr<ComputePipeline>  m_UpsamplePipeline;
        std::shared_ptr<GraphicsPipeline> m_CompositePipeline;
        mutable ShaderBindingLayoutCache  m_CacheLayout;
        mutable ShaderBindingLayoutCache  m_CacheVsmLayout;
        mutable ShaderBindingLayoutCache  m_MarchLayout;
        mutable ShaderBindingLayoutCache  m_UpsampleLayout;
        mutable ShaderBindingLayoutCache  m_CompositeLayout;
        // The clouds' composite program and material (scene = rgb x PreExposure + scene x a): the same
        // over-operator.
        std::unique_ptr<MaterialCloudComposite> m_CompositeMaterial;

        std::shared_ptr<ShaderResources::StorageBuffer> m_LightingParams; // VolumetricFogGpuPayload, lighting half
        std::shared_ptr<ShaderResources::StorageBuffer> m_VolumesBuffer;  // the on-screen volumes, at the cap
        std::shared_ptr<ShaderResources::StorageBuffer> m_MarchParams;    // HeterogeneousVolumeMarchGpu

        std::shared_ptr<Image3D>                m_Cache;
        std::array<std::shared_ptr<Image2D>, 2> m_March{};
        std::shared_ptr<Image2D>                m_Guide;
        std::shared_ptr<Image2D>                m_Upsampled;
        glm::uvec2                              m_ViewSize{ 0u };
        uint32_t                                m_Current = 0;
        PassHistoryStamp                        m_History;

        // This frame's state, reset by DeclareLightingNodes.
        HeterogeneousVolumeShadowInput m_ShadowInput;
        uint64_t                       m_LitFrame     = ~0ull;
        bool                           m_LitThisFrame = false;
        glm::vec4                      m_ScreenRect{ 0.0f };
        bool                           m_Composite = false;
    };
} // namespace Desert::Graphic::System
