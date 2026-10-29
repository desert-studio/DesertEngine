#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/RenderMethods.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// "Shadows: Virtual Shadow Map" (UE Renderer/Private/VirtualShadowMaps: VirtualShadowMapArray,
// VirtualShadowMapClipmap, VirtualShadowMapCacheManager, VirtualShadowMapProjection): ShadowMethod::VirtualMaps
// for the sun. One virtual 16k x 16k depth map per clipmap level around the camera, cut into 128 x 128 pages; each
// frame the G-buffer's pixels request the pages they are shaded from, only those get a page of the physical pool,
// a page stays cached across frames until its world page scrolls out, its level's depth range moves, a moved
// caster covers it or the sun turns, only dirty requested pages are cleared and drawn, and the screen mask is
// traced through the map with shadow-map rays inside the sun's SourceAngle cone (SMRT).
//
// THE POINT AND SPOT LIGHTS (VSM-LOCAL; UE FVirtualShadowMap per local light): the same page table, pool and page
// passes hold every shadowed local light under the same method. A spot light is one 16k perspective view, a point
// light six cube faces, each view a chain of kVsmLocalMipCount mips (128 x 128 pages at mip 0 down to 1 x 1); a
// G-buffer pixel requests the page of the mip its footprint asks for ("VSM: Mark Local"), only requested pages get
// a physical page, only dirty ones are cleared and drawn ("VSM: Raster", one node per drawn mip), and a page stays
// cached until the light changes or a caster's box (the motion mask: CollectVirtualShadowInvalidation's moved and
// changed casters) covers it. "VSM: Project Local" writes each light's screen mask into the local mask array the
// deferred composite reads by ShadowMaskSlot.
//
// The GPU side is Common/VirtualShadowMap.glslh and Programs/Shadows/VirtualShadowMap*.shader; every constant
// below has its twin there.
namespace Desert::Graphic::System
{
    // ---- The map's shape: UE's VSM constants (VirtualShadowMapDefinitions.h), not settings.
    // ----------------------
    inline constexpr uint32_t kVsmPageSize          = 128; // VSM_PAGE_SIZE: texels per page side
    inline constexpr uint32_t kVsmLevelPages        = 128; // VSM_LEVEL0_DIM_PAGES_XY: pages per level side
    inline constexpr uint32_t kVsmPagesPerLevel     = kVsmLevelPages * kVsmLevelPages;
    inline constexpr uint32_t kVsmVirtualResolution = kVsmPageSize * kVsmLevelPages; // 16384
    static_assert( kVsmVirtualResolution == Common::Scalability::kVirtualShadowMapResolution,
                   "the render area ResolveRenderMethods asks the device for is the virtual map's side" );
    // r.Shadow.Virtual.Clipmap.FirstLevel / LastLevel: level k covers +-2^k cm around the camera (64 cm .. 42 km).
    inline constexpr int32_t  kVsmClipmapFirstLevel = 6;
    inline constexpr int32_t  kVsmClipmapLastLevel  = 22;
    inline constexpr uint32_t kVsmClipmapLevelCount =
         static_cast<uint32_t>( kVsmClipmapLastLevel - kVsmClipmapFirstLevel + 1 );
    // r.Shadow.Virtual.Clipmap.ZRangeScale: a level's depth range is +-radius * this around its snapped centre, so
    // casters far toward the sun still land in the map.
    inline constexpr float kVsmClipmapZRangeScale = 1000.0f;
    // The physical pool's width in pages (Common/VirtualShadowMap.glslh VSM_POOL_PAGES_PER_ROW): 8192 texels.
    inline constexpr uint32_t kVsmPoolPagesPerRow = 64;
    // r.Shadow.Virtual.NormalBias: the receiver's offset along its normal, in texels of its level.
    inline constexpr float kVsmNormalBiasTexels = 0.5f;
    // How far an SMRT ray marches toward the sun, in pages of the receiver's level (UE
    // r.Shadow.Virtual.SMRT.RayLengthScaleDirectional bounds the same march): the widest penumbra the map can
    // show.
    inline constexpr float kVsmSmrtRayLengthPages = 4.0f;

    // Local lights (VSM_LEVEL_LOCAL): every view a mip chain of virtual maps, mip k of a view 128 >> k pages a
    // side.
    inline constexpr uint32_t kVsmLocalMipCount = 8; // VSM_LOCAL_MIPS (UE VSM_MAX_MIP_LEVELS)
    // The page-table entries of one local view: 128^2 + 64^2 + ... + 1^2.
    inline constexpr uint32_t kVsmLocalViewEntries = ( 4u * kVsmPagesPerLevel - 1u ) / 3u;
    static_assert( kVsmLocalViewEntries == 21845u );
    [[nodiscard]] constexpr uint32_t VsmLocalMipPages( const uint32_t mip )
    {
        return kVsmLevelPages >> mip;
    }
    // The first entry of mip @p mip within its view's kVsmLocalViewEntries.
    [[nodiscard]] constexpr uint32_t VsmLocalMipEntryOffset( const uint32_t mip )
    {
        uint32_t offset = 0;
        for ( uint32_t m = 0; m < mip; ++m )
            offset += VsmLocalMipPages( m ) * VsmLocalMipPages( m );
        return offset;
    }
    static_assert( VsmLocalMipEntryOffset( kVsmLocalMipCount ) == kVsmLocalViewEntries );
    inline constexpr uint32_t kVsmLevelKindClipmap = 0; // VSM_LEVEL_CLIPMAP
    inline constexpr uint32_t kVsmLevelKindLocal   = 1; // VSM_LEVEL_LOCAL
    inline constexpr float kVsmLocalPointCone = -2.0f; // VSM_LOCAL_POINT_CONE: VsmLocalLightGpu::Spot.w of a point
    // The page table's room for local views starts at this many and doubles when the frame's lights need more (the
    // table is made again then: every page starts unmapped).
    inline constexpr uint32_t kVsmMinLocalViews = 64;
    // A light not handed in for this many frames gives its view slots back.
    inline constexpr uint64_t kVsmLocalRetireFrames = 120;

    // Page-table flags (VSM_FLAG_*).
    inline constexpr uint32_t kVsmFlagAllocated = 1u;
    inline constexpr uint32_t kVsmFlagDirty     = 2u;
    inline constexpr uint32_t kVsmFlagRequested = 4u;
    inline constexpr uint32_t kVsmInvalid       = 0xFFFFFFFFu;

    inline constexpr std::string_view kVirtualShadowMapPassPrefix = "VSM: ";

    // ---- GPU records (std430; the glslh structs of the same name).
    // ------------------------------------------------
    struct VsmFrameGpu
    {
        glm::mat4 InvViewProjection{ 1.0f };
        glm::vec4 CameraFootprint{ 0.0f }; // xyz camera cm, w = world size of one render pixel per cm of distance
        glm::vec4 Lod{ 0.0f };             // x LOD bias, y level count, z first level, w frame index
        glm::vec4 ToSun{ 0.0f };           // xyz toward the sun, w SourceAngle radians
        glm::vec4 Smrt{ 0.0f };            // x rays, y samples per ray, z normal bias texels, w ray length pages
    };
    static_assert( sizeof( VsmFrameGpu ) == 128 );

    struct VsmLevelGpu
    {
        glm::mat4  ViewProjection{ 1.0f }; // world -> level clip, orthographic, standard Z
        glm::ivec4 Window{ 0 };            // xy window origin (world page), z absolute level, w 1 = reset now
        glm::vec4  Extent{ 0.0f };         // x page world cm, y depth range cm, z radius cm, w texel world cm
        // x first page-table entry, y pages per side (0 = unused level), z kVsmLevelKind*, w local light index
        glm::uvec4 Pages{ 0u };
    };
    static_assert( sizeof( VsmLevelGpu ) == 112 );

    // One point / spot light shaded from the map this frame (VsmLocalLight in Common/VirtualShadowMap.glslh).
    struct VsmLocalLightGpu
    {
        glm::vec4  Position{ 0.0f }; // xyz cm, w attenuation radius cm
        glm::vec4  Spot{ 0.0f };     // xyz out of the apex, w cos outer (kVsmLocalPointCone = point)
        glm::uvec4 Views{ 0u };      // x level of view 0 mip 0, y view count, z mask layer, w finest mip
        glm::vec4  Params{ 0.0f }; // x near cm, y far cm, z texel world size at unit distance (mip 0), w source cm
    };
    static_assert( sizeof( VsmLocalLightGpu ) == 64 );

    struct VsmPageEntryGpu
    {
        uint32_t Physical = 0;
        uint32_t Flags    = 0;
        int32_t  TagX     = 0;
        int32_t  TagY     = 0;
    };
    static_assert( sizeof( VsmPageEntryGpu ) == 16 );

    // "VSM: Dirty Mask": per level, one bit per cell of kVsmMaskCellPages x kVsmMaskCellPages window pages, set
    // when a page of the cell is drawn this frame (allocated, dirty, requested) - what "VSM: Cull Casters" tests a
    // caster's footprint against (UE: the page flags' hierarchy CullPerPageDrawCommands reads).
    inline constexpr uint32_t kVsmMaskCellPages     = 8;
    inline constexpr uint32_t kVsmMaskCells         = kVsmLevelPages / kVsmMaskCellPages; // per side
    inline constexpr uint32_t kVsmMaskWordsPerLevel = kVsmMaskCells * kVsmMaskCells / 32u;

    // "VSM: Cull Casters" records (std430; Programs/Shadows/VirtualShadowMapCullCasters.shader).
    // One instance a draw may draw: its world box and which draw it belongs to.
    struct VsmCullInstanceGpu
    {
        glm::vec4  Min{ 0.0f };
        glm::vec4  Max{ 0.0f };
        glm::uvec4 Draw{ 0u }; // x = its draw, y = the instance index its vertex stage fetches, zw 0
    };
    static_assert( sizeof( VsmCullInstanceGpu ) == 48 );
    // One draw: its argument records (MeshDrawArguments.hpp: one per drawn submesh, consecutive) and the slots of
    // the instance remap its surviving instances fill (its records' first instance is RemapBase).
    struct VsmCullDrawGpu
    {
        uint32_t FirstRecord = 0;
        uint32_t Records     = 0;
        uint32_t RemapBase   = 0;
        uint32_t Padding     = 0;
    };
    static_assert( sizeof( VsmCullDrawGpu ) == 16 );

    // What one level's casters hand "VSM: Cull Casters": every draw's records with an instance count of 0, every
    // instance, and the remap's size (the sum of the draws' instance counts).
    struct VsmCasterCull
    {
        std::vector<VsmCullInstanceGpu> Instances;
        std::vector<VsmCullDrawGpu>     Draws;
        std::vector<uint32_t>           Arguments;
        uint32_t                        RemapSize = 0;
    };

    // Its output, read by the level's "VSM: Raster": the argument records with the surviving instance counts
    // (Access::IndirectArgs) and the instance remap (VsmInstanceRemap: draw slot -> the instance index to fetch).
    struct VsmCulledCasters
    {
        RDG::BufferRef Arguments;
        RDG::BufferRef InstanceRemap;
    };

    // VsmAllocator: the frame's counters and the indirect dispatch of "VSM: Clear Pages" at kVsmClearArgsOffset.
    inline constexpr uint64_t kVsmAllocatorBytes  = 48;
    inline constexpr uint64_t kVsmClearArgsOffset = 32;
    inline constexpr uint64_t kVsmOverflowOffset  = 16; // VsmAllocator.Overflow

    // ---- The clipmap, on the CPU (UE FVirtualShadowMapClipmap).
    // -------------------------------------------------- One level's placement this frame. The window is snapped
    // to the level's page grid in the sun's light space and its depth range to a quarter of the range: a page
    // keeps its contents while the camera moves inside a page and the depth centre inside its step.
    struct VsmClipmapLevel
    {
        glm::mat4  ViewProjection{ 1.0f };
        glm::ivec2 WindowOrigin{ 0 };  // world page coordinate of the window's page 0
        float      DepthCentre = 0.0f; // light-space z of the depth range's centre, cm (snapped)
        float      Radius      = 0.0f; // cm
        float      PageWorld   = 0.0f; // cm
        float      DepthRange  = 0.0f; // cm
    };

    // The light basis every level shares: a view looking along @p lightDirection (the direction the sunlight
    // travels), the cascades' convention (ShadowCascades.hpp).
    [[nodiscard]] glm::mat4 VsmLightBasis( const glm::vec3& lightDirection );
    // Level @p levelIndex (0 .. kVsmClipmapLevelCount - 1) around @p camera in light basis @p lightBasis.
    [[nodiscard]] VsmClipmapLevel MakeVsmClipmapLevel( const glm::mat4& lightBasis, const glm::vec3& camera,
                                                       uint32_t levelIndex );

    // Per frame, the quality the view resolved (Scalability Shadows.Virtual*).
    struct VirtualShadowMapQuality
    {
        uint32_t MaxPhysicalPages = 2048; // Shadows.VirtualMaxPhysicalPages (r.Shadow.Virtual.MaxPhysicalPages)
        float ResolutionLodBias = 0.0f; // Shadows.VirtualResolutionLodBias, levels (ResolutionLodBiasDirectional)
        uint32_t SmrtRayCount   = 8;    // Shadows.VirtualSMRTRayCount (r.Shadow.Virtual.SMRT.RayCountDirectional)
        uint32_t SmrtSamplesPerRay = 4; // Shadows.VirtualSMRTSamplesPerRay (SMRT.SamplesPerRayDirectional)
    };

    // A world box whose casters changed this frame (UE FVirtualShadowMapCacheManager invalidation): a movable
    // caster's bounds now and where it stood last frame.
    struct VsmInvalidationBox
    {
        glm::vec3 Min{ 0.0f };
        glm::vec3 Max{ 0.0f };
    };

    struct VirtualShadowMapViewInputs
    {
        RDG::TextureRef Depth;  // the G-buffer's reversed-Z depth
        RDG::TextureRef Normal; // the G-buffer's world normal
        glm::mat4       InvJitteredViewProjection{ 1.0f };
        glm::mat4       Projection{ 1.0f }; // the camera's (its [1][1] gives the pixel footprint)
        glm::vec3       CameraPosition{ 0.0f };
        uint32_t        Width      = 0;
        uint32_t        Height     = 0;
        uint64_t        FrameIndex = 0;
    };

    struct VirtualShadowMapSun
    {
        glm::vec3 DirectionToSun{ 0.0f, 1.0f, 0.0f };
        float     SourceAngleDegrees = 0.0f; // DirectionalLightData::SourceAngle
    };

    // A shadowed point or spot light of the frame (SceneRenderer, from its payload). Its views keep their pages
    // while SourceId and everything the views are made of (position, direction, cone, reach) stay the same.
    struct VirtualShadowMapLocalLight
    {
        uint32_t  SourceId = 0; // LocalLightShadowSettings::SourceId (entity + 1)
        glm::vec3 Position{ 0.0f };
        float     Radius = 0.0f; // the point's radius / the spot's range, cm (the views' far plane)
        glm::vec3 SpotDirection{ 0.0f, 0.0f, -1.0f };
        float     CosOuter     = kVsmLocalPointCone; // kVsmLocalPointCone = a point light (six views)
        float     SourceRadius = 0.0f;               // cm
        // Its layer of the frame's local screen-mask array (ShadowMaskSlot). Only lights whose sphere meets the
        // view are handed in; the pages of one that is not stay cached.
        uint32_t MaskSlot = 0;
    };

    // A drawn mip of a local view: one "VSM: Raster" node of the mesh renderer, over a ResolutionPages * 128 texel
    // square, its casters drawn with ViewProjection.
    struct VsmLocalRaster
    {
        uint32_t  Level           = 0; // its level in the frame's level table (RasterLevel, AddCasterCulling)
        uint32_t  ResolutionPages = 0; // pages per side of its mip
        uint32_t  Light           = 0; // index into the frame's local lights
        uint32_t  View            = 0; // 0 for a spot, the cube face of a point light
        uint32_t  Mip             = 0;
        glm::mat4 ViewProjection{ 1.0f };
    };

    // One local light's pages as last read back (DesertCtl state, "vsm").
    struct VsmLocalLightStats
    {
        uint32_t SourceId  = 0;
        uint32_t Views     = 0; // 1 spot, 6 point
        uint32_t Requested = 0; // pages a pixel asked for
        uint32_t Rendered  = 0; // of them, cleared and drawn that frame
        uint32_t Cached    = 0; // of them, kept from an earlier frame
    };

    // What the casters' raster reads to draw this frame's dirty pages, and what the projection reads.
    struct VirtualShadowMapFrame
    {
        RDG::BufferRef  FrameData;    // VsmFrameData: VsmFrameGpu + LevelCount VsmLevelGpu
        RDG::BufferRef  PageTable;    // VsmPageTable
        RDG::TextureRef PhysicalPool; // R32_UINT, the casters' atomic max target
        // VsmRasterLevel of each level: { level index, 0, 0, 0 }, the one value a caster's fragment needs beside
        // FrameData to find its page (Mesh/Surface/Pass_VirtualShadowDepth.glslh).
        // Every level of the table: the kVsmClipmapLevelCount sun levels first, then the local views' mips.
        std::vector<RDG::BufferRef> RasterLevel;
        RDG::BufferRef              DirtyMask; // VsmDirtyMask: kVsmMaskWordsPerLevel words per level
        std::vector<glm::mat4>      LevelViewProjection;
        uint32_t                    LevelCount = 0;
        // Whether the sun's clipmap levels are in use this frame (a sun was handed to AddPagePasses).
        bool HasSun = false;
        // The local lights' drawn mips, in light order, then view, then mip.
        std::vector<VsmLocalRaster> LocalRasters;
        RDG::BufferRef              LocalLights; // VsmLocalLights: VsmLocalLightGpu per light
        uint32_t                    LocalLightCount = 0;
    };

    class VirtualShadowMapRenderer
    {
    public:
        [[nodiscard]] Common::BoolResultStr Initialize();

        // The frame's page passes, in order: Invalidate, Invalidate Casters, Mark, Gather Available Pages,
        // Allocate, Clear Args, Clear Pages. After them the casters draw the dirty pages into PhysicalPool; then
        // AddProjection.
        // @p sun: nullptr = no sun this frame (its levels hold nothing). @p locals: the shadowed point / spot
        // lights whose sphere meets the view, each with its mask layer (Mark Local, the local rasters, Project
        // Local). @p invalidations: the boxes of the casters that moved or changed (now and last frame), the
        // motion mask every level's cached pages are tested against. @p staticCastersChanged: every cached page is
        // dropped.
        [[nodiscard]] Common::ResultStr<VirtualShadowMapFrame>
        AddPagePasses( RDG::Builder& graph, const VirtualShadowMapViewInputs& view, const VirtualShadowMapSun* sun,
                       std::span<const VirtualShadowMapLocalLight> locals, const VirtualShadowMapQuality& quality,
                       std::span<const VsmInvalidationBox> invalidations, bool staticCastersChanged );

        // "VSM: Cull Casters" of level @p level (UE CullPerPageDrawCommands): every instance of @p cull whose
        // world box covers a cell of the level holding a page drawn this frame (frame.DirtyMask) is counted into
        // each of its draw's records and written to the draw's remap slots; a level with no such page draws
        // nothing. Refused for an empty @p cull (the caller adds no raster node then).
        [[nodiscard]] Common::ResultStr<VsmCulledCasters> AddCasterCulling( RDG::Builder&                graph,
                                                                            const VirtualShadowMapFrame& frame,
                                                                            uint32_t                     level,
                                                                            const VsmCasterCull& cull ) const;

        // "VSM: Project": the sun's screen shadow mask (System::kShadowMaskSunName), 1 = lit.
        [[nodiscard]] Common::ResultStr<RDG::TextureRef> AddProjection( RDG::Builder&                     graph,
                                                                        const VirtualShadowMapFrame&      frame,
                                                                        const VirtualShadowMapViewInputs& view );

        // "VSM: Project Local": every local light of @p frame into its layer of @p masks (the 2D-array screen mask
        // RayTracedLocalShadowRenderer::CreateMasks makes, read by the composite through ShadowMaskSlot).
        [[nodiscard]] Common::BoolResultStr AddLocalProjection( RDG::Builder&                     graph,
                                                                const VirtualShadowMapFrame&      frame,
                                                                const VirtualShadowMapViewInputs& view,
                                                                RDG::TextureRef                   masks );

        // The local lights' pages of the newest frame read back (one entry per light of that frame; empty before).
        [[nodiscard]] const std::vector<VsmLocalLightStats>& GetLocalLightStats() const
        {
            return m_LocalStats;
        }

    private:
        enum Kernel : uint32_t
        {
            Invalidate = 0,
            InvalidateCasters,
            Mark,
            Gather,
            Allocate,
            ClearArgs,
            Clear,
            Project,
            DirtyMask,
            CullCasters,
            MarkLocal,
            ProjectLocal,
            KernelCount
        };
        struct KernelProgram
        {
            std::shared_ptr<Graphic::Shader> Program;
            std::shared_ptr<ComputePipeline> Pipeline;
            mutable ShaderBindingLayoutCache Layout;
        };

        // The pool, the page table and the owners: made on the first frame and again when the pool's size changes
        // (every page then starts unmapped).
        [[nodiscard]] bool AcquirePersistent( RDG::Builder& graph, uint32_t physicalPages, uint32_t localViews,
                                              RDG::TextureRef& pool, RDG::BufferRef& pageTable,
                                              RDG::BufferRef& owners );
        // Places the local views of @p locals in the table (a light keeps its view slots while it stays; a changed
        // one keeps them and is reset), fills the frame's VsmLocalLightGpu and rasters. Returns the view capacity
        // the table needs (>= m_LocalViews; more makes the table again).
        uint32_t PlaceLocalLights( std::span<const VirtualShadowMapLocalLight> locals, const glm::vec3& camera,
                                   float pixelFootprint, float lodBias, uint64_t frameIndex,
                                   std::vector<VsmLocalLightGpu>& gpuLights,
                                   std::vector<VsmLocalRaster>&   rasters );
        // The levels of every local view slot (from kVsmClipmapLevelCount on): unused slots have no pages.
        void WriteLocalLevels( std::vector<VsmLevelGpu>& levels, bool resetAll ) const;
        // The page counts of the frame that last used @p slot, per light (m_LocalStats).
        void ReadStats( uint32_t slot );
        // This frame's clipmap: fills @p levels and says which levels' depth ranges moved; true when the light
        // basis changed (every page is dropped).
        bool UpdateClipmap( const glm::vec3& directionToSun, const glm::vec3& camera,
                            std::array<VsmLevelGpu, kVsmClipmapLevelCount>& levels );

        // The pool overflow of the frame that last used @p slot (its fence was waited before this frame recorded),
        // logged once per change of the count, naming Shadows.VirtualMaxPhysicalPages (UE prints the same warning
        // from its VSM stats readback).
        void ReportOverflow( uint32_t slot );

        std::array<KernelProgram, KernelCount> m_Kernels;

        // "VSM: Allocate"'s counters, extracted for the host once per frame-in-flight slot, and the pool size that
        // frame had.
        std::vector<RDG::ExternalBuffer> m_AllocatorReadback;
        std::vector<uint32_t>            m_ReadbackPhysicalPages;
        uint32_t                         m_ReportedOverflow = 0;

        RDG::ExternalTexture m_Pool;
        RDG::ExternalBuffer  m_PageTable;
        RDG::ExternalBuffer  m_Owners;
        uint32_t             m_PhysicalPages = 0; // 0 = not made yet
        uint32_t             m_LocalViews    = 0; // the table's room for local views

        // THE LOCAL VIEWS IN THE TABLE: per light, its first view slot and what its views were drawn from.
        struct LocalPlacement
        {
            uint32_t                 SourceId  = 0;
            uint32_t                 FirstView = 0;
            uint32_t                 Views     = 0;
            std::array<glm::mat4, 6> ViewProjection{};
            float                    Near        = 0.0f;
            float                    Far         = 0.0f;
            float                    TexelAtUnit = 0.0f;
            uint64_t                 LastFrame   = 0;
            uint32_t                 FrameLight = kVsmInvalid; // its index among this frame's lights, if handed in
            bool                     Reset      = true;        // its levels are reset this frame
        };
        std::vector<LocalPlacement> m_LocalPlacements;

        // Per frame-in-flight slot: the level counters "VSM: Dirty Mask" counted that frame, and the light
        // (SourceId, view count) each of its levels belonged to ({0, 0}: the sun's or none).
        std::vector<RDG::ExternalBuffer>                        m_StatsReadback;
        std::vector<std::vector<std::pair<uint32_t, uint32_t>>> m_StatsLevelLights;
        std::vector<VsmLocalLightStats>                         m_LocalStats;

        std::optional<glm::vec3>                 m_LightDirection; // the basis the cached pages were drawn in
        std::array<float, kVsmClipmapLevelCount> m_DepthCentres{};
        std::array<bool, kVsmClipmapLevelCount>  m_DepthCentresValid{};
    };
} // namespace Desert::Graphic::System
