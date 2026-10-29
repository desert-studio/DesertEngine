#pragma once

#include <Common/Settings/RenderMethods.hpp>
#include <Common/Settings/Scalability.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// THE WORLD RADIANCE CACHE — the interface (GI-CACHE-IFACE). Port of UE Lumen's radiance cache:
// LumenRadianceCache.h (FRadianceCacheInputs, FRadianceCacheInterpolationParameters), LumenRadianceCache.cpp /
// .usf (the update), LumenRadianceCacheInterpolation.ush (the GPU read side), LumenRadianceCacheMarkCommon.ush
// (marking).
//
// WHAT IT IS. Clipmaps of world-aligned probe grids around the camera (clipmap i covers ClipmapWorldExtent ×
// DistributionBase^i, GridResolution³ cells each). A cell holds a probe only where something needs one: the
// screen probes of the final gather (and, through the GI-HWRT/SW tracers, long rays that left the screen) MARK the
// cells they will interpolate, and only marked cells get an atlas slot — sparse, through a per-clipmap INDIRECTION
// texture of probe indices. Each frame a budget of probes (ProbesPerFrame) is traced, highest priority first; a
// probe's rays are lit at their hits by the one shared hit function (Common/GITracing.glslh: surface cache, hit
// lighting outside coverage, sky on a miss), written as octahedral radiance, then filtered to irradiance.
//
// THE UPDATE, pass by pass (UE names and files, so the port is checkable side by side):
//   1. MarkRadianceProbesUsedByScreenProbes (LumenScreenProbeGather.usf): "used" into the indirection.
//   2. UpdateCacheForUsedProbes (LumenRadianceCacheUpdate.usf): keep last frame's slots of used probes, free the
//      rest (the clipmap scrolled or nothing reads them).
//   3. AllocateUsedProbes (LumenRadianceCacheUpdate.usf): slots for newly used probes.
//   4. SelectMaxPriorityBucket (LumenRadianceCacheUpdate.usf): the ProbesPerFrame most urgent probes.
//   5. SetupTraceFromProbes / TraceFromProbes (LumenRadianceCache.usf): the tracer, then GIHitRadiance.
//   6. Filter the radiance, then the irradiance and sky-visibility atlases the final gather samples.
//
// WHERE THIS DIFFERS FROM UE (for the owner):
//   * Priority also rises with a LIGHT CHANGE in the probe's cell (the world layout of the light grid,
//     LIGHT-CLUSTER-WORLD, hashes the lights touching a cell): a lamp switched on re-traces its probes the next
//     frame. UE ranks by age and use only, so an indoor lamp change shows up over ~a second.
namespace Desert::Graphic::RadianceCache
{
    // ---- Fixed shape: UE CVar defaults that BaseScalability does not vary ----------------------------------
    // RADIANCE_PROBE_MAX_CLIPMAPS.
    inline constexpr uint32_t kMaxClipmaps = 6;
    // r.Lumen.ScreenProbeGather.RadianceCache.NumClipmaps.
    inline constexpr uint32_t kClipmaps = 4;
    // Cells per clipmap side (…RadianceCache.GridResolution).
    inline constexpr uint32_t kGridResolution = 48;
    // Half size of clipmap 0, cm (…RadianceCache.ClipmapWorldExtent).
    inline constexpr float kClipmapWorldExtent = 2500.0f;
    // Extent ratio of clipmap i+1 to clipmap i (…RadianceCache.ClipmapDistributionBase).
    inline constexpr float kClipmapDistributionBase = 2.0f;
    // Probes per atlas side (…RadianceCache.ProbeAtlasResolutionInProbes).
    inline constexpr uint32_t kProbeAtlasResolutionInProbes = 128;
    // Texels around each sampled probe, so bilinear reads stay inside it (one mip).
    inline constexpr uint32_t kProbeBorder = 1;
    // Octahedral irradiance texels per probe side (IRRADIANCE_PROBE_RES).
    inline constexpr uint32_t kIrradianceProbeResolution = 6;
    // …RadianceCache.ReprojectionRadiusScale.
    inline constexpr float kReprojectionRadiusScale = 1.5f;
    // A cell with no probe in the indirection texture.
    inline constexpr uint32_t kInvalidProbeIndex = 0xFFFFFFFFu;
    static_assert( kClipmaps <= kMaxClipmaps );

    // ---- The resources (UE FRadianceCacheState) ---------------------------------------------------------------
    // Indirection: R32_UINT 3D, (GridResolution × Clipmaps, GridResolution, GridResolution) — clipmaps side by
    // side along x; a probe index or kInvalidProbeIndex. Probe atlases: 2D, ProbeAtlasResolutionInProbes probes
    // per side.
    inline constexpr Core::Formats::ImageFormat kIndirectionFormat     = Core::Formats::ImageFormat::R32_UINT;
    inline constexpr Core::Formats::ImageFormat kRadianceAtlasFormat   = Core::Formats::ImageFormat::RGBA16F;
    inline constexpr Core::Formats::ImageFormat kIrradianceAtlasFormat = Core::Formats::ImageFormat::RGBA16F;
    inline constexpr Core::Formats::ImageFormat kSkyVisibilityFormat   = Core::Formats::ImageFormat::R8_UNORM;

    // What one cache is built with (UE FRadianceCacheInputs).
    struct RadianceCacheInputs
    {
        uint32_t Clipmaps                     = kClipmaps;
        uint32_t GridResolution               = kGridResolution;
        float    ClipmapWorldExtent           = kClipmapWorldExtent;
        float    ClipmapDistributionBase      = kClipmapDistributionBase;
        float    ReprojectionRadiusScale      = kReprojectionRadiusScale;
        uint32_t ProbeAtlasResolutionInProbes = kProbeAtlasResolutionInProbes;
        uint32_t IrradianceProbeResolution    = kIrradianceProbeResolution;
        uint32_t ProbeResolution              = 0; // traced octahedral texels per side (Scalability)
        uint32_t ProbesPerFrame               = 0; // probes traced per frame (Scalability)

        // Texels per side of a probe as SAMPLED: the traced resolution plus the border (UE FinalProbeResolution).
        [[nodiscard]] uint32_t FinalProbeResolution() const
        {
            return ProbeResolution + 2u * kProbeBorder;
        }

        bool operator==( const RadianceCacheInputs& ) const = default;
    };

    // PURE. The cache of the machine's quality, for the EFFECTIVE GI method @p method. ProbeVolume reads this same
    // cache per pixel (no screen probes in front of it, IrradianceFieldGather.hpp) on its own update budget,
    // Scalability GlobalIllumination.IrradianceFieldProbesPerFrame (UE r.Lumen.IrradianceFieldGather.
    // NumProbesToTraceBudget, against the screen probes' RadianceCacheProbesPerFrame). The SHAPE is the same for
    // both methods, so switching between them keeps every traced probe (WorldRadianceCache rebuilds only on a
    // shape change).
    [[nodiscard]] inline RadianceCacheInputs
    MakeRadianceCacheInputs( const Common::Scalability::ResolvedQuality& quality,
                             const Common::Scalability::GIMethod         method )
    {
        using P = Common::Scalability::Parameter;
        RadianceCacheInputs inputs;
        inputs.ProbeResolution = quality.As<uint32_t>( P::RadianceCacheProbeResolution );
        P budget = P::RadianceCacheProbesPerFrame;
        if ( method == Common::Scalability::GIMethod::ProbeVolume )
            budget = P::IrradianceFieldProbesPerFrame;
        inputs.ProbesPerFrame = quality.As<uint32_t>( budget );
        return inputs;
    }

    // One clipmap of one frame: its min corner (world, cm) and its cell size (cm).
    struct Clipmap
    {
        glm::vec3 Corner{ 0.0f };
        float     CellSize = 0.0f;

        bool operator==( const Clipmap& ) const = default;
    };

    // PURE. The clipmaps around @p cameraPosition (UE UpdateRadianceCacheState): clipmap i has half size
    // ClipmapWorldExtent × DistributionBase^i and is snapped to WHOLE CELLS of its own size, so a probe stays at
    // one world position while the camera moves and the history of every cell that remains is valid as is.
    [[nodiscard]] inline std::array<Clipmap, kMaxClipmaps> MakeClipmaps( const glm::vec3&           cameraPosition,
                                                                         const RadianceCacheInputs& inputs )
    {
        std::array<Clipmap, kMaxClipmaps> clipmaps{};
        const uint32_t                    count = std::min( inputs.Clipmaps, kMaxClipmaps );
        for ( uint32_t i = 0; i < count; ++i )
        {
            const float extent =
                 inputs.ClipmapWorldExtent * std::pow( inputs.ClipmapDistributionBase, static_cast<float>( i ) );
            const float     cell    = 2.0f * extent / static_cast<float>( inputs.GridResolution );
            const glm::vec3 snapped = glm::floor( cameraPosition / cell + 0.5f ) * cell;
            clipmaps[i].Corner      = snapped - glm::vec3( extent );
            clipmaps[i].CellSize    = cell;
        }
        return clipmaps;
    }

    // ---- The GPU record (std430; must match Common/RadianceCache.glslh). 176 bytes. --------------------------
    struct RadianceCacheParamsPayload
    {
        glm::vec4 ClipmapCornerAndCellSize[kMaxClipmaps]{}; // xyz = corner (world, cm), w = cell size (cm)
        // Per clipmap, the distance a probe's rays skip before they may hit, cm (UE RadianceProbeSettings.x,
        // GetRadianceProbeTMin). [0].xyzw = clipmaps 0..3, [1].xy = 4..5.
        glm::vec4 ClipmapProbeTMin[2]{};
        // x = clipmaps, y = grid resolution, z = traced probe resolution, w = final probe resolution (with border)
        glm::uvec4 Shape{ 0u };
        // x = probe atlas resolution in probes, y = irradiance probe resolution, z = probes per frame,
        // w = distance-moment probe resolution
        glm::uvec4 Atlas{ 0u };
        // x = reprojection radius scale, y = 1 / clipmap fade size (cells), z = 1 / final radiance atlas texels
        glm::vec4 Sampling{ 0.0f };
    };
    static_assert( sizeof( RadianceCacheParamsPayload ) == 176 );

    // ---- The update's own shape (GI-RADIANCE-CACHE) -----------------------------------------------------------
    // Probe slots in the atlases: one per atlas cell.
    [[nodiscard]] constexpr uint32_t MaxProbes( const RadianceCacheInputs& inputs )
    {
        return inputs.ProbeAtlasResolutionInProbes * inputs.ProbeAtlasResolutionInProbes;
    }
    // Indirection entries: GridResolution^3 cells per clipmap, clipmaps side by side along x (UE's 3D texture,
    // here a buffer so a frame without the cache binds an uploaded stand-in instead of a 3D integer texture).
    [[nodiscard]] constexpr uint32_t IndirectionEntries( const RadianceCacheInputs& inputs )
    {
        return inputs.GridResolution * inputs.GridResolution * inputs.GridResolution * inputs.Clipmaps;
    }
    // Octahedral texels per side of a probe's DISTANCE MOMENTS (mean and mean square of the probe rays' hit
    // distance, DDGI's visibility term; not in UE — see WorldRadianceCache.hpp, "where this differs").
    inline constexpr uint32_t                   kDepthProbeResolution = 8;
    inline constexpr Core::Formats::ImageFormat kDepthAtlasFormat     = Core::Formats::ImageFormat::RG16F;
    // Frames a probe keeps its slot after the last frame a pixel read it (UE frees a probe the frame it is not
    // marked; a camera turning back would then re-trace what it saw a moment ago).
    inline constexpr uint32_t kProbeUnusedFramesBeforeFree = 8;
    // Priority buckets of the update (UE SelectMaxPriorityBucket's histogram).
    inline constexpr uint32_t kPriorityBuckets = 32;
    // Edge fade band of a clipmap, cells (UE GRadianceCacheClipmapFadeSize...: the dithered clipmap pick).
    inline constexpr float kClipmapFadeCells = 4.0f;
    // Temporal accumulation of a re-traced probe's radiance: the share of history kept (UE
    // r.Lumen.RadianceCache... temporal update), lower where the lights of the probe's cell changed.
    inline constexpr float kHistoryWeight      = 0.9f;
    inline constexpr float kDirtyHistoryWeight = 0.3f;

    // One probe slot's state (std430; Common/RadianceCache.glslh RadianceCacheProbe). 64 bytes.
    struct RadianceCacheProbePayload
    {
        glm::ivec4 Cell{ 0 }; // xyz = world cell (cells of its clipmap from the world origin), w = clipmap
        glm::uvec4 Frames{
             0u }; // x = last used, y = last traced (0 = never), z = traced light hash, w = light hash
        glm::vec4  Offset{ 0.0f }; // xyz = the probe's relocation out of geometry (cm), w = 0
        glm::uvec4 Priority{ 0u }; // x = priority bucket this frame
    };
    static_assert( sizeof( RadianceCacheProbePayload ) == 64 );

    // PURE. The GPU record of this frame's cache.
    [[nodiscard]] inline RadianceCacheParamsPayload
    MakeRadianceCacheParams( const std::array<Clipmap, kMaxClipmaps>& clipmaps, const RadianceCacheInputs& inputs )
    {
        RadianceCacheParamsPayload params;
        const uint32_t             count = std::min( inputs.Clipmaps, kMaxClipmaps );
        for ( uint32_t i = 0; i < count; ++i )
            params.ClipmapCornerAndCellSize[i] = glm::vec4( clipmaps[i].Corner, clipmaps[i].CellSize );
        // The probes are moved out of geometry before they trace (WorldRadianceCache.hpp), so their rays start
        // in free space and skip nothing.
        params.ClipmapProbeTMin[0] = glm::vec4( 0.0f );
        params.ClipmapProbeTMin[1] = glm::vec4( 0.0f );
        params.Shape =
             glm::uvec4( count, inputs.GridResolution, inputs.ProbeResolution, inputs.FinalProbeResolution() );
        params.Atlas    = glm::uvec4( inputs.ProbeAtlasResolutionInProbes, inputs.IrradianceProbeResolution,
                                      inputs.ProbesPerFrame, kDepthProbeResolution );
        params.Sampling = glm::vec4(
             inputs.ReprojectionRadiusScale, 1.0f / kClipmapFadeCells,
             1.0f / static_cast<float>( inputs.ProbeAtlasResolutionInProbes * inputs.FinalProbeResolution() ),
             0.0f );
        return params;
    }
} // namespace Desert::Graphic::RadianceCache
