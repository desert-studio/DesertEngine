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
#include <string>

namespace Desert::Graphic::RDG
{
    class Builder;
    struct ShaderBindingLayout;
} // namespace Desert::Graphic::RDG

namespace Desert::Graphic
{
    class ComputePipeline;

    // MR3. Depth of field, UE's Diaphragm DOF in outline: a PHYSICAL circle of confusion from the thin-lens model
    // and a gather of the foreground and the background layers apart, recombined with the full-resolution colour.
    //
    // THE LENS. UE keeps DepthOfFieldFocalDistance / DepthOfFieldFstop / DepthOfFieldSensorWidth in the
    // post-process settings (FPostProcessSettings) and a CineCamera drives them from its filmback and lens; the
    // FOCAL LENGTH is not authored, it follows from the field of view and the sensor width (CineCamera: FOV = 2
    // atan(w / 2f)). Here the same: Core::PostProcessSettings DepthOfField* (volume-blended, ViewSettings) and the
    // focal length from the frame's projection: f = SensorWidth / 2 * Projection[0][0] (Projection[0][0] = 1 /
    // tan(hFOV / 2)).
    //
    // THE CIRCLE OF CONFUSION. Thin lens, aperture diameter A = f / N, focus distance s, scene distance z:
    //   CoC (on the sensor, mm) = | A * f * (z - s) / (z * (s - f)) |
    // mapped to output pixels by OutputWidth / SensorWidth. The engine keeps its RADIUS, SIGNED: negative in front
    // of the focus plane (foreground), positive behind (background), clamped to MaxBokehRadius (UE
    // r.DOF.Kernel.MaxForeground/BackgroundRadius, percent of the output width).
    //
    // THE PASSES (all compute):
    //   1. Setup            — per HALF-res pixel: the 2x2 average of the resolved colour and the signed CoC
    //                         (half-res pixels) of the scene depth at its centre, one RGBA16F;
    //   2. TileFlatten      — per kDofTileSize^2 half-res tile: the largest foreground and background radius;
    //   3. TileDilate       — per tile: the largest of the neighbourhood that the largest bokeh can reach
    //                         (DilateTiles tiles each side), so a pixel's kernel covers every disc landing on it;
    //   4. GatherForeground — per half-res pixel: ring samples (DofRingsForQuality) of the foreground discs that
    //                         cover it: colour and opacity (scatter-as-gather);
    //   5. GatherBackground — the same for the background discs: colour and presence;
    //   6. Recombine        — per OUTPUT pixel: the in-focus colour, the background layer by the pixel's own CoC,
    //                         the foreground layer over both by its opacity.
    //
    // WHERE IT RUNS. UE's post chain: TAA/TSR -> DOF -> MotionBlur -> bloom/exposure/tonemap. Here in
    // SceneRenderer::AddFrameTemporal on the resolved colour, after the resolve / spatial / SSAA / sharpen and
    // before MotionBlur (View/MotionBlur.hpp) and the overlay target set: overlays are never defocused.
    inline constexpr uint32_t kDofTileSize = 8; // half-res pixels per tile side

    // PostProcess.DepthOfFieldQuality -> gather rings: 0 off, 1 Low, 2 High (UE r.DOF.Quality, sg.PostProcess).
    // Ring k of R holds 8k samples at radius k / R of the tile's kernel, plus the centre sample.
    inline constexpr int kDofRingsLow  = 3;
    inline constexpr int kDofRingsHigh = 5;
    [[nodiscard]] int    DofRingsForQuality( int quality );
    [[nodiscard]] int    DofSampleCount( int rings ); // 1 + 4 R (R + 1)

    // The view's depth of field this frame: the resolved post-process grade and the scalability ring count.
    // SceneRenderer::BeginScene fills it.
    struct DepthOfFieldSettings
    {
        float FocalDistanceCm = 0.0f;    // UE DepthOfFieldFocalDistance; <= 0 = off (UE: no DOF)
        float FStop           = 4.0f;    // UE DepthOfFieldFstop
        float SensorWidthMm   = 24.576f; // UE DepthOfFieldSensorWidth (Super 35)
        float MaxBokehPercent = 0.0f;    // largest CoC RADIUS, percent of the output width
        int   Rings           = 0;       // DofRingsForQuality; 0 = off
    };

    // The lens the CoC is computed from, millimetres except the focus distance.
    struct DofLens
    {
        float FocalDistanceCm = 0.0f;
        float FStop           = 0.0f;
        float SensorWidthMm   = 0.0f;
        float FocalLengthMm   = 0.0f;
    };

    // f = SensorWidth / 2 * Projection[0][0] (the horizontal field of view of the unjittered projection).
    [[nodiscard]] float   DofFocalLengthMm( float sensorWidthMm, const glm::mat4& projection );
    [[nodiscard]] DofLens MakeDofLens( const DepthOfFieldSettings& settings, const ViewFrame& frame );

    // The CoC diameter on the sensor in mm (the thin-lens formula above, unsigned); an infinitely far z gives
    // the limit A f / (s - f).
    [[nodiscard]] float DofCocDiameterMm( const DofLens& lens, float sceneDistanceCm );
    // The signed CoC RADIUS in output pixels (negative = foreground), clamped to +-maxRadiusPixels.
    [[nodiscard]] float DofCocRadiusPixels( const DofLens& lens, float sceneDistanceCm, uint32_t outputWidth,
                                            float maxRadiusPixels );
    // The largest CoC radius in output pixels: MaxBokehPercent of the output width.
    [[nodiscard]] float DofMaxRadiusPixels( const DepthOfFieldSettings& settings, ViewExtent output );

    // Reversed-Z device depth -> distance along the view axis in cm, through the unjittered inverse projection
    // (the shaders' twin reads DofParams::DepthToView). Device depth 0 under an infinite far plane is +inf.
    [[nodiscard]] glm::vec4 DofDepthToView( const glm::mat4& invProjection );
    [[nodiscard]] float     DofSceneDistanceCm( float deviceDepth, glm::vec4 depthToView );

    // How many tiles each side TileDilate reaches: the largest half-res radius in tiles, rounded up.
    [[nodiscard]] int DofDilateTiles( float maxRadiusOutputPixels );

    // Whether the six nodes are added: a focus distance, an f-stop, a sensor, a ring count, a bokeh limit, a
    // focus beyond the focal length (a thin lens cannot focus nearer than f) and a non-empty split.
    [[nodiscard]] bool DepthOfFieldRuns( const DepthOfFieldSettings& settings, const ViewFrame& frame );

    [[nodiscard]] ViewExtent    DofHalfExtent( ViewExtent output );
    [[nodiscard]] RDG::Extent3D DofTileExtent( ViewExtent output );

    // The parameters every DOF pass reads (one buffer, binding "DepthOfFieldBuffer" in all six shaders).
    struct DofParams
    {
        glm::vec2 OutputSize;
        glm::vec2 RenderSize;
        glm::vec2 HalfSize;
        glm::vec2 TileCount;
        glm::vec4 DepthToView;            // DofDepthToView
        float     FocalDistanceMm = 0.0f; // s
        float     FocalLengthMm   = 0.0f; // f
        float     CocScale        = 0.0f; // A f / 2 * OutputWidth / SensorWidth: output-pixel radius numerator
        float     MaxRadius       = 0.0f; // output pixels
        int32_t   Rings           = 0;
        int32_t   DilateTiles     = 0;
        float     Pad[2]          = {};
    };
    static_assert( offsetof( DofParams, RenderSize ) == 8 );
    static_assert( offsetof( DofParams, HalfSize ) == 16 );
    static_assert( offsetof( DofParams, TileCount ) == 24 );
    static_assert( offsetof( DofParams, DepthToView ) == 32 );
    static_assert( offsetof( DofParams, FocalDistanceMm ) == 48 );
    static_assert( offsetof( DofParams, FocalLengthMm ) == 52 );
    static_assert( offsetof( DofParams, CocScale ) == 56 );
    static_assert( offsetof( DofParams, MaxRadius ) == 60 );
    static_assert( offsetof( DofParams, Rings ) == 64 );
    static_assert( offsetof( DofParams, DilateTiles ) == 68 );
    static_assert( sizeof( DofParams ) == 80 );

    [[nodiscard]] DofParams MakeDofParams( const DepthOfFieldSettings& settings, const ViewFrame& frame );

    // The binding layouts of the six shaders as the graph sees them; the ShaderCacheKey suite pins each against
    // the compiled shader's reflection.
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& DofSetupLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& DofTileFlattenLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& DofTileDilateLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& DofGatherForegroundLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& DofGatherBackgroundLayout();
    [[nodiscard]] const std::shared_ptr<const RDG::ShaderBindingLayout>& DofRecombineLayout();

    struct DepthOfFieldInputs
    {
        RDG::TextureRef SceneColor; // linear HDR, OutputExtent (the resolved colour)
        RDG::TextureRef SceneDepth; // reversed-Z device depth, RenderExtent, single-sample
    };

    class DepthOfField
    {
    public:
        DepthOfField();
        ~DepthOfField();
        DepthOfField( const DepthOfField& )            = delete;
        DepthOfField& operator=( const DepthOfField& ) = delete;

        // Adds "DepthOfField: Setup", "DepthOfField: TileFlatten", "DepthOfField: TileDilate",
        // "DepthOfField: GatherForeground", "DepthOfField: GatherBackground", "DepthOfField: Recombine" and
        // returns the defocused colour (OutputExtent, RGBA16F). An error names what is missing or why it cannot
        // run; DepthOfFieldRuns false is the caller's "do not call".
        [[nodiscard]] Common::ResultStr<RDG::TextureRef> AddPasses( RDG::Builder& graph, const ViewFrame& frame,
                                                                    const DepthOfFieldSettings& settings,
                                                                    const DepthOfFieldInputs&   inputs ) const;

    private:
        enum class Kernel : uint32_t
        {
            Setup,
            TileFlatten,
            TileDilate,
            GatherForeground,
            GatherBackground,
            Recombine,
            Count
        };
        std::shared_ptr<ComputePipeline> PipelineFor( Kernel kernel, std::string& error ) const;

        mutable std::mutex m_PipelineMutex;
        mutable std::array<std::shared_ptr<ComputePipeline>, static_cast<std::size_t>( Kernel::Count )>
             m_Pipelines;
    };
} // namespace Desert::Graphic
