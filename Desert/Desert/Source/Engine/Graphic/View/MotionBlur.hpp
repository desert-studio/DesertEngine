#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace Desert::Graphic::RDG
{
    class Builder;
    struct ShaderBindingLayout;
} // namespace Desert::Graphic::RDG

namespace Desert::Graphic
{
    class ComputePipeline;

    // MR2. Real-time motion blur from the velocity target, UE's PostProcessMotionBlur in outline (McGuire's
    // tile-max reconstruction):
    //   1. Flatten     — per RENDER pixel: the velocity (View/Velocity.hpp, NDC current - previous) turned into
    //                    OUTPUT pixels of blur (VelocityScale, clamped to MaxPixels), packed with the scene depth;
    //   2. TileMax     — per kMotionBlurTileSize x kMotionBlurTileSize render tile: the longest flattened
    //   velocity;
    //   3. NeighborMax — per tile: the longest of the 3x3 tile neighbourhood, so a moving silhouette blurs over
    //   the
    //                    still pixels next to it (the soft edge);
    //   4. Gather      — per OUTPUT pixel: Samples taps of the resolved colour along the neighbourhood's dominant
    //                    velocity, weighted by depth (foreground / background) and by each tap's own velocity.
    //
    // WHERE IT RUNS. On the resolved colour, after the temporal resolve (and the spatial upscale / SSAA downsample
    // / sharpen that complete it) and before the overlay target set is built, so the overlays (debug lines,
    // gizmos, UI) are never blurred and the post chain (bloom, exposure, tonemap) reads the blurred colour - UE's
    // order (TAA/TSR -> MotionBlur -> post). SceneRenderer::AddFrameTemporal calls it; a frame with no resolve but
    // with motion blur still goes through that function so the overlays draw onto the blurred colour.
    //
    // The camera's own motion is in the velocity already (VelocityNdc compares PrevViewProjection * PrevWorld with
    // ViewProjection * World), so a turning camera blurs a still world. A still camera over still objects writes
    // exactly 0 (and a reset view reprojects onto itself, ViewFrame), which the gather returns as the unmodified
    // colour; with TargetFPS > 0 a frame whose DeltaSeconds is 0 (paused, reset) has VelocityScale 0 and no nodes.
    inline constexpr uint32_t kMotionBlurTileSize = 16; // render pixels per tile side (UE: 16)

    // The view's motion blur settings this frame: the resolved post-process grade (PostProcessSettings
    // MotionBlurAmount / MotionBlurMax / MotionBlurTargetFPS) and the scalability sample count
    // (PostProcess.MotionBlurQuality via MotionBlurSamplesForQuality). SceneRenderer::BeginScene fills it.
    struct MotionBlurSettings
    {
        float Amount     = 0.0f; // shutter: the fraction of the frame's motion that blurs (UE MotionBlurAmount)
        float MaxPercent = 0.0f; // longest blur, percent of the output width (UE MotionBlurMax)
        float TargetFPS  = 0.0f; // > 0: blur as if the frame lasted 1 / TargetFPS (UE MotionBlurTargetFPS)
        int   Samples    = 0;    // gather taps per pixel; 0 = off (MotionBlurQuality 0)
    };

    // PostProcess.MotionBlurQuality -> gather taps: 0 off, 1 Low, 2 High (UE r.MotionBlurQuality, sg.PostProcess).
    inline constexpr int kMotionBlurSamplesLow  = 8;
    inline constexpr int kMotionBlurSamplesHigh = 16;
    [[nodiscard]] int    MotionBlurSamplesForQuality( int quality );

    // NDC velocity -> output-pixel velocity multiplier, before the output size: Amount, and with TargetFPS > 0
    // the frame-time normalisation (1 / TargetFPS) / DeltaSeconds; 0 when that frame has no duration.
    [[nodiscard]] float MotionBlurVelocityScale( const MotionBlurSettings& settings, float deltaSeconds );

    // Whether the four nodes are added this frame: an amount, a sample count, a maximum, a non-zero scale and a
    // non-empty split. Nothing is added otherwise (the graph has no motion blur node to cull).
    [[nodiscard]] bool MotionBlurRuns( const MotionBlurSettings& settings, const ViewFrame& frame );

    // The longest blur in output pixels: MaxPercent of the output width.
    [[nodiscard]] float MotionBlurMaxPixels( const MotionBlurSettings& settings, ViewExtent output );

    // Flatten's per-pixel value (CPU twin of MotionBlurFlatten.shader): NDC velocity -> output pixels
    // (ndc * (0.5, -0.5) * output * scale, Velocity.hpp's uv convention), its length clamped to maxPixels.
    [[nodiscard]] glm::vec2 MotionBlurPixelVelocity( glm::vec2 velocityNdc, float scale, ViewExtent output,
                                                     float maxPixels );

    // TileMax / NeighborMax reductions (CPU twins of the shaders): the longest vector wins; on a tie the first.
    [[nodiscard]] glm::vec2 LongestVelocity( std::span<const glm::vec2> velocities );
    // The longest of the 3x3 tiles around (x, y) of a width x height tile grid (row-major), edges clamped.
    [[nodiscard]] glm::vec2 NeighborhoodMaxVelocity( std::span<const glm::vec2> tiles, uint32_t width,
                                                     uint32_t height, uint32_t x, uint32_t y );

    [[nodiscard]] RDG::Extent3D MotionBlurTileExtent( ViewExtent render );

    // The parameters every motion blur pass reads (one buffer, binding "MotionBlurBuffer" in all four shaders).
    struct MotionBlurParams
    {
        glm::vec2 RenderSize;           // pixels: velocity, depth, flattened velocity
        glm::vec2 OutputSize;           // pixels: resolved colour, output
        glm::vec2 TileCount;            // tiles
        float     VelocityScale = 0.0f; // MotionBlurVelocityScale
        float     MaxPixels     = 0.0f; // MotionBlurMaxPixels
        int32_t   Samples       = 0;
        float     Pad[3]        = {};
    };
    static_assert( offsetof( MotionBlurParams, OutputSize ) == 8 );
    static_assert( offsetof( MotionBlurParams, TileCount ) == 16 );
    static_assert( offsetof( MotionBlurParams, VelocityScale ) == 24 );
    static_assert( offsetof( MotionBlurParams, MaxPixels ) == 28 );
    static_assert( offsetof( MotionBlurParams, Samples ) == 32 );
    static_assert( sizeof( MotionBlurParams ) == 48 );

    [[nodiscard]] MotionBlurParams MakeMotionBlurParams( const MotionBlurSettings& settings,
                                                         const ViewFrame&          frame );

    // The binding layouts of the four shaders as the graph sees them; the ShaderCacheKey suite pins each against
    // the compiled shader's reflection.
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& MotionBlurFlattenLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& MotionBlurTileMaxLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& MotionBlurNeighborMaxLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& MotionBlurGatherLayout();

    struct MotionBlurInputs
    {
        RDG::TextureRef SceneColor; // linear HDR, OutputExtent (the resolved colour)
        RDG::TextureRef SceneDepth; // reversed-Z device depth, RenderExtent, single-sample
        RDG::TextureRef Velocity;   // ViewTargetFormats::kVelocity, RenderExtent
    };

    class MotionBlur
    {
    public:
        MotionBlur();
        ~MotionBlur();
        MotionBlur( const MotionBlur& )            = delete;
        MotionBlur& operator=( const MotionBlur& ) = delete;

        // Adds "MotionBlur: Flatten", "MotionBlur: TileMax", "MotionBlur: NeighborMax", "MotionBlur: Gather" and
        // returns the blurred colour (OutputExtent, RGBA16F). An error names what is missing or why it cannot run;
        // the caller decides (MotionBlurRuns false is the caller's "do not call").
        [[nodiscard]] Common::ResultStr<RDG::TextureRef> AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                                                                    const MotionBlurSettings& settings,
                                                                    const MotionBlurInputs&   inputs ) const;

    private:
        enum class Kernel : uint32_t
        {
            Flatten,
            TileMax,
            NeighborMax,
            Gather,
            Count
        };
        std::shared_ptr<ComputePipeline> PipelineFor( Kernel kernel, std::string& error ) const;

        mutable std::mutex m_PipelineMutex;
        mutable std::array<std::shared_ptr<ComputePipeline>, static_cast<std::size_t>( Kernel::Count )>
             m_Pipelines;
    };
} // namespace Desert::Graphic
