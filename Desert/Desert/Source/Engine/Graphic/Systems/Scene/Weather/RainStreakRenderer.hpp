#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Systems/Scene/Weather/WeatherLayout.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/SystemRasterPass.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Graphic::System
{
    // The RainStreaks shader's parameters: the one spelling the pass binds and Initialize checks.
    struct RainStreaksShaderParameters
    {
        DESERT_SHADER_PARAMETER( UniformBuffer, RainStreaksUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, WorldParameters );
        DESERT_SHADER_PARAMETER( Texture, RainOcclusionMapTexture );
        DESERT_SHADER_PARAMETER( Sampler, RainOcclusionSampler );
        DESERT_SHADER_PARAMETER( UniformBuffer, RainOcclusionUB );
        static constexpr std::array kAll{ RDG::Erase( RainStreaksUB ), RDG::Erase( WorldParameters ),
                                          RDG::Erase( RainOcclusionMapTexture ), RDG::Erase( RainOcclusionSampler ),
                                          RDG::Erase( RainOcclusionUB ) };
    };

    // The frame's graph resources the pass reads (created by SceneRendererImpl::AddFrameRainStreaks).
    struct RainStreaksFrameInputs
    {
        RDG::BufferRef  Uniforms;
        RDG::BufferRef  WorldParameters;
        RDG::TextureRef RainOcclusionMap;
        RDG::BufferRef  RainOcclusion;
        uint32_t        Drops = 0; // instances (DropCount)
    };

    /**
     * @brief THE FALLING RAIN (NG-PUDDLES pass 3; UE: the Niagara GPU rain of the weather samples). Procedural GPU
     * streaks in a world-anchored box around the camera (Programs/Weather/RainStreaks.shader): no simulation buffer,
     * no CPU particle. Every value is the world's (WorldParameters RainIntensity / RainDrops / RainDropShape), the air
     * is the scene's (ViewFrame::WindVelocityXZ + WindGust, what the SceneWind block carries), the roof cull is the
     * view's rain occlusion map, and the count is scaled by the Effects group's spawn-count scale (UE Niagara
     * SpawnCountScale, Scalability Effects.SpawnCountScalePercent). One raster pass in the translucency, after the
     * particles (SceneRendererImpl::AddFrameTranslucency).
     */
    class RainStreakRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        Common::BoolResultStr Initialize() override;

        // The ceiling on drawn drops (6 vertices each): beyond it the box is denser than any eye resolves.
        static constexpr uint32_t kMaxDrops = 1u << 20;

        /// Drops in the box: density (per m^3) x the box's volume (m^3) x RainIntensity x the spawn-count scale.
        /// 0 = no rain (no pass).
        [[nodiscard]] static uint32_t DropCount( float rainIntensity, const glm::vec4& rainDrops,
                                                 float spawnCountScale ) noexcept;

        /// The pass on the scene target, depth read-only. No target or pipeline: an empty pass (not added).
        [[nodiscard]] SystemRasterPass DrawPass( const RainStreaksFrameInputs& inputs );

        [[nodiscard]] bool IsReady() const noexcept
        {
            return m_Pipeline != nullptr;
        }

    private:
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        mutable ShaderBindingLayoutCache  m_Layout;
    };
} // namespace Desert::Graphic::System
