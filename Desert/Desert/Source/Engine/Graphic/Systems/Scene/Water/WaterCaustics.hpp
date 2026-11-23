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
        // The frame row (its body table: each map texel's still level is the level of the body over it).
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterCausticsFrameUB );
        static constexpr std::array kAll{ RDG::Erase( WaterTiles ), RDG::Erase( WaterInfoTexture ),
                                          RDG::Erase( WaterCausticsEnergy ), RDG::Erase( WaterCausticsFrameUB ) };
    };

    // The resolve shader's parameters (Programs/Water/WaterCausticsResolve.shader).
    struct WaterCausticsResolveShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, WaterCausticsEnergy );
        DESERT_SHADER_PARAMETER( StorageTexture, WaterCausticsImage );
        static constexpr std::array kAll{ RDG::Erase( WaterCausticsEnergy ), RDG::Erase( WaterCausticsImage ) };
    };

    // The frame row's head, std430 (Common/WaterCaustics.glslh WaterCausticsFrameUB), followed by Shape.x rows of
    // WaterCausticsBodyRow (the block's runtime array WaterCausticsBodies, two vec4 per body): read by every
    // receiver with the map (SceneViewInputs::WaterCausticsFrame) and by the splat.
    struct WaterCausticsFrameHead
    {
        glm::vec4 Window{
             0.0f };             // xy = the map's lower world (X, Z), z = 1 / side (cm), w = 1 when there is a map
        glm::vec4 Shape{ 0.0f }; // x = the body rows that follow, y = onset depth, z = fade depth (cm), w = strength
    };
    static_assert( sizeof( WaterCausticsFrameHead ) == 2 * sizeof( glm::vec4 ), "std430 head of two vec4" );
    // One body of the frame whose footprint meets the map's square.
    struct WaterCausticsBodyRow
    {
        glm::vec4 Footprint{ 0.0f }; // xy = min (X, Z), zw = max (cm)
        glm::vec4 Level{ 0.0f };     // x = the still surface Y (cm)
    };
    static_assert( sizeof( WaterCausticsBodyRow ) == 2 * sizeof( glm::vec4 ), "std430 body of two vec4" );

    // What every receiver binds when the caustics draw (FrameTransients::WaterCausticsMap / WaterCausticsFrame ->
    // SceneViewInputs): two valid refs of this frame's graph. A frame without caustics keeps the graph's 'off'
    // pair (FrameTextures::SetSceneViewInputsOff: System.White and a zeroed row whose Window.w is 0), so the
    // receiver returns exactly 1.0 and the frame is the frame without them, bit for bit.
    struct WaterCausticsFrame
    {
        RDG::TextureRef Map;
        RDG::BufferRef  Row;
    };

    // THE WATER'S CAUSTICS (WATER-CAUSTICS; Evan Wallace's WebGL water / GPU Gems 1 ch. 2, the map UE's water
    // samples in its lighting): each frame the sun is refracted through the water surface the water mesh draws -
    // the same waves, the same WaterInfo damping - onto the floor below and the landed light's density is a
    // kMapSize^2 map of one world square, "Water: Caustics Splat" + "Water: Caustics Resolve". The square is
    // centred on the point of the body nearest the camera that is closest to it (the camera itself when it is over
    // that body); every body of the frame that meets the square is in the frame row, each with its own still
    // level. Every sun receiver - the deferred direct pass, the forward meshes, the glass, the forward terrain -
    // multiplies the sun under a body's surface by it (Common/WaterCaustics.glslh WaterCausticsFactor), through
    // the scene/view inputs. The lit particles read the sun from the translucency lighting volume, whose cells
    // (metres) average the map to its mean, 1: they take none. Scalability Effects.WaterCaustics (off Low /
    // Medium).
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
        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_Enabled;
        }

        // The frame's water as the caustics read it (SceneRenderer, from WaterRenderer after FinishPrepare).
        struct FrameInputs
        {
            // WaterRenderer::GetFrameWavesBlock: the waves block every body of the frame draws with; empty = no
            // water.
            std::span<const std::byte> Waves;
            // WaterRenderer::GetFrameBodies: every body drawn this frame (its footprint and still level).
            std::span<const Water::WaterQuadTreeBody> Bodies;
            // The imported WaterInfo ground (FrameTextures::Import of WaterRenderer::GetWaterInfo), or invalid
            // when none is baked (the waves block's WaterInfoFrame.w is then 0 and the shader reads no texel of
            // it).
            RDG::TextureRef WaterInfo;
            glm::vec3       SunTravel{ 0.0f, -1.0f, 0.0f }; // the direction the sunlight travels
            glm::vec3       Camera{ 0.0f };                 // the view's world position (cm)
            float           TimeSeconds = 0.0f; // ViewFrame::TimeSeconds = the water mesh's cameraUB.Time
            // FrameGraphRefs::System.Black: the splat's ground where no WaterInfo is baked.
            RDG::TextureRef Black;
        };

        // Adds this frame's passes when the caustics draw (enabled, a body with waves, the sun above the horizon,
        // both programs built) and returns what every receiver binds; otherwise nullopt and no pass or upload:
        // the frame keeps the graph's 'off' pair (FrameTextures::SetSceneViewInputsOff), the one default.
        [[nodiscard]] std::optional<WaterCausticsFrame> AddPasses( RDG::Builder&      graph,
                                                                   const FrameInputs& inputs );

    private:
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
