#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>
#include <Engine/Graphic/GlobalIllumination/WorldRadianceCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

// THE SCREEN PROBE GATHER (GI-FINAL-GATHER) — the final gather of the traced GI methods, ported from UE Lumen's
// LumenScreenProbeGather.cpp / LumenScreenProbeTracing.usf / LumenScreenProbeFiltering.usf /
// LumenScreenProbeGather.usf / LumenScreenSpaceBentNormal.usf / LumenScreenProbeGatherTemporal.usf. Sixteen
// compute passes a frame, eleven when the denoiser is skipped (Programs/GlobalIllumination/ScreenProbe*.shader;
// the GPU records are Common/ScreenProbes.glslh):
//   Place ×3   a uniform probe in every 16-pixel tile, at the frame's jitter (UE GetScreenTileJitter: Hammersley
//              over 8 frames), then adaptive probes on the 8 and 4 pixel grids at the same jitter wherever the
//              probes already placed cannot interpolate the pixel (UE ScreenProbeAdaptivePlacementCS), up to the
//              scalability's share of the uniform count (GlobalIllumination.AdaptiveProbeAllocationPercent, UE
//              r.Lumen.ScreenProbeGather.AdaptiveProbeAllocationFraction: 100 % at Epic, 50 % below). An adaptive
//              probe is listed under the uniform tile it falls in (UE ScreenTileAdaptiveProbeHeader / Indices),
//              and a pixel whose uniform corner misses it takes the best adaptive probe of that corner's tile (UE
//              CalculateUpsampleInterpolationWeights).
//   Trace ×5   8x8 rays per probe, in UE's passes (LumenScreenProbeTracing.cpp TraceScreenProbes): the SCREEN over
//              the view's HZB, a hit returning last frame's lit scene (ScreenProbeTraceScreenTexturesCS); the rays
//              it left open COMPACTED into a dense list with its indirect arguments (ScreenProbeCompactTracesCS);
//              from where the screen left off, the GI ray contract's NEAR field over that list (TracerVariant /
//              DeclareTracerReads: the passes name no tracer resource; software: the mesh SDFs of the view's
//              near-field froxel grid, ScreenProbeTraceMeshSDFsCS over LumenMeshSDFCulling's grid,
//              GlobalDistanceField::AddNearFieldCullPasses); every ray still without a hit compacted again; its
//              FAR field (software: the global field) out to the world radiance cache's minimum trace distance,
//              past which the cache's radiance answers (RadianceCacheRadianceAt; ScreenProbeTraceVoxelsCS). The
//              legs hand one another each ray's state in a trace hit buffer (Common/ScreenProbeTracing.glslh).
//   Disocclusion  per probe: the share of its tile's pixels whose history holds few frames (UE
//              SpatialFilter.DisocclusionMaxFrames / DisocclusionFraction): the spatial filter relaxes there.
//   SpatialFilter ×3  the probes' radiance filtered across neighbouring probes (UE
//   ScreenProbeFilterGatherTracesCS,
//              SpatialFilterNumPasses): weighted by the plane distance and the hit angle seen from the probe.
//   Filter     the filtered radiance with its border (rough reflections) and its second-order SH irradiance
//              (diffuse); the irradiance from behind the probe's surface, read from the world radiance cache at -N
//              (a hemisphere probe holds nothing there).
//   ShortRangeAO  per pixel: a bent normal and its AO from four cosine rays marched through the scene depth over
//              2 % of the screen (UE ScreenSpaceShortRangeAOCS, r.Lumen.ScreenProbeGather.ShortRangeAO): the
//              occluders between the pixel and its probes, a tile away, which the probes cannot see.
//   Integrate  per pixel: the four uniform corners by plane and normal, each missed corner replaced by its
//              tile's best adaptive probe; diffuse from the SH at the bent normal times the short-range AO, rough
//              specular from GGX samples of the probes' radiance each weighted by its visibility in the bent cone
//              (UE CalculateSpecularOcclusion). The composite therefore applies no SSAO to the gather (UE
//              r.Lumen.DiffuseIndirect.SSAO 0). Only a pixel no probe holds after all that (the adaptive budget
//              spent) reads the world radiance cache.
//   Temporal   per pixel: the integrated diffuse, rough specular and backface diffuse blended with the view's
//              history (SceneViewState ScreenProbeGatherHistory, UE FScreenProbeGatherTemporalState) reprojected
//              by the velocity, each bilinear tap kept by a depth and a normal test, by 1 / (1 + frames
//              accumulated) up to ten (UE ScreenProbeTemporalReprojectionCS): a disoccluded pixel restarts and
//              converges in a few frames. A camera cut, a teleport or a resize is the view's HistoryReset: no tap
//              is kept.
// THE DENOISER (Disocclusion, SpatialFilter, Temporal) runs unless ScreenProbeGatherInputs::Denoise is false: a
// temporal method that denoises the raw signals itself (DLSS Ray Reconstruction) takes the integrate's output.
// The deferred composite reads the two results per pixel (one fetch each) in place of the sky's diffuse
// irradiance and prefiltered radiance: no surface of a traced method reads the sky cube.
//
// WHERE THIS DIFFERS FROM UE (for the owner):
//   * The probe is a hemi-octahedron around its pixel's normal, not a world-space full octahedron: every ray
//     leaves the surface (UE spends the lower half on BRDF importance sampling). Neighbours meet by direction,
//     each in its own frame, in the filter and the integrate.
//   * A screen hit is reprojected by the camera only (UE GetHistoryScreenPosition); UE also rejects it against
//     last frame's depth (HistorySceneDepth), which the view does not keep.
//   * Irradiance SH per probe rather than UE's default octahedral irradiance: nine coefficients, one evaluation
//     per probe and pixel.
//   * Smooth surfaces read the probes' radiance too (UE traces them separately in Lumen Reflections): until a
//     reflection tracer exists, a smooth floor in a closed room reflects the room's light, not the sky cube.
namespace Desert::Graphic
{
    class Image;
    class Image2D;
    class Shader;
    class ComputePipeline;
    class ScreenProbeGatherHistory; // Graphic/View/SceneViewState.hpp
} // namespace Desert::Graphic

namespace Desert::Graphic::GlobalIllumination
{
    // std430 — must match `struct ScreenProbeParams` in Common/ScreenProbes.glslh. 192 bytes.
    struct ScreenProbeParamsPayload
    {
        glm::mat4  PrevViewProjection{ 1.0f };
        glm::mat4  PrevInvViewProjection{ 1.0f }; // the temporal filter's previous world position of a pixel
        glm::vec4  PrevCamera{ 0.0f };
        glm::vec4  Jitter{ 0.0f }; // xy = this frame's jitter in NDC, zw = last frame's
        glm::uvec4 Grid{ 0u };  // xy = level-0 grid, z = uniform probes, w = adaptive probe budget
        glm::uvec4 Atlas{ 0u }; // x = probes per atlas row, y = frame index, z = kScreenProbe* flags below,
                                // w = the frame's probe jitter in pixels (x | y << 16, ScreenProbeJitter)
    };
    static_assert( sizeof( ScreenProbeParamsPayload ) == 192 );
    // ScreenProbeParamsPayload::Atlas.z — must match SP_FLAG_* in Common/ScreenProbes.glslh.
    inline constexpr uint32_t kScreenProbeHistoryValid = 1u; // the temporal history is the view's previous frame
    inline constexpr uint32_t kScreenProbeShortRangeAO = 2u; // the ShortRangeAO pass ran: the integrate reads it
    inline constexpr uint32_t kScreenProbeScreenTrace  = 4u; // last frame's scene colour is readable: screen leg

    // std430 — must match `struct ScreenProbe` in Common/ScreenProbes.glslh. 32 bytes.
    struct ScreenProbePayload
    {
        glm::vec4 Position{ 0.0f };
        glm::vec4 Normal{ 0.0f };
    };
    static_assert( sizeof( ScreenProbePayload ) == 32 );

    // The probe layout of an extent (pure; Common/ScreenProbes.glslh computes the same on the GPU).
    struct ScreenProbeLayout
    {
        // The narrowest atlas row, in probes; MakeScreenProbeLayout widens it until the atlas height fits the
        // device's image limit (AtlasProbesRow).
        static constexpr uint32_t kMinAtlasProbesRow = 128;
        static constexpr uint32_t kSHCoefficients  = 9;
        static constexpr uint32_t kMaxLevels       = 5; // 64 px: uniform + adaptive 32, 16, 8, 4

        // UE r.Lumen.ScreenProbeGather.DownsampleFactor (Scalability GlobalIllumination.ScreenProbeDownsample):
        // 8, 16, 32 or 64 pixels a uniform probe side; the shaders' SP_DOWNSAMPLE (ScreenProbeLayoutDefines).
        uint32_t Downsample = 16;
        // Uniform + adaptive levels down to 4 px (UE AdaptiveProbeMinDownsampleFactor 4): log2(Downsample) - 1.
        uint32_t Levels = 3;
        // The adaptive positions one uniform tile holds, the coarser levels' skipped (UE
        // `any((DispatchThreadId.xy & 0x1) != 0)`): (Downsample / 4)² - 1, 15 at 16 px.
        uint32_t AdaptivePerTile = 15;
        // UE r.Lumen.ScreenProbeGather.TracingOctahedronResolution (Scalability
        // GlobalIllumination.ScreenProbeTracedTexels), 4..16: the shaders' SP_RES.
        uint32_t TracedTexels = 8;

        glm::uvec2 Grid{ 0u }; // level-0 tiles
        uint32_t   UniformProbes  = 0;
        uint32_t   AdaptiveProbes = 0; // the budget
        // 1 (the adaptive count) + the uniform grid + per uniform tile: one adaptive count per adaptive level and
        // AdaptivePerTile adaptive probe slots (Common/ScreenProbes.glslh SPTileHeader / SPTileSlot).
        uint32_t   IndexEntries   = 0;
        // Probes a traced / filtered atlas row (the shaders' screenProbeParams.atlas.x) and the rows: at least
        // kMinAtlasProbesRow, widened so AtlasRows * (TracedTexels + 2) fits the device's 2D image limit.
        uint32_t   AtlasProbesRow = kMinAtlasProbesRow;
        uint32_t   AtlasRows      = 0;

        [[nodiscard]] uint32_t Probes() const
        {
            return UniformProbes + AdaptiveProbes;
        }
    };

    // Whether @p downsample / @p tracedTexels are a layout the shaders compile (SP_DOWNSAMPLE_LOG2 3..6,
    // SP_TRACED_TEXELS 4..16 in Common/ScreenProbes.glslh).
    [[nodiscard]] constexpr bool ScreenProbeLayoutSupported( const uint32_t downsample, const uint32_t tracedTexels )
    {
        return ( downsample == 8u || downsample == 16u || downsample == 32u || downsample == 64u ) &&
               tracedTexels >= 4u && tracedTexels <= 16u;
    }

    // @p adaptivePercent: adaptive probes per uniform probe, percent (Scalability
    // GlobalIllumination.AdaptiveProbeAllocationPercent, UE AdaptiveProbeAllocationFraction x 100);
    // @p downsample / @p tracedTexels: ScreenProbeLayoutSupported; @p maxImageDimension: the device's
    // maxImageDimension2D (DeviceCapabilities::MaxTexture2DSize), which bounds both atlas sides.
    //
    // THE IMAGE-LIMIT CLAMP. The widest atlas texel a probe takes is the filtered one, side = TracedTexels + 2, so
    // the atlas holds at most C = (maxImageDimension / side)² probes. The row is widened first:
    // AtlasProbesRow = max(kMinAtlasProbesRow, ceil(Probes / (maxImageDimension / side))), capped at
    // maxImageDimension / side. If the probes still exceed C, the ADAPTIVE budget is clamped to C - UniformProbes
    // (the layout permutation, i.e. Downsample / TracedTexels the programs are compiled under, is never changed);
    // a uniform grid alone over C is a layout the device cannot hold: AtlasRows = 0, ScreenProbeLayoutFits false,
    // ScreenProbeGather::AddPasses refuses it with the numbers.
    [[nodiscard]] inline ScreenProbeLayout MakeScreenProbeLayout( const glm::uvec2 extent,
                                                                  const uint32_t   adaptivePercent,
                                                                  const uint32_t   downsample,
                                                                  const uint32_t   tracedTexels,
                                                                  const uint32_t   maxImageDimension )
    {
        ScreenProbeLayout layout;
        layout.Downsample = downsample;
        layout.Levels     = static_cast<uint32_t>( std::countr_zero( downsample ) ) - 1u;
        layout.AdaptivePerTile = ( downsample / 4u ) * ( downsample / 4u ) - 1u;
        layout.TracedTexels    = tracedTexels;
        const auto        grid = [extent]( const uint32_t spacing )
        { return glm::uvec2( ( extent.x + spacing - 1u ) / spacing, ( extent.y + spacing - 1u ) / spacing ); };
        layout.Grid           = grid( downsample );
        layout.UniformProbes  = layout.Grid.x * layout.Grid.y;
        layout.AdaptiveProbes = layout.UniformProbes * adaptivePercent / 100u;
        layout.IndexEntries   = 1u + layout.UniformProbes * ( 1u + ( layout.Levels - 1u ) + layout.AdaptivePerTile );
        const uint32_t side    = tracedTexels + 2u;
        const uint32_t maxSide = maxImageDimension / side; // probes along one atlas side
        const uint64_t capacity = static_cast<uint64_t>( maxSide ) * maxSide;
        if ( layout.UniformProbes > capacity )
        {
            layout.AtlasRows = 0; // ScreenProbeLayoutFits false
            return layout;
        }
        if ( layout.Probes() > capacity )
            layout.AdaptiveProbes = static_cast<uint32_t>( capacity ) - layout.UniformProbes;
        const uint32_t probes = layout.Probes();
        layout.AtlasProbesRow =
             std::min( maxSide, std::max( ScreenProbeLayout::kMinAtlasProbesRow, ( probes + maxSide - 1u ) / maxSide ) );
        layout.AtlasRows = ( probes + layout.AtlasProbesRow - 1u ) / layout.AtlasProbesRow;
        return layout;
    }

    // Whether MakeScreenProbeLayout found an atlas the device holds (its uniform grid fits the image limit).
    [[nodiscard]] inline bool ScreenProbeLayoutFits( const ScreenProbeLayout& layout )
    {
        return layout.AtlasRows != 0u;
    }

    // The probes' pixel offset inside their tiles in frame @p frameIndex (UE GetScreenTileJitter:
    // Hammersley16(TemporalIndex % 8, 8) * DownsampleFactor) — every level's grid is shifted by it, so over eight
    // frames the probes visit eight positions of each tile and the integrate's history gathers them.
    [[nodiscard]] inline glm::uvec2 ScreenProbeJitter( const uint32_t frameIndex, const uint32_t downsample )
    {
        const uint32_t i        = frameIndex % 8u;
        uint32_t       reversed = 0u; // the 3-bit reversal of i: Hammersley's radical inverse over 8 samples
        for ( uint32_t bit = 0; bit < 3u; ++bit )
            reversed |= ( ( i >> bit ) & 1u ) << ( 2u - bit );
        return glm::uvec2( i, reversed ) * ( downsample / 8u );
    }

    // This frame's graph resources the gather reads.
    struct ScreenProbeGatherInputs
    {
        RDG::TextureRef    GBufferDepth;
        RDG::TextureRef    GBufferA; // albedo
        RDG::TextureRef    GBufferB; // normal
        RDG::TextureRef    GBufferShadingWord; // the temporal filter's foliage test (DesertHasBackfaceDiffuse)
        RDG::TextureRef    GBufferEmissive;
        RDG::TextureRef    EnvSpecular;  // the sky's radiance cube (or the system black cube)
        TracerFrame        Tracer;       // the active tracer and its reads (GITracing.hpp)
        SurfaceCacheFrame  SurfaceCache; // the surface cache a hit reads first (GITracing.hpp)
        RDG::BufferRef     PointLights;
        RDG::BufferRef     SpotLights;
        RDG::BufferRef     WorldGridParams;
        RDG::BufferRef     WorldGridCells;
        RDG::BufferRef     GIFrame; // GIFramePayload, "GIFrameUB"
        RadianceCacheFrame RadianceCache;
        glm::uvec2         Extent{ 0u };
        uint32_t AdaptiveProbePercent = 0; // Scalability GlobalIllumination.AdaptiveProbeAllocationPercent
        uint32_t Downsample   = 16; // Scalability GlobalIllumination.ScreenProbeDownsample (ScreenProbeLayout)
        uint32_t TracedTexels = 8;  // Scalability GlobalIllumination.ScreenProbeTracedTexels (ScreenProbeLayout)
        bool     ShortRangeAO = true; // Scalability GlobalIllumination.ShortRangeAO: the ShortRangeAO pass runs
        glm::mat4          PrevViewProjection{ 1.0f }; // unjittered
        glm::vec3          PrevCameraPosition{ 0.0f };
        // ViewFrame::PrevPreExposure: the scale the scene colour history (the screen trace's hit radiance) is
        // stored at; the trace divides by it so its output stays absolute.
        float PrevPreExposure = 1.0f;
        // The temporal filter's reprojection: the view's velocity (FrameTransients::Velocity), this frame's and
        // last frame's jitter (ViewFrame::JitterNdc / PrevJitterNdc), and whether the view's previous frame is a
        // previous frame of this one (ViewFrame::HistoryValid: false after a cut, a teleport, a resize).
        RDG::TextureRef Velocity;
        glm::vec2       JitterNdc{ 0.0f };
        glm::vec2       PrevJitterNdc{ 0.0f };
        bool            ViewHistoryValid = false;
        RDG::TextureRef Black; // the system black texture: the history's binding when the view has none
        // The gather's denoiser (disocclusion, spatial filter, temporal filter) runs. False only where the view's
        // temporal method denoises the raw signals itself (DLSS Ray Reconstruction): the one decision point is
        // SceneRenderer's, this flag carries it.
        bool Denoise = true;
        // The screen trace's (SceneRenderer::FrameScreenTrace): this frame's closest-depth HZB and the view's
        // scene colour history; the leg is skipped when either is invalid or the history is not last frame's.
        RDG::TextureRef HZB;
        RDG::TextureRef SceneColorHistory;
        bool            SceneColorHistoryReadable = false;
    };

    // What the deferred composite reads (rgba16f, the G-buffer's extent).
    struct ScreenProbeGatherFrame
    {
        RDG::TextureRef
             Diffuse; // rgb = irradiance / pi, a = distance to the camera — "u_GIDiffuse" in the composite
        RDG::TextureRef Specular; // rgb = radiance in the reflection direction — "u_GISpecular" in the composite
        RDG::TextureRef Backface; // rgb = irradiance / pi from -N (a two-sided leaf's) — "u_GIBackface"
        bool ShortRangeAO = false; // Diffuse and Specular carry the short-range AO: the composite applies no SSAO
    };

    class ScreenProbeGather
    {
    public:
        ScreenProbeGather();
        ~ScreenProbeGather();
        ScreenProbeGather( const ScreenProbeGather& )            = delete;
        ScreenProbeGather& operator=( const ScreenProbeGather& ) = delete;

        // The tracer-independent shaders and pipelines; the world legs' are acquired per tracer permutation.
        [[nodiscard]] Common::BoolResultStr Initialize();
        // Trace's program under TracerVariant( @p pipeline ), re-acquired when the pipeline changes. Public so the
        // view asks for it first and refuses the GI method of that tracer by name when it does not build
        // (SceneRenderer::AddFrameTracedGI); AddPasses asks again (cached).
        [[nodiscard]] Common::BoolResultStr EnsureTraceProgram( const Pipeline& pipeline );

        // The gather of one frame, its passes added to @p graph after the radiance cache's update. @p history:
        // the view's (SceneViewState::ScreenProbeGather), read and written by the temporal filter.
        [[nodiscard]] Common::ResultStr<ScreenProbeGatherFrame>
        AddPasses( RDG::Builder& graph, const WorldRadianceCache::ImportImage& import,
                   const ScreenProbeGatherInputs& inputs, ScreenProbeGatherHistory& history );

    private:
        enum Program : uint32_t
        {
            Place = 0,
            ShortRangeAO,
            TraceScreen,
            CompactTraces,
            TraceNearField, // ScreenProbeTrace, a tracer permutation (EnsureTraceProgram)
            TraceFarField,  // the same program's far-field leg
            Disocclusion,
            SpatialFilter,
            Filter,
            Integrate,
            Temporal,
            ProgramCount
        };

        [[nodiscard]] Common::BoolResultStr EnsureImages( glm::uvec2 extent, const ScreenProbeLayout& layout );
        // Every program under the layout permutation of @p downsample / @p tracedTexels (rebuilt on a change;
        // the trace legs re-acquired by EnsureTraceProgram).
        [[nodiscard]] Common::BoolResultStr EnsureLayoutPrograms( uint32_t downsample, uint32_t tracedTexels );

        std::array<std::shared_ptr<Shader>, ProgramCount>          m_Shaders;
        std::array<std::shared_ptr<ComputePipeline>, ProgramCount> m_Pipelines;
        mutable std::array<ShaderBindingLayoutCache, ProgramCount> m_Layouts;
        Pipeline                                                   m_TraceProgramFor{};
        uint32_t m_ProgramDownsample   = 0; // the layout the programs are compiled under (0 = none yet)
        uint32_t m_ProgramTracedTexels = 0;
        uint32_t m_TraceProgramDownsample   = 0; // the layout the trace legs are compiled under
        uint32_t m_TraceProgramTracedTexels = 0;

        std::shared_ptr<Image2D>                m_Traced;   // SP_RES² texels per probe
        std::shared_ptr<Image2D>                m_Filtered; // (SP_RES + 2)² per probe
        std::shared_ptr<Image2D>                m_ShortRangeAO; // xyz = bent normal, w = AO, the G-buffer's extent
        glm::uvec2                              m_Extent{ 0u };
        uint32_t                                m_AtlasRows  = 0;
        uint32_t                                m_AtlasProbesRow    = 0;
        uint32_t                                m_ImageTracedTexels = 0;
        uint32_t                                m_FrameIndex = 0;
    };
} // namespace Desert::Graphic::GlobalIllumination
