#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>
#include <Engine/Graphic/GlobalIllumination/WorldRadianceCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>

// THE SCREEN PROBE GATHER (GI-FINAL-GATHER) — the final gather of the traced GI methods, ported from UE Lumen's
// LumenScreenProbeGather.cpp / LumenScreenProbeTracing.usf / LumenScreenProbeFiltering.usf /
// LumenScreenProbeGather.usf / LumenScreenSpaceBentNormal.usf. Seven compute passes a frame
// (Programs/GlobalIllumination/ScreenProbe*.shader; the GPU records are Common/ScreenProbes.glslh):
//   Place ×3   a uniform probe in every 16-pixel tile, at the frame's jitter (UE GetScreenTileJitter: Hammersley
//              over 8 frames), then adaptive probes on the 8 and 4 pixel grids at the same jitter wherever the
//              probes already placed cannot interpolate the pixel (UE ScreenProbeAdaptivePlacementCS), up to the
//              scalability's share of the uniform count (GlobalIllumination.AdaptiveProbeAllocationPercent, UE
//              r.Lumen.ScreenProbeGather.AdaptiveProbeAllocationFraction: 100 % at Epic, 50 % below). An adaptive
//              probe is listed under the uniform tile it falls in (UE ScreenTileAdaptiveProbeHeader / Indices),
//              and a pixel whose uniform corner misses it takes the best adaptive probe of that corner's tile (UE
//              CalculateUpsampleInterpolationWeights).
//   Trace      8x8 rays per probe, each in UE's order of legs (LumenScreenProbeTracing.usf): the SCREEN over the
//              view's HZB, a hit returning last frame's lit scene (ScreenProbeTraceScreenTexturesCS); then, from
//              where the screen trace left off, the GI ray contract (TracerVariant / DeclareTracerReads: the pass
//              names no tracer resource) out to the world radiance cache's minimum trace distance - for the
//              software tracer the mesh SDFs of the view's near-field froxel grid first
//              (ScreenProbeTraceMeshSDFsCS over LumenMeshSDFCulling's grid,
//              GlobalDistanceField::AddNearFieldCullPasses), then the global field; past it the cache's radiance
//              is the far field (RadianceCacheRadianceAt).
//   Filter     spatial filter across neighbouring probes with UE's hit-angle test; the filtered radiance (rough
//              reflections) and its second-order SH irradiance (diffuse); the irradiance from behind the probe's
//              surface, read from the world radiance cache at -N (a hemisphere probe holds nothing there).
//   ShortRangeAO  per pixel: a bent normal and its AO from four cosine rays marched through the scene depth over
//              2 % of the screen (UE ScreenSpaceShortRangeAOCS, r.Lumen.ScreenProbeGather.ShortRangeAO): the
//              occluders between the pixel and its probes, a tile away, which the probes cannot see.
//   Integrate  per pixel: the four uniform corners by plane and normal, each missed corner replaced by its
//              tile's best adaptive probe; diffuse from the SH at the bent normal times the short-range AO, rough
//              specular from GGX samples of the probes' radiance each weighted by its visibility in the bent cone
//              (UE CalculateSpecularOcclusion), blended with the reprojected history. The composite therefore
//              applies no SSAO to the gather (UE r.Lumen.DiffuseIndirect.SSAO 0). Only a pixel no probe holds
//              after all that (the adaptive budget spent) reads the world radiance cache.
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
} // namespace Desert::Graphic

namespace Desert::Graphic::GlobalIllumination
{
    // std430 — must match `struct ScreenProbeParams` in Common/ScreenProbes.glslh. 112 bytes.
    struct ScreenProbeParamsPayload
    {
        glm::mat4  PrevViewProjection{ 1.0f };
        glm::vec4  PrevCamera{ 0.0f };
        glm::uvec4 Grid{ 0u };  // xy = level-0 grid, z = uniform probes, w = adaptive probe budget
        glm::uvec4 Atlas{ 0u }; // x = probes per atlas row, y = frame index, z = kScreenProbe* flags below,
                                // w = the frame's probe jitter in pixels (x | y << 16, ScreenProbeJitter)
    };
    static_assert( sizeof( ScreenProbeParamsPayload ) == 112 );
    // ScreenProbeParamsPayload::Atlas.z — must match SP_FLAG_* in Common/ScreenProbes.glslh.
    inline constexpr uint32_t kScreenProbeHistoryValid = 1u; // the history is this view's
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
        static constexpr uint32_t kDownsample     = 16; // UE r.Lumen.ScreenProbeGather.DownsampleFactor
        static constexpr uint32_t kLevels         = 3;  // uniform 16 px + adaptive 8 px + adaptive 4 px
        // The adaptive positions one uniform tile holds: 2x2 - 1 at 8 px and 4x4 - 2x2 at 4 px (the positions of
        // a coarser level are skipped, UE `any((DispatchThreadId.xy & 0x1) != 0)`).
        static constexpr uint32_t kAdaptivePerTile = 3 + 12;
        static constexpr uint32_t kTracedTexels   = 8;  // UE r.Lumen.ScreenProbeGather.TracingOctahedronResolution
        static constexpr uint32_t kAtlasProbesRow = 128;
        static constexpr uint32_t kSHCoefficients  = 9;

        glm::uvec2 Grid{ 0u }; // level-0 tiles
        uint32_t   UniformProbes  = 0;
        uint32_t   AdaptiveProbes = 0; // the budget
        // 1 (the adaptive count) + the uniform grid + per uniform tile: one adaptive count per adaptive level and
        // kAdaptivePerTile adaptive probe slots (Common/ScreenProbes.glslh SPTileHeader / SPTileSlot).
        uint32_t   IndexEntries   = 0;
        uint32_t   AtlasRows      = 0;

        [[nodiscard]] uint32_t Probes() const
        {
            return UniformProbes + AdaptiveProbes;
        }
    };

    // @p adaptivePercent: adaptive probes per uniform probe, percent (Scalability
    // GlobalIllumination.AdaptiveProbeAllocationPercent, UE AdaptiveProbeAllocationFraction x 100).
    [[nodiscard]] inline ScreenProbeLayout MakeScreenProbeLayout( const glm::uvec2 extent,
                                                                  const uint32_t   adaptivePercent )
    {
        ScreenProbeLayout layout;
        const auto        grid = [extent]( const uint32_t spacing )
        { return glm::uvec2( ( extent.x + spacing - 1u ) / spacing, ( extent.y + spacing - 1u ) / spacing ); };
        layout.Grid           = grid( ScreenProbeLayout::kDownsample );
        layout.UniformProbes  = layout.Grid.x * layout.Grid.y;
        layout.AdaptiveProbes = layout.UniformProbes * adaptivePercent / 100u;
        layout.IndexEntries   = 1u + layout.UniformProbes * ( 1u + ( ScreenProbeLayout::kLevels - 1u ) +
                                                            ScreenProbeLayout::kAdaptivePerTile );
        layout.AtlasRows =
             ( layout.Probes() + ScreenProbeLayout::kAtlasProbesRow - 1u ) / ScreenProbeLayout::kAtlasProbesRow;
        return layout;
    }

    // The probes' pixel offset inside their tiles in frame @p frameIndex (UE GetScreenTileJitter:
    // Hammersley16(TemporalIndex % 8, 8) * DownsampleFactor) — every level's grid is shifted by it, so over eight
    // frames the probes visit eight positions of each tile and the integrate's history gathers them.
    [[nodiscard]] inline glm::uvec2 ScreenProbeJitter( const uint32_t frameIndex )
    {
        const uint32_t i        = frameIndex % 8u;
        uint32_t       reversed = 0u; // the 3-bit reversal of i: Hammersley's radical inverse over 8 samples
        for ( uint32_t bit = 0; bit < 3u; ++bit )
            reversed |= ( ( i >> bit ) & 1u ) << ( 2u - bit );
        return glm::uvec2( i, reversed ) * ( ScreenProbeLayout::kDownsample / 8u );
    }

    // This frame's graph resources the gather reads.
    struct ScreenProbeGatherInputs
    {
        RDG::TextureRef    GBufferDepth;
        RDG::TextureRef    GBufferA; // albedo
        RDG::TextureRef    GBufferB; // normal
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
        bool     ShortRangeAO = true; // Scalability GlobalIllumination.ShortRangeAO: the ShortRangeAO pass runs
        glm::mat4          PrevViewProjection{ 1.0f }; // unjittered
        glm::vec3          PrevCameraPosition{ 0.0f };
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

        // The tracer-independent shaders and pipelines; Trace's is acquired per tracer permutation.
        [[nodiscard]] Common::BoolResultStr Initialize();

        // The gather of one frame, its seven passes added to @p graph after the radiance cache's update.
        [[nodiscard]] Common::ResultStr<ScreenProbeGatherFrame>
        AddPasses( RDG::Builder& graph, const WorldRadianceCache::ImportImage& import,
                   const ScreenProbeGatherInputs& inputs );

    private:
        enum Program : uint32_t
        {
            Place = 0,
            ShortRangeAO,
            Trace,
            Filter,
            Integrate,
            ProgramCount
        };

        [[nodiscard]] Common::BoolResultStr EnsureImages( glm::uvec2 extent, const ScreenProbeLayout& layout );
        [[nodiscard]] Common::BoolResultStr EnsureTraceProgram( const Pipeline& pipeline );

        std::array<std::shared_ptr<Shader>, ProgramCount>          m_Shaders;
        std::array<std::shared_ptr<ComputePipeline>, ProgramCount> m_Pipelines;
        mutable std::array<ShaderBindingLayoutCache, ProgramCount> m_Layouts;
        Pipeline                                                   m_TraceProgramFor{};

        std::shared_ptr<Image2D>                m_Traced;   // SP_RES² texels per probe
        std::shared_ptr<Image2D>                m_Filtered; // (SP_RES + 2)² per probe
        std::shared_ptr<Image2D>                m_ShortRangeAO; // xyz = bent normal, w = AO, the G-buffer's extent
        std::array<std::shared_ptr<Image2D>, 2> m_Diffuse;  // this frame's output / last frame's history
        std::array<std::shared_ptr<Image2D>, 2> m_Specular;
        std::array<std::shared_ptr<Image2D>, 2> m_Backface;
        glm::uvec2                              m_Extent{ 0u };
        uint32_t                                m_AtlasRows    = 0;
        uint32_t                                m_Current      = 0;
        uint32_t                                m_FrameIndex   = 0;
        bool                                    m_HistoryValid = false;
    };
} // namespace Desert::Graphic::GlobalIllumination
