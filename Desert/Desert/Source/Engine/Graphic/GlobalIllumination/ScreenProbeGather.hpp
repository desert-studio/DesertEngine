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
// LumenScreenProbeGather.usf. Six compute passes a frame (Programs/GlobalIllumination/ScreenProbe*.shader; the
// GPU records are Common/ScreenProbes.glslh):
//   Place ×3   a uniform probe on every 16-pixel tile's centre, then adaptive probes on the 8 and 4 pixel tiles
//              where the coarser probes cannot interpolate (UE ScreenProbeAdaptivePlacement), up to half the
//              uniform count (UE r.Lumen.ScreenProbeGather.AdaptiveProbeAllocationFraction 0.5).
//   Trace      8x8 rays per probe through the GI ray contract (TracerVariant / DeclareTracerReads: the pass names
//              no tracer resource), out to the world radiance cache's minimum trace distance; past it the cache's
//              radiance is the far field (RadianceCacheRadianceAt).
//   Filter     spatial filter across neighbouring probes with UE's hit-angle test; the filtered radiance (rough
//              reflections) and its second-order SH irradiance (diffuse).
//   Integrate  per pixel: the probes around it by plane and normal, diffuse from the SH, specular from the
//              radiance in the reflection direction, blended with the reprojected history.
// The deferred composite reads the two results per pixel (one fetch each) in place of the sky's diffuse
// irradiance and prefiltered radiance: no surface of a traced method reads the sky cube.
//
// WHERE THIS DIFFERS FROM UE (for the owner):
//   * The probe is a hemi-octahedron around its pixel's normal, not a world-space full octahedron: every ray
//     leaves the surface (UE spends the lower half on BRDF importance sampling). Neighbours meet by direction,
//     each in its own frame, in the filter and the integrate.
//   * No screen-space trace (HZB) in front of the world trace yet: a ray starts at the tracer's surface bias
//     (GIRayBias). It is the tracer contract's next consumer, not a change of this gather.
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
        glm::uvec4 Atlas{ 0u }; // x = probes per atlas row, y = frame index, z = 1 when the history is valid
    };
    static_assert( sizeof( ScreenProbeParamsPayload ) == 112 );

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
        static constexpr uint32_t kTracedTexels   = 8;  // UE r.Lumen.ScreenProbeGather.TracingOctahedronResolution
        static constexpr uint32_t kAtlasProbesRow = 128;
        static constexpr uint32_t kSHCoefficients = 9;

        glm::uvec2 Grid{ 0u }; // level-0 tiles
        uint32_t   UniformProbes  = 0;
        uint32_t   AdaptiveProbes = 0; // the budget
        uint32_t   IndexEntries   = 0; // 1 (the adaptive count) + every level's grid
        uint32_t   AtlasRows      = 0;

        [[nodiscard]] uint32_t Probes() const
        {
            return UniformProbes + AdaptiveProbes;
        }
    };

    [[nodiscard]] inline ScreenProbeLayout MakeScreenProbeLayout( const glm::uvec2 extent )
    {
        ScreenProbeLayout layout;
        const auto        grid = [extent]( const uint32_t spacing )
        { return glm::uvec2( ( extent.x + spacing - 1u ) / spacing, ( extent.y + spacing - 1u ) / spacing ); };
        layout.Grid           = grid( ScreenProbeLayout::kDownsample );
        layout.UniformProbes  = layout.Grid.x * layout.Grid.y;
        layout.AdaptiveProbes = layout.UniformProbes / 2u;
        layout.IndexEntries   = 1u;
        for ( uint32_t level = 0; level < ScreenProbeLayout::kLevels; ++level )
        {
            const glm::uvec2 g = grid( ScreenProbeLayout::kDownsample >> level );
            layout.IndexEntries += g.x * g.y;
        }
        layout.AtlasRows =
             ( layout.Probes() + ScreenProbeLayout::kAtlasProbesRow - 1u ) / ScreenProbeLayout::kAtlasProbesRow;
        return layout;
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
        glm::mat4          PrevViewProjection{ 1.0f }; // unjittered
        glm::vec3          PrevCameraPosition{ 0.0f };
    };

    // What the deferred composite reads (rgba16f, the G-buffer's extent).
    struct ScreenProbeGatherFrame
    {
        RDG::TextureRef
             Diffuse; // rgb = irradiance / pi, a = distance to the camera — "u_GIDiffuse" in the composite
        RDG::TextureRef Specular; // rgb = radiance in the reflection direction — "u_GISpecular" in the composite
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

        // The gather of one frame, its six passes added to @p graph after the radiance cache's update.
        [[nodiscard]] Common::ResultStr<ScreenProbeGatherFrame>
        AddPasses( RDG::Builder& graph, const WorldRadianceCache::ImportImage& import,
                   const ScreenProbeGatherInputs& inputs );

    private:
        enum Program : uint32_t
        {
            Place = 0,
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
        std::array<std::shared_ptr<Image2D>, 2> m_Diffuse;  // this frame's output / last frame's history
        std::array<std::shared_ptr<Image2D>, 2> m_Specular;
        glm::uvec2                              m_Extent{ 0u };
        uint32_t                                m_AtlasRows    = 0;
        uint32_t                                m_Current      = 0;
        uint32_t                                m_FrameIndex   = 0;
        bool                                    m_HistoryValid = false;
    };
} // namespace Desert::Graphic::GlobalIllumination
