#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GlobalIllumination/SurfaceCacheScene.hpp>
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

        // The captured layers this renderer owns (SurfaceCache::Layer Depth..Emissive). The lit layers arrive
        // with the card lighting (the next task of the programme).
        static constexpr std::array<Layer, 5> kCapturedLayers{ Layer::Depth, Layer::Albedo, Layer::Opacity,
                                                               Layer::Normal, Layer::Emissive };

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
        // "Surface Cache Atlas": the four captured layers, a quadrant each (albedo, normal, depth, opacity).
        [[nodiscard]] Common::BoolResultStr AddAtlasViewPass( RDG::Builder& graph, const ImportImage& import,
                                                              RDG::TextureRef target );
        // "Lumen Scene": the scene drawn from its cards alone - every captured card's surface (its depth layer
        // placed back into the world) shaded with its albedo, depth-tested against the other cards.
        [[nodiscard]] Common::BoolResultStr AddSceneViewPass( RDG::Builder& graph, const ImportImage& import,
                                                              RDG::TextureRef target, const SceneFrame& frame,
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
        std::shared_ptr<Framebuffer>                                          m_CaptureBuffer;
        std::shared_ptr<Shader>                                               m_CopyShader;
        std::shared_ptr<ComputePipeline>                                      m_CopyPipeline;
        mutable ShaderBindingLayoutCache                                      m_CopyLayout;
        std::shared_ptr<Shader>                                               m_AtlasViewShader;
        mutable ShaderBindingLayoutCache                                      m_AtlasViewLayout;
        std::shared_ptr<Shader>                                               m_SceneViewShader;
        mutable ShaderBindingLayoutCache                                      m_SceneViewLayout;
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_AtlasViewPipelines;
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_SceneViewPipelines;
    };
} // namespace Desert::Graphic::SurfaceCache
