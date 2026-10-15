#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>
#include <Engine/Graphic/GlobalIllumination/RadianceCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

// THE WORLD RADIANCE CACHE, RUNNING (GI-RADIANCE-CACHE) — the update of RadianceCache.hpp's interface, ported from
// UE Lumen's LumenRadianceCache.cpp / LumenRadianceCacheUpdate.usf / LumenRadianceCache.usf. Six compute passes a
// frame (Programs/GlobalIllumination/RadianceCache*.shader):
//   Mark      the cells this frame's readers interpolate are stamped "used": the G-buffer pixels, which are where
//             the screen probes sit (UE marks from the screen probes; every probe is a G-buffer pixel).
//   Update    entries whose probe belongs to another world cell (the clipmap scrolled) or that went unread for
//             kProbeUnusedFramesBeforeFree frames free their slot.
//   Allocate  used empty entries take a slot; every used probe is ranked (new, lights changed, then age of its
//             last trace) into the priority histogram.
//   Select    the histogram is cut at ProbesPerFrame; the probes above the cut are this frame's trace list.
//   Trace     each listed probe traces its octahedral texels through the GI ray contract (Common/GITracing.glslh:
//             the pipeline's tracer, the hit lit in place, the sky on a miss) into the radiance atlas, blended
//             with its history. Its program is the permutation of TracerVariant( Pipeline ) and its tracer reads
//             are DeclareTracerReads — the pass names no tracer resource itself (GITracing.hpp).
//   Filter    radiance -> irradiance probe and distance moments, octahedral borders; the probe is published.
// The screen probe gather (ScreenProbeGather.hpp) then reads the cache's RADIANCE as the far field of its rays,
// and every GI hit reads its irradiance for the next bounce (RadianceCacheIrradianceAt in GIHitRadiance).
//
// WHERE THIS DIFFERS FROM UE (for the owner):
//   * Visibility in the interpolation. UE interpolates the eight probes with trilinear weights only and leaves
//     leaks to the screen probes' short traces in front of the cache. Until those exist the pixel reads the cache
//     directly, so each probe also keeps the mean and mean square of its rays' hit distances (DDGI), and a probe
//     that sees a nearer surface in the point's direction (the point is behind a wall from it) is weighted out
//     by Chebyshev's bound. Kept when screen probes arrive: it also guards their own cache reads.
//   * Probes move out of geometry before they trace (DDGI's relocation, along the distance field's gradient,
//     at most 0.45 of a cell). UE's world-grid probe inside a wall traces from inside it.
//   * Lights changing in a probe's cell raise its priority (RadianceCache.hpp header note).
//   * The indirection is a toroidal BUFFER (world cell mod resolution), not UE's 3D texture remapped on every
//     clipmap move: a scroll costs nothing, an entry is stale exactly when its probe records another cell.
//   * A probe keeps its slot kProbeUnusedFramesBeforeFree frames unread; UE frees it the first unmarked frame.
namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class Image;
    class Image2D;
    class Shader;
    class ComputePipeline;
} // namespace Desert::Graphic

namespace Desert::Graphic::GlobalIllumination
{
    // This frame's graph resources the update reads.
    struct RadianceCacheUpdateInputs
    {
        RDG::TextureRef                GBufferDepth;
        RDG::TextureRef                GBufferA; // albedo
        RDG::TextureRef                GBufferB; // normal
        RDG::TextureRef                GBufferEmissive;
        RDG::TextureRef                EnvSpecular; // the sky's radiance cube (or the system black cube)
        TracerFrame                    Tracer;      // the active tracer and its reads (GITracing.hpp)
        SurfaceCacheFrame              SurfaceCache; // the surface cache a hit reads first (GITracing.hpp)
        RDG::BufferRef                 PointLights;
        RDG::BufferRef                 SpotLights;
        RDG::BufferRef                 WorldGridParams; // the world light grid (LightGridLayout::WorldClipmaps)
        RDG::BufferRef                 WorldGridCells;
        RDG::BufferRef                 GIFrame; // this frame's GIFramePayload (GITracing.hpp), "GIFrameUB"
        glm::vec3                      CameraPosition{ 0.0f };
        glm::uvec2                     Extent{ 0u };
    };

    // What a reader of this frame's cache binds (Common/RadianceCache.glslh, the read section).
    struct RadianceCacheFrame
    {
        RDG::BufferRef  Params;      // "RadianceCacheParamsUB"
        RDG::BufferRef  Indirection; // "RadianceCacheIndirectionUB"
        RDG::BufferRef  Probes;      // "RadianceCacheProbesUB"
        RDG::TextureRef Irradiance;  // "u_RadianceCacheIrradiance"
        RDG::TextureRef Depth;       // "u_RadianceCacheDepth"
        // The traced radiance probes, octahedral with borders (UE RadianceProbeAtlasTexture): the far field of
        // a screen probe ray that leaves the cache's minimum trace distance without a hit
        // (RadianceCacheRadianceAt).
        RDG::TextureRef Radiance; // "u_RadianceCacheRadianceAtlas"
    };

    class WorldRadianceCache
    {
    public:
        WorldRadianceCache();
        ~WorldRadianceCache();
        WorldRadianceCache( const WorldRadianceCache& )            = delete;
        WorldRadianceCache& operator=( const WorldRadianceCache& ) = delete;

        // The five tracer-independent shaders and pipelines; Trace's is acquired per tracer permutation on the
        // first update (EnsureTraceProgram). An error names what is missing.
        [[nodiscard]] Common::BoolResultStr Initialize();

        using ImportImage = std::function<RDG::TextureRef( const std::shared_ptr<Image>&, std::string_view )>;

        // The update of one frame, its six passes added to @p graph. The cache is (re)built when @p shape differs
        // from the last frame's (a quality change): every probe is traced again.
        [[nodiscard]] Common::ResultStr<RadianceCacheFrame>
        AddUpdatePasses( RDG::Builder& graph, const ImportImage& import,
                         const RadianceCache::RadianceCacheInputs& shape,
                         const RadianceCacheUpdateInputs&          inputs );

    private:
        enum Pass : uint32_t
        {
            Mark = 0,
            Update,
            Allocate,
            Select,
            Trace,
            Filter,
            PassCount
        };

        [[nodiscard]] Common::BoolResultStr EnsureResources( const RadianceCache::RadianceCacheInputs& shape );
        // The Trace program compiled under TracerVariant( @p pipeline ), re-acquired when the pipeline changes.
        [[nodiscard]] Common::BoolResultStr EnsureTraceProgram( const Pipeline& pipeline );

        std::array<std::shared_ptr<Shader>, PassCount>          m_Shaders;
        std::array<std::shared_ptr<ComputePipeline>, PassCount> m_Pipelines;
        mutable std::array<ShaderBindingLayoutCache, PassCount> m_Layouts;
        Pipeline                                                m_TraceProgramFor{}; // what m_Shaders[Trace] is

        // Persistent state (StorageBuffer persistent = true: one device buffer every frame writes).
        std::shared_ptr<ShaderResources::StorageBuffer> m_Indirection;
        std::shared_ptr<ShaderResources::StorageBuffer> m_LastUsed;
        std::shared_ptr<ShaderResources::StorageBuffer> m_Probes;
        std::shared_ptr<ShaderResources::StorageBuffer> m_FreeList;
        RDG::ExternalBuffer                             m_IndirectionImport;
        RDG::ExternalBuffer                             m_LastUsedImport;
        RDG::ExternalBuffer                             m_ProbesImport;
        RDG::ExternalBuffer                             m_FreeListImport;
        std::shared_ptr<Image2D>                        m_RadianceAtlas;
        std::shared_ptr<Image2D>                        m_IrradianceAtlas;
        std::shared_ptr<Image2D>                        m_DepthAtlas;

        RadianceCache::RadianceCacheInputs m_Shape{};
        bool                               m_HasShape   = false;
        bool                               m_Reset      = true;
        uint32_t                           m_FrameIndex = 0;
    };
} // namespace Desert::Graphic::GlobalIllumination
