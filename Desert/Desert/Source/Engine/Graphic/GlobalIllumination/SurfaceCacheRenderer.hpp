#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Clouds/CloudShadowPayload.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/GlobalIllumination/SurfaceCacheScene.hpp>
#include <Engine/Graphic/Materials/SceneResources.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string_view>
#include <utility>

namespace Desert::Graphic
{
    class ComputePipeline;
    class Framebuffer;
    class GraphicsPipeline;
    class Image;
    class Image2D;
    class Shader;
} // namespace Desert::Graphic

// THE SURFACE CACHE'S GPU HALF (GI-SURFACE-CACHE-2): the atlases, the capture atlas and the passes that fill and
// show them. UE: FLumenSceneData's atlases + LumenSceneCardCapture.cpp (the capture atlas, then
// CopyCardsToSurfaceCache) + the "Lumen Scene" / "Surface Cache" visualisations (LumenVisualize.cpp).
//
// THE CAPTURE IS THE G-BUFFER PASS, NOT A SHADER OF ITS OWN (UE LumenCardPixelShader writes the same material
// outputs): MeshRenderer draws the card's mesh through the card's orthographic camera with the G-buffer program's
// light-view permutation (DESERT_GBUFFER_RSM: world from the push transform, no shading word) into this
// renderer's capture framebuffer, whose slots are ViewTargetFormats::kRSMColourSlots - the one list that pipeline
// is built against. AddCopyPass then moves each captured card's pages out of it into the atlas layers.
namespace Desert::Graphic::SurfaceCache
{
    class SurfaceCacheRenderer
    {
    public:
        // UE r.LumenScene.SurfaceCache.CardCaptureAtlas: the frame's captures share one atlas of this side; a
        // card that does not fit waits for the next frame (SurfaceCacheScene::Update packs it).
        static constexpr uint32_t kCaptureAtlasSize = 1024;
        // UE r.LumenScene.SurfaceCache.CardCapturesPerFrame (300): the cap on one frame's captures.
        static constexpr uint32_t kMaxCapturesPerFrame = 256;

        using ImportImage = std::function<RDG::TextureRef( const std::shared_ptr<Image>&, std::string_view )>;

        // The captured layers this renderer owns (SurfaceCache::Layer Depth..Emissive).
        static constexpr std::array<Layer, 5> kCapturedLayers{ Layer::Depth, Layer::Albedo, Layer::Opacity,
                                                               Layer::Normal, Layer::Emissive };
        // The lit layers of the direct lighting (GI-SURFACE-CACHE-4). IndirectLighting and the radiosity count
        // arrive with the radiosity (the next task of the programme).
        static constexpr std::array<Layer, 2> kLitLayers{ Layer::DirectLighting, Layer::FinalLighting };

        // The frame's records as graph buffers, uploaded once and read by every pass of the frame.
        struct FrameBuffers
        {
            RDG::BufferRef Cards;
            RDG::BufferRef Pages;
        };
        // What the card lighting is lit by: THE SAME inputs the deferred composite reads this frame (its scene
        // view inputs - cascades, cloud map, local shadow atlas -, the view's light buffers, the composite's
        // ShadowUB and cloud values, the sun).
        struct DirectLightingInputs
        {
            SceneViewInputs              View;
            ViewLightBuffers             Lights;
            SceneResources::ShadowUBData Shadow{};
            CloudShadowUniforms          Cloud;
            glm::vec4                    LightDir{ 0.0f, -1.0f, 0.0f, 0.0f };
            glm::vec4                    LightColor{ 0.0f };
            uint32_t                     PointCount = 0;
            uint32_t                     SpotCount  = 0;
        };

        // Shaders, the copy pipeline and the capture framebuffer.
        [[nodiscard]] Common::BoolResultStr Initialize();
        // The machine's budget; a new atlas side recreates the layers and drops every capture.
        [[nodiscard]] Common::BoolResultStr Configure( const SurfaceCacheBudget& budget );

        [[nodiscard]] SurfaceCacheScene& GetScene()
        {
            return m_Scene;
        }
        [[nodiscard]] const std::shared_ptr<Framebuffer>& GetCaptureBuffer() const
        {
            return m_CaptureBuffer;
        }

        // The capture framebuffer's graph textures (TargetsOf of GetCaptureBuffer): albedo, normal, emissive and
        // depth of the frame's capture atlas.
        struct CaptureTextures
        {
            RDG::TextureRef Albedo;
            RDG::TextureRef Normal;
            RDG::TextureRef Emissive;
            RDG::TextureRef Depth;
        };
        // UE CopyCardsToSurfaceCache: one 8 x 8 group per tile of every page of the frame's captures.
        [[nodiscard]] Common::BoolResultStr AddCopyPass( RDG::Builder& graph, const ImportImage& import,
                                                         const CaptureTextures& capture, const SceneFrame& frame );
        [[nodiscard]] FrameBuffers          UploadRecords( RDG::Builder& graph, const SceneFrame& frame ) const;
        // UE LumenSceneDirectLighting: the frame's LightingPages relit into DirectLighting and FinalLighting.
        [[nodiscard]] Common::BoolResultStr AddDirectLightingPass( RDG::Builder& graph, const ImportImage& import,
                                                                   const SceneFrame&           frame,
                                                                   const FrameBuffers&         buffers,
                                                                   const DirectLightingInputs& inputs );
        // "Surface Cache Atlas": the four captured layers, a quadrant each (albedo, normal, depth, opacity).
        [[nodiscard]] Common::BoolResultStr AddAtlasViewPass( RDG::Builder& graph, const ImportImage& import,
                                                              RDG::TextureRef target );
        // "Lumen Scene": the scene drawn from its cards alone - every captured card's surface (its depth layer
        // placed back into the world) shaded with its albedo, or with its FinalLighting when @p lit ("Lumen Scene
        // (Lit)"), depth-tested against the other cards.
        [[nodiscard]] Common::BoolResultStr AddSceneViewPass( RDG::Builder& graph, const ImportImage& import,
                                                              RDG::TextureRef target, const SceneFrame& frame,
                                                              const FrameBuffers& buffers, bool lit,
                                                              const glm::mat4& viewProjection,
                                                              const glm::vec3& cameraPosition );

    private:
        [[nodiscard]] Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
        ViewPipeline( std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>>& cache,
                      const std::shared_ptr<Shader>& shader, Core::Formats::ImageFormat format, uint32_t samples,
                      bool depth, std::string_view name );

        SurfaceCacheScene                                                     m_Scene;
        uint32_t                                                              m_AtlasSize = 0;
        std::array<std::shared_ptr<Image2D>, 5>                               m_Layers;
        std::array<std::shared_ptr<Image2D>, 2>                               m_LitLayers;
        std::shared_ptr<Framebuffer>                                          m_CaptureBuffer;
        std::shared_ptr<Shader>                                               m_CopyShader;
        std::shared_ptr<ComputePipeline>                                      m_CopyPipeline;
        mutable ShaderBindingLayoutCache                                      m_CopyLayout;
        std::shared_ptr<Shader>                                               m_AtlasViewShader;
        mutable ShaderBindingLayoutCache                                      m_AtlasViewLayout;
        std::shared_ptr<Shader>                                               m_SceneViewShader;
        mutable ShaderBindingLayoutCache                                      m_SceneViewLayout;
        std::shared_ptr<Shader>                                               m_LightingShader;
        mutable ShaderBindingLayoutCache                                      m_LightingLayout;
        std::shared_ptr<GraphicsPipeline>                                     m_LightingPipeline;
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_AtlasViewPipelines;
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_SceneViewPipelines;
    };
} // namespace Desert::Graphic::SurfaceCache
