#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/GlobalIllumination/GITracing.hpp>
#include <Engine/Graphic/GlobalIllumination/RadianceCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/EmissiveLightsFrame.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

// THE WORLD RADIANCE CACHE, RUNNING (GI-RADIANCE-CACHE) — the update of RadianceCache.hpp's interface, ported from
// UE Lumen's LumenRadianceCache.cpp / LumenRadianceCacheUpdate.usf / LumenRadianceCache.usf. Five compute passes
// a frame (Programs/GlobalIllumination/RadianceCache*.shader); UE's seven steps, the short ones riding the pass
// before them on the async chain (FPS-RADIANCE-CACHE):
//   Mark      the cells this frame's readers interpolate are stamped "used": the G-buffer pixels, which are where
//             the screen probes sit (UE marks from the screen probes; every probe is a G-buffer pixel). Its group
//             rows past the pixels FREE (UE UpdateCacheForUsedProbes), one thread per probe slot, not per
//             indirection entry: a live probe whose cell the clipmap scrolled past, or whose entry went unread for
//             kProbeUnusedFramesBeforeFree frames, gives its slot back.
//   Allocate  used empty entries take a slot; every used probe is ranked (new of the coverage, new of the
//             fallback, lights changed, then age of its last trace) into the priority histogram.
//   Select    the histogram is cut at ProbesPerFrame; the probes above the cut are this frame's trace list, and
//             their trace TILES of 8x8 rays the trace tile list (UE GenerateProbeTraceTiles: a near probe traces
//             at half resolution and supersamples to full only the squares its readers' normals face - the BRDF
//             bins Mark gathers per entry -; a probe past the downsample distance, or read only as the fallback,
//             traces at a quarter). The thread that lists a probe moves it out of geometry (UE's probe placement before
//             TraceFromProbes): its offset, which Trace's rays start from. A tracer permutation as Trace's,
//             acquired with it.
//   Trace     one group per listed tile traces its octahedral texels through the GI ray contract
//             (Common/GITracing.glslh: the pipeline's tracer, the hit lit in place, the sky on a miss) into the
//             radiance atlas, blended with its history. Its program is the permutation of TracerVariant( Pipeline
//             ) and its tracer reads are DeclareTracerReads — the pass names no tracer resource itself
//             (GITracing.hpp).
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
        RDG::TextureRef   GBufferDepth;
        RDG::TextureRef   GBufferA; // albedo
        RDG::TextureRef   GBufferB; // normal
        RDG::TextureRef   GBufferEmissive;
        RDG::TextureRef   GBufferShadingWord; // the uint shading word: a hit's EMISSION_IS_LIGHT (GIHitMaterial)
        RDG::TextureRef   EnvSpecular;  // the sky's radiance cube (or the system black cube)
        TracerFrame       Tracer;       // the active tracer and its reads (GITracing.hpp)
        SurfaceCacheFrame SurfaceCache; // the surface cache a hit reads first (GITracing.hpp)
        // A probe ray's hit reads its lighting without its own emission (GI_HIT_EXCLUDES_EMISSION): the listed
        // emitters reach the probe by sampling this list instead (RadianceCacheTrace's candidates). Required.
        RayTracing::EmissiveLightsFrame Emissive;
        // The surface cache's Emissive layer (the cards' emission a card hit subtracts); required when the hits
        // read the cards.
        RDG::TextureRef SurfaceCacheEmissive;
        RDG::BufferRef    PointLights;
        RDG::BufferRef    SpotLights;
        // The light function atlas the rows' tiles name (ViewLightBuffers::LightFunctionAtlas); invalid = none.
        RDG::TextureRef   LightFunctionAtlas;
        RDG::BufferRef    WorldGridParams; // the world light grid (LightGridLayout::WorldClipmaps)
        RDG::BufferRef    WorldGridCells;
        RDG::BufferRef    GIFrame; // this frame's GIFramePayload (GITracing.hpp), "GIFrameUB"
        // World points read in the six axis directions besides the pixels (cm): the reflection captures' points,
        // whose stored diffuse is measured from this cache (ReflectionCaptures/ReflectionCaptureRenderer.hpp).
        std::vector<glm::vec3> MarkPoints;
        glm::vec3              CameraPosition{ 0.0f };
        glm::uvec2             Extent{ 0u };
        // The lighting history key of the frame (lights and sky; the same values GIFrame carries): a significant
        // change from the key the probes were last fully traced under makes this frame a full update
        // (AddUpdatePasses).
        LightingHistoryKey Lighting;
        // The screen probes read this frame's cache with the coarser fallback (Common/RadianceCache.glslh
        // RadianceCacheRadianceAt): Mark also stamps the clipmap past each pixel's coarse end, the level that
        // answers while the coverage clipmap's cells are untraced. False for a cache only the irradiance readers
        // read (the probe volume gather): nobody reads that level there, so its cells would only take the budget.
        bool MarkRadianceFallback = false;
    };

    // What a reader of this frame's cache binds (Common/RadianceCache.glslh, the read section).
    struct RadianceCacheFrame
    {
        RDG::BufferRef  Params;      // "RadianceCacheParamsUB"
        RDG::BufferRef  Indirection; // "RadianceCacheIndirectionUB"
        RDG::BufferRef  Probes;      // "RadianceCacheProbesUB"
        RDG::TextureRef Irradiance;  // "RadianceCacheIrradianceTexture"
        RDG::TextureRef Depth;       // "RadianceCacheDepthTexture"
        // The traced radiance probes, octahedral with borders (UE RadianceProbeAtlasTexture): the far field of
        // a screen probe ray that leaves the cache's minimum trace distance without a hit
        // (RadianceCacheRadianceAt).
        RDG::TextureRef Radiance; // "RadianceCacheRadianceAtlasTexture"
    };

    // PROBE-TRACE-COST-3: one frame's radiance cache update in counts - RC_COUNT_* of
    // Common/RadianceCacheState.glslh, in this order, written by RadianceCacheSelect.shader once Mark (its free
    // too) and Allocate are done. Where a camera turn's probes go: the entries the readers stamped and how many of
    // them hold a probe never traced, against what the budget traced; the slots taken, refused (the atlas full)
    // and freed, by why.
    struct RadianceCacheUpdateCounts
    {
        static constexpr uint32_t kWords = 14u + 2u * RadianceCache::kMaxClipmaps; // RC_COUNT_WORDS
        uint32_t Stamped       = 0; // entries a reader stamped this frame (RadianceCacheMark)
        uint32_t NeverTraced   = 0; // ... by a coverage reader, never traced (RC_PRIORITY_NEW_COVERAGE)
        uint32_t Traced        = 0; // probes traced this frame: what Select listed (the refresh capped, FPS-RC-BUDGET)
        uint32_t Budget        = 0; // the Select pass's budget (ProbesPerFrame, MaxProbes on a full update)
        uint32_t Allocated     = 0; // empty stamped entries that took a slot
        uint32_t Failed        = 0; // ... that found none (the atlas is full)
        uint32_t FreedStale    = 0; // the clipmap scrolled past the probe's cell
        uint32_t FreedUnused   = 0; // unread for more than kProbeUnusedFramesBeforeFree frames
        uint32_t FreedPressure = 0; // unread for more than the pressure's keep only (RADIANCE_CACHE_PRESSURE_KEEP_FRAMES)
        uint32_t Pressed       = 0; // 1: the last frame refused a slot, so the pressure's keep applied
        uint32_t Live          = 0; // slots held after Allocate
        uint32_t MaxProbes     = 0; // the atlas's slots
        std::array<uint32_t, RadianceCache::kMaxClipmaps> NeverTracedByClipmap{}; // NeverTraced per clipmap
        // Never-traced probes only the fallback stamped (the clipmap past a pixel's coarse end), per clipmap; they
        // are traced from what the coverage's new probes leave of the budget (RC_PRIORITY_NEW_FALLBACK).
        std::array<uint32_t, RadianceCache::kMaxClipmaps> FallbackByClipmap{};
        // FPS-RC-BUDGET census: the stamped probes due a trace by an event other than new (lights changed, or last
        // traced before the last full update), and those due only by age (the refresh, RadianceCache::kRefreshBudgetDivisor).
        uint32_t LightsChanged = 0;
        uint32_t Aged          = 0;
        bool     Valid         = false; // a frame's words have come back
    };
    static_assert( RadianceCacheUpdateCounts::kWords == 26 ); // RC_COUNT_WORDS with RADIANCE_CACHE_MAX_CLIPMAPS 6

    // RadianceCache.UpdateCounts read back through a frame-in-flight slot (ScreenProbeMissCounts' pattern): when the
    // slot comes round again its fence has passed. Last() is the newest frame read, slots-in-flight late.
    class RadianceCacheUpdateCountsReadback
    {
    public:
        // Reads the slot of @p frameIndex, then hands it this frame's @p counts (extracted for the host).
        void AddFrame( RDG::Builder& graph, uint64_t frameIndex, RDG::BufferRef counts );
        [[nodiscard]] const RadianceCacheUpdateCounts& Last() const
        {
            return m_Last;
        }

    private:
        std::vector<RDG::ExternalBuffer> m_Slots;
        RadianceCacheUpdateCounts        m_Last;
    };

    class WorldRadianceCache
    {
    public:
        WorldRadianceCache();
        ~WorldRadianceCache();
        WorldRadianceCache( const WorldRadianceCache& )            = delete;
        WorldRadianceCache& operator=( const WorldRadianceCache& ) = delete;

        // The three tracer-independent shaders and pipelines; Select's and Trace's are acquired per tracer
        // permutation on the first update (EnsureTraceProgram). An error names what is missing.
        [[nodiscard]] Common::BoolResultStr Initialize();
        // The Select and Trace programs compiled under TracerVariant( @p pipeline ), re-acquired when the pipeline
        // changes.
        // Public so the view asks for it before the update and refuses the GI methods of that tracer by name
        // when it does not build (SceneRenderer::AddFrameTracedGI); AddUpdatePasses asks again (cached).
        [[nodiscard]] Common::BoolResultStr EnsureTraceProgram( const Pipeline& pipeline );
        // Every variant EnsureTraceProgram can acquire: Select and Trace under each tracer permutation
        // (TracerProgramVariants), cooked by the package (PackageCook CookShaderVariants).
        [[nodiscard]] static std::vector<Core::CookedProgramVariant> ProgramVariants();

        using ImportImage = std::function<RDG::TextureRef( const std::shared_ptr<Image>&, std::string_view )>;

        // The update of one frame, its first four passes (Mark .. Trace) added to @p graph; the fifth, Filter,
        // is AddFilterPass's, which the caller MUST declare in the same graph before any reader of the returned
        // frame's Indirection, Probes, Irradiance, Depth or Radiance (Params is an upload: readable at once). The
        // cache is (re)built when @p shape differs from the last frame's (a quality change): every probe is traced
        // again. A significant change of the lighting history key (lights and sky; InvalidatesLightingHistory)
        // makes the frame a FULL UPDATE (UE r.Lumen.RadianceCache.ForceFullUpdate): every used probe is traced in
        // it, its history dropped.
        // FPS-PROBE-WAIT-2 (UE Lumen: the far-field trace overlaps the cache): the two declarations let the view
        // declare async work that reads nothing of the cache (the screen probes' far field) between Trace and
        // Filter, so the async queue runs it beside the cache's filter instead of after it.
        // The cache's persistent resources registered in one graph, once (RDG::Builder::RegisterExternal makes a
        // new record per call: two registrations of one buffer in a graph would hide each other's hazards).
        struct GraphResources
        {
            RadianceCacheFrame Frame; // Params: the last filtered update's clipmaps (AddReadFrame), or invalid
            RDG::BufferRef     LastUsed;
            RDG::BufferRef     FreeList;
        };
        // @p registered: the resources AddReadFrame registered in this same graph (the update after its readers,
        // FPS-ASYNC-WAIT), or null to register them here.
        [[nodiscard]] Common::ResultStr<RadianceCacheFrame>
        AddUpdatePasses( RDG::Builder& graph, const ImportImage& import,
                         const RadianceCache::RadianceCacheInputs& shape, const RadianceCacheUpdateInputs& inputs,
                         const GraphResources* registered );
        // The update's Filter pass (the probes' irradiance and depth from the traced radiance) and the update's
        // counts read-back, after the AddUpdatePasses of the same graph; an error when none is pending (no update
        // declared, or its filter declared already).
        [[nodiscard]] Common::BoolResultStr AddFilterPass( RDG::Builder& graph );

        // FPS-ASYNC-WAIT (the cache's update off the GI's critical path): true when the cache holds a FILTERED
        // update of @p shape - a frame may then read it as that update left it (AddReadFrame) and declare its
        // own update after every reader, where its async run overlaps the frame's tail (the temporal upscale,
        // the post chain) instead of heading the chain the composite waits for. False after a (re)build, before
        // the first filter, or while an update waits for its filter: that frame updates before its readers.
        [[nodiscard]] bool HoldsFilteredUpdate( const RadianceCache::RadianceCacheInputs& shape ) const;
        // The cache as the last filtered update left it, registered in @p graph: its Params re-uploaded from
        // that update's clipmaps (the indirection is laid out by them, not by this frame's camera). Pass the
        // result to this graph's AddUpdatePasses, declared after the last reader. Refused unless
        // HoldsFilteredUpdate( @p shape ).
        [[nodiscard]] Common::ResultStr<GraphResources>
        AddReadFrame( RDG::Builder& graph, const ImportImage& import,
                      const RadianceCache::RadianceCacheInputs& shape );
        // The newest update read back in counts (RadianceCacheUpdateCounts), frames in flight late.
        [[nodiscard]] const RadianceCacheUpdateCounts& LastUpdateCounts() const
        {
            return m_UpdateCounts.Last();
        }

    private:
        enum Pass : uint32_t
        {
            Mark = 0,
            Allocate,
            Select,
            Trace,
            Filter,
            PassCount
        };

        // std430 RadianceCachePush (WorldRadianceCache.cpp, Common/RadianceCacheState.glslh), carried as bytes.
        static constexpr size_t kPushBytes = 80;
        // One update's graph resources and dispatch shapes, from AddUpdatePasses to its AddFilterPass.
        struct UpdateDeclaration
        {
            RadianceCacheFrame                 Frame;
            RDG::BufferRef                     LastUsed;
            RDG::BufferRef                     FreeList;
            RDG::BufferRef                     Schedule;
            RDG::BufferRef                     UpdateCounts;
            RDG::BufferRef                     DispatchArgs;
            RDG::BufferRef                     MarkPoints;
            RDG::BufferRef                     GIFrame;
            std::array<glm::uvec3, PassCount>  Groups{};
            std::array<std::byte, kPushBytes>  Push{};
            uint32_t                           FrameIndex = 0;
            bool                               FullUpdate = false;
            RadianceCache::RadianceCacheParamsPayload ParamsPayload{}; // what Frame.Params was uploaded with
        };
        // The persistent buffers and atlases registered in @p graph (Params left invalid).
        [[nodiscard]] Common::ResultStr<GraphResources> RegisterResources( RDG::Builder&      graph,
                                                                           const ImportImage& import );
        // One pass of the update; @p inputs is null for Filter (it reads only the cache's own resources).
        void DeclarePass( RDG::Builder& graph, Pass pass, const UpdateDeclaration& update,
                          const RadianceCacheUpdateInputs* inputs );

        [[nodiscard]] Common::BoolResultStr EnsureResources( const RadianceCache::RadianceCacheInputs& shape );

        std::array<std::shared_ptr<Shader>, PassCount>          m_Shaders;
        std::array<std::shared_ptr<ComputePipeline>, PassCount> m_Pipelines;
        mutable std::array<ShaderBindingLayoutCache, PassCount> m_Layouts;
        Pipeline m_TraceProgramFor{}; // what m_Shaders[Select / Trace] are

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
        // The lighting history key (lights and sky) of the last full update (empty until the first update after a
        // reset).
        std::optional<LightingHistoryKey>  m_LitBy;
        uint32_t                           m_LightingFrame = 0; // FrameIndex of the last full update (0 = none)
        uint32_t m_FullUpdatesInWindow                     = 0; // since the last "[RadianceCache] traced" line
        RadianceCacheUpdateCountsReadback  m_UpdateCounts;
        // The update declared by AddUpdatePasses whose Filter AddFilterPass has not declared yet.
        std::optional<UpdateDeclaration> m_PendingFilter;
        // The clipmaps of the newest update whose Filter was declared (AddFilterPass); dropped by a rebuild
        // (EnsureResources). What AddReadFrame's Params carries.
        std::optional<RadianceCache::RadianceCacheParamsPayload> m_FilteredParams;
    };
} // namespace Desert::Graphic::GlobalIllumination
