#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGShaderParameters.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Water/WaterQuadTree.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace Desert::Graphic::System
{
    // The splat shader's parameters (Programs/Water/WaterCausticsSplat.shader): the one spelling the pass binds
    // and Initialize checks. WaterTiles / WaterInfoTexture are the water mesh's own names
    // (Mesh/Surface/WaterMeshTiles.glslh): the waves block and the WaterInfo ground.
    struct WaterCausticsSplatShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterTiles );
        DESERT_SHADER_PARAMETER( Texture, WaterInfoTexture );
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterCausticsEnergy );
        static constexpr std::array kAll{ RDG::Erase( WaterTiles ), RDG::Erase( WaterInfoTexture ),
                                          RDG::Erase( WaterCausticsEnergy ) };
    };

    // The resolve shader's parameters (Programs/Water/WaterCausticsResolve.shader).
    struct WaterCausticsResolveShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterCausticsEnergy );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterCausticsImage );
        static constexpr std::array kAll{ RDG::Erase( WaterCausticsEnergy ), RDG::Erase( WaterCausticsImage ) };
    };

    // The receiver's frame row, std430 (Common/WaterCaustics.glslh WaterCausticsFrameUB): read by the deferred
    // direct pass with the map.
    struct WaterCausticsFrameRow
    {
        glm::vec4 Window{
             0.0f };             // xy = the map's lower world (X, Z), z = 1 / side (cm), w = 1 when there is a map
        glm::vec4 Body{ 0.0f };  // the body's footprint: xy = min (X, Z), zw = max
        glm::vec4 Shape{ 0.0f }; // x = still surface Y, y = onset depth, z = fade depth (cm), w = strength
    };
    static_assert( sizeof( WaterCausticsFrameRow ) == 3 * sizeof( glm::vec4 ), "std430 row of three vec4" );

    // What the deferred direct pass binds (DeferredCompositeInputs::WaterCaustics / WaterCausticsFrame): ALWAYS
    // two valid refs of this frame's graph. A frame without caustics binds System.White and a frame row whose
    // Window.w is 0, so the receiver returns exactly 1.0 and the frame is the frame without them, bit for bit.
    struct WaterCausticsFrame
    {
        RDG::TextureRef Map;
        RDG::BufferRef  Row;
    };

    // THE WATER'S CAUSTICS (WATER-CAUSTICS; Evan Wallace's WebGL water / GPU Gems 1 ch. 2, the map UE's water
    // samples in its lighting): each frame the sun is refracted through the water surface the water mesh draws -
    // the same waves, the same WaterInfo damping - onto the floor below and the landed light's density is a
    // kMapSize^2 map of one world square around the camera, "Water: Caustics Splat" + "Water: Caustics Resolve".
    // The deferred direct pass multiplies the sun on the G-buffer texels under the body's surface by it
    // (Common/WaterCaustics.glslh WaterCausticsFactor). Scalability Effects.WaterCaustics (off Low / Medium).
    class WaterCaustics final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        // The map's side in texels (Common/WaterCaustics.glslh WATER_CAUSTICS_SIZE).
        static constexpr uint32_t kMapSize = 512;

        Common::BoolResultStr Initialize() override;

        // Effects.WaterCaustics (SceneRenderer::OnUpdate, with every quality choice of the view).
        void SetEnabled( bool enabled ) noexcept
        {
            m_Enabled = enabled;
        }

        // The frame's water as the caustics read it (SceneRenderer, from WaterRenderer after FinishPrepare).
        struct FrameInputs
        {
            // WaterRenderer::GetFrameWavesBlock: the waves block the water mesh draws with; empty = no water.
            std::span<const std::byte> Waves;
            // WaterRenderer::GetFrameBody: the body the waves are of (its footprint and still level).
            std::optional<Water::WaterQuadTreeBody> Body;
            // The imported WaterInfo ground (FrameTextures::Import of WaterRenderer::GetWaterInfo), or invalid
            // when none is baked (the waves block's WaterInfoFrame.w is then 0 and the shader reads no texel of
            // it).
            RDG::TextureRef WaterInfo;
            glm::vec3       SunTravel{ 0.0f, -1.0f, 0.0f }; // the direction the sunlight travels
            glm::vec3       Camera{ 0.0f };                 // the view's world position: the map's centre
            float           TimeSeconds = 0.0f; // ViewFrame::TimeSeconds = the water mesh's cameraUB.Time
            // FrameGraphRefs::System: the neutral textures a frame without caustics binds.
            RDG::TextureRef White;
            RDG::TextureRef Black;
        };

        // Adds this frame's passes when the caustics draw (enabled, a body with waves, the sun above the horizon,
        // both programs built) and returns what the direct pass binds; otherwise the neutral pair (no pass).
        [[nodiscard]] WaterCausticsFrame AddPasses( RDG::Builder& graph, const FrameInputs& inputs );

    private:
        // The neutral pair: @p white and a frame row with no map.
        [[nodiscard]] static WaterCausticsFrame Off( RDG::Builder& graph, RDG::TextureRef white );
        // Says "[Water] caustics=on res=N" / "=off" when the state changes, never per frame.
        void ReportState( bool active, std::string_view why ) noexcept;

        std::shared_ptr<ComputePipeline> m_SplatPipeline;
        std::shared_ptr<ComputePipeline> m_ResolvePipeline;
        ShaderBindingLayoutCache         m_SplatLayout;
        ShaderBindingLayoutCache         m_ResolveLayout;
        bool                             m_Enabled = false;
        std::optional<bool>              m_Reported; // the state last said, nullopt = nothing said yet
    };
} // namespace Desert::Graphic::System
