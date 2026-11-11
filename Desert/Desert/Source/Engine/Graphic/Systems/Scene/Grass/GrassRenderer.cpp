// Ported from UE 5.8 Engine/Source/Runtime/Landscape/Private/LandscapeGrass.cpp:2032-2080 (FGrassBuilderBase: the
// grid size from GrassDensity) and :2378-2520 (FAsyncGrassBuilder::Build, its grid branch), adapted: the instances
// are built on the GPU once per world tile of a world-aligned grid around the view and kept while the tile is
// within the cull radius (GRASS-TILE-1, Programs/Grass/GrassGenerate.shader; UE builds them on worker threads per
// landscape component into HISM tiles), culled per view into the drawn lists, and drawn by indirect draws through
// the mesh renderer's instanced cells (GpuInstances.hpp) instead of a HISM component.
#include <Engine/Graphic/Systems/Scene/Grass/GrassRenderer.hpp>

#include <Common/Core/JobSystem.hpp>
#include <Common/Core/Profiler.hpp>
#include <Engine/Core/Device.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Core/Frustum.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/DrawCounters.hpp>
#include <Engine/Graphic/MeshDrawArguments.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>
#include <Engine/Graphic/TextureStreamingDemand.hpp>
#include <Engine/Graphic/VisibilityCulling.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_set>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr uint32_t kGrassGroupSize       = 8; // GrassGenerate's LocalSize
        constexpr uint32_t kFlagRandomRotation   = 1u;
        constexpr uint32_t kFlagAlignToSurface   = 2u;
        constexpr uint32_t kFlagWeightAttenuates = 4u;
        constexpr uint32_t kFlagBlade            = 8u;
        // GRASS-TILE-1, the tile cache (GrassGenerate.shader kTile*, kMode*): a tile's side in cells, a page's
        // records, a tile slot's header words, a record's bytes, the feedback words.
        constexpr uint32_t kGrassTileCells         = 32u;
        constexpr uint32_t kGrassTilePageRecords   = 64u;
        // GRASS-TILE-2: a tile's clusters (GrassGenerate.shader kTileClusters), each one thread group's cells - of the
        // build and of the tile cull (GRASS-TILE-4: one group per cluster, one thread per record); the tile's header
        // is 32 words of the tile, then 8 words per cluster (kTileClusterWord).
        constexpr uint32_t kGrassTileClusters =
             ( kGrassTileCells / kGrassGroupSize ) * ( kGrassTileCells / kGrassGroupSize );
        constexpr uint32_t kGrassTileHeaderWords   = 32u + kGrassTileClusters * 8u;
        constexpr uint32_t kGrassTileRecordBytes   = 3u * sizeof( glm::uvec4 );
        constexpr uint32_t kGrassTileFeedbackWords = 4u;
        constexpr uint32_t kGrassTileModeInit      = 0u;
        constexpr uint32_t kGrassTileModeFree      = 1u;
        constexpr uint32_t kGrassTileModeCount     = 2u;
        constexpr uint32_t kGrassTileModeAllocate  = 3u;
        constexpr uint32_t kGrassTileModeWrite     = 4u;
        static_assert( kGrassTileCells % kGrassGroupSize == 0u, "a tile is whole thread groups of cells" );
        // UE grass.MaxCreatePerFrame: the tiles a variety builds in one frame, nearest first (the rest wait). A
        // build is two passes of one thread per cell; the view settles within N / 64 frames of N tiles.
        constexpr uint32_t kGrassTileBuildsPerFrame = 64u;
        // GRASS-VRAM-BUDGET-1, THE POOL'S SEGMENTS (Common/GrassVarietyRows.glslh kGrassTileSegmentPages): the pool
        // grows by segments of this many pages (3 MB), each its own buffer; a tile holds at most kGrassTileMostPages.
        constexpr uint32_t kGrassTileSegmentPages = 1024u;
        constexpr uint32_t kGrassTileMostPages    = kGrassTileCells * kGrassTileCells / kGrassTilePageRecords;
        constexpr uint64_t kGrassTilePageBytes    = uint64_t{ kGrassTilePageRecords } * kGrassTileRecordBytes;
        constexpr uint64_t kGrassTileSegmentBytes = kGrassTilePageBytes * kGrassTileSegmentPages;
        static_assert( kGrassTileSegmentBytes <= UINT32_MAX );
        // UE r.Streaming.PoolSize, a pool's ceiling inside the device's budget: the share of the device-local budget
        // every variety's tile pools together may hold.
        constexpr double kGrassTilePoolBudgetShare = 0.1;
        // @p pages rounded up to whole segments.
        constexpr uint32_t SegmentPages( const uint32_t pages )
        {
            return ( pages + kGrassTileSegmentPages - 1u ) / kGrassTileSegmentPages * kGrassTileSegmentPages;
        }

        // CPU-GRASS-POOL-2: tile (x, z)'s cell of a ring grid @p side cells wide (VarietyGpu::TileRing).
        uint32_t RingCell( const int32_t x, const int32_t z, const uint32_t side )
        {
            const auto wrap = [side]( const int32_t v )
            {
                const int32_t m = v % static_cast<int32_t>( side );
                return static_cast<uint32_t>( m < 0 ? m + static_cast<int32_t>( side ) : m );
            };
            return wrap( z ) * side + wrap( x );
        }
        // CPU-GRASS-POOL-2: GrassGeneratePush::Base of a variety (GrassRenderer::VarietyGpu): its header entry in
        // the shared tile table and its first word of the shared tile feedback; CPU-GRASS-POOL-3: z its first word
        // of the shared tile pool words (GrassRenderer m_TileWordsPool).
        // CPU-GRASS-POOL-7: GrassGeneratePush::Ranges of a variety: its params record, its first chunks word and
        // its first segment table entry in the shared pools (GrassRenderer m_ParamsPool / m_ChunksPool /
        // m_SegmentTablesPool).
        template <typename Gpu>
        glm::uvec4 TileRanges( const Gpu& gpu )
        {
            const uint64_t record = static_cast<uint64_t>( gpu.ParamsWords.size() ) * 4u;
            return glm::uvec4( static_cast<uint32_t>( gpu.Params.Offset / record ),
                               static_cast<uint32_t>( gpu.Chunks.Offset / 4u ),
                               static_cast<uint32_t>( gpu.TilePool.Offset / sizeof( uint64_t ) ), 0u );
        }
        template <typename Gpu>
        glm::uvec4 TileBase( const Gpu& gpu )
        {
            return glm::uvec4( gpu.TileTableBase, gpu.FeedbackSlot * kGrassTileFeedbackWords,
                               static_cast<uint32_t>( gpu.TileWords.Offset / 4u ), 0u );
        }
        // GpuInstanceBucket: count, first, capacity, and a mesh level's half screen size (bit pattern; 0 for a blade).
        constexpr uint32_t kBucketWords = 4u;
        // GRASS-VIS: CameraArgs' words per bucket after its bucket table (GrassGenerate.shader CameraArgs):
        // candidates appended, candidates frozen, phase 1's drawn, phase 2's appended.
        constexpr uint32_t kPhaseWords = 4u;
        // GRASS-VIS-7: Feedback's words per variety past its bucket counts (GrassCull.shader FinalizeFull): the
        // written mark, the full list's drawn count, the camera's phase-1 drawn count, phase 2's appended count.
        // GPU-COST-21: then one drawn count per cascade view's list (GrassCull.shader FinalizeCascades).
        // GRASS-CULL-COST-3: then the law's slots handed out this law generation, uncapped (GrassCull.shader
        // kFeedbackLawWord; Instances is sized from it).
        constexpr uint32_t kFeedbackLawWord   = 4u + kGrassCascadeLists;
        constexpr uint32_t kFeedbackTailWords = kFeedbackLawWord + 1u;
        static_assert(
             kGrassCascadeLists == MeshRenderer::kCascadeViews &&
                  kGrassCascadeLists == GrassDrawCounters::kCascadeViews && kGrassCascadeLists == 8u,
             "GrassCull.shader VarietyCascadePlanes[48] and FinalizeCascades' eight views are sized by it" );
        // A camera candidate's two uvec4 (GrassGenerate.shader Candidates): its sphere and its row of Instances.
        constexpr uint32_t kCandidateBytes = 32u;
        // GPU-COST-22: one slot of the camera's list or a cascade's (GrassGenerate.shader CameraIds / CascadeIds,
        // the vertex stages' GpuInstanceIds): the row of Instances it draws. An instance's mat4 lies in Instances
        // alone.
        constexpr uint32_t kGrassIdBytes = sizeof( uint32_t );
        static_assert( kGrassIdBytes == 4u && sizeof( glm::mat4 ) == 64u,
                       "GrassGenerate.shader: uint GrassCameraIdsData[] / GrassCascadeIdsData[], mat4 "
                       "GrassInstancesData[]" );
        // GRASS-CULL-2 (UE GPUScene / InstanceCulling: one table describes every primitive's instances, and one
        // dispatch culls them all): THE VARIETY TABLE GrassCull reads (Common/GrassVarietyRows.glslh, std430), written
        // once per frame by WriteVarietyTable - the camera's side planes, then one row per culled variety
        // (m_FrameCull). A row names the variety's buffers by
        // device address (StorageBuffer::GetDeviceAddress: the copies this frame imports) and carries what the cull
        // reads of its params, so a pass over every variety binds the table once.
        struct GrassVarietyRowGpu
        {
            glm::uvec2 Instances;
            glm::uvec2 Args;
            glm::uvec2 CameraIds;
            glm::uvec2 CameraArgs;
            glm::uvec2 Candidates;
            glm::uvec2 PostArgs;
            glm::uvec2 Feedback;
            glm::uvec2 CascadeArgs;
            // x = bucket count, y = the bucket table's first word in Args, z = flags (kRowBlade), w = the cascade
            // views with a list (VarietyGpu::CascadeMask)
            glm::uvec4 Info;
            glm::vec4  Bounds; // VarietyGpu::CullBounds
            // GRASS-CULL-UNIFY-1, the cascades' lists GrassCull culls every frame: CascadeIds' address, x = one
            // list's words (VarietyGpu::CascadeArgWords), y = its first rest count, and list k's ring at [k / 2]
            // .xy / .zw (VarietyGpu::CascadeRings).
            glm::uvec2 CascadeIds;
            glm::uvec2 CascadeWords;
            glm::vec4  CascadeRings[kGrassCascadeLists / 2u];
            // GRASS-VIEW-LAW-1, what the view law reads and writes (Common/GrassVarietyRows.glslh, the chunk
            // contract): the build's chunks (VarietyGpu::Chunks, its header holds the room), the variety's params,
            // the field, the tile pool and the full list's ids. std430 rounds the GLSL row to its 16-byte
            // alignment, which Pad spells out.
            glm::uvec2 Chunks;
            glm::uvec2 Params;
            glm::uvec2 Field;
            glm::uvec2 TilePool;
            glm::uvec2 FullIds;
            // GRASS-CULL-COST-3, THE LAW'S SLOTS (GrassCull.shader LawChunk): the law generation
            // (VarietyGpu::LawGeneration, never 0: a chunk the build wrote carries 0) and the slots of Instances.
            uint32_t   LawGeneration = 0u;
            uint32_t   LawCapacity   = 0u;
            // CPU-GRASS-POOL-5: the ranges' first row of Instances and first slot of each id list in their pools
            // (GrassCull.shader adds them to the ids it writes and to the records' firstInstance).
            uint32_t   InstancesBase  = 0u;
            uint32_t   FullIdsBase    = 0u;
            uint32_t   CameraIdsBase  = 0u;
            uint32_t   CascadeIdsBase = 0u;
            // CPU-GRASS-POOL-7: the mark FinalizeFull writes into Feedback (VarietyGpu::FeedbackMark).
            uint32_t   FeedbackMark = 0u;
            uint32_t   Pad[3]       = {};
        };
        static_assert(
             sizeof( GrassVarietyRowGpu ) == 256,
             "Common/GrassVarietyRows.glslh GrassVarietyRow: 8 uvec2, uvec4 Info at 64, vec4 Bounds at 80, "
             "uvec2 CascadeIds at 96, uvec2 CascadeWords at 104, vec4 CascadeRings[4] at 112, uvec2 Chunks at 176, "
             "Params at 184, Field at 192, TilePool at 200, FullIds at 208, uint LawGeneration at 216, uint "
             "LawCapacity at 220, uint InstancesBase at 224, FullIdsBase at 228, CameraIdsBase at 232, "
             "CascadeIdsBase at 236, FeedbackMark at 240, std430's 12 bytes of padding at 244" );
        struct GrassVarietyTableGpu
        {
            glm::vec4 CameraPlanes[4]; // the camera's side planes (ViewVolumes::Camera): the camera cull's
            // GRASS-CULL-UNIFY-1: the cascades' caster boxes (ViewVolumes::CascadePlanes) and the shadow wind
            // rule's origin (MeshRenderer::CasterWindOrigin of the first list's view): GrassCull's CascadeCull.
            glm::vec4 CascadePlanes[kGrassCascadeLists * 6u];
            glm::vec4 CasterWindOrigin;
        };
        static_assert( sizeof( GrassVarietyTableGpu ) == 848,
                       "GrassCull.shader Varieties: VarietyCameraPlanes[4], "
                       "VarietyCascadePlanes[48], VarietyCasterWindOrigin before the rows" );
        // GrassVarietyRows.glslh kGrassRowBlade: a blade strip's records, sets of one record per bucket.
        constexpr uint32_t kRowBlade = 1u;
        // GrassVarietyRows.glslh kGrassRowCascadesHeld: the row's cascade lists hold from an earlier frame
        // (VarietyGpu::CascadeKey), GrassCull neither culls nor finalizes them.
        constexpr uint32_t kRowCascadesHeld = 2u;
        // GrassVarietyRows.glslh kGrassRowCameraHeld: with its cascades, the row's camera list and candidates hold
        // from an earlier frame (VarietyGpu::CameraKey); GrassCull gives the row no camera cull groups.
        constexpr uint32_t kRowCameraHeld = 4u;
        // GrassVarietyRows.glslh kGrassChunkHeader / kGrassChunkWords (the chunk contract): Chunks' header (x = the
        // chunks, one per tile slot and cluster, y = the law's slots handed out, z = the room, w = their law
        // generation; GrassCull writes y and w) and the words of one chunk.
        constexpr uint32_t kChunkHeaderWords = 4u;
        // GRASS-CULL-COST-3: words 10..25 are the camera cull's law words (the generation, the kept box, the codes,
        // the first slot of Instances).
        constexpr uint32_t kChunkWords = 26u;
        // GrassVarietyRows.glslh kGrassLawBuckets: a chunk's law words keep each record's bucket + 1 in four bits.
        constexpr uint32_t kLawBuckets = 15u;
        // GrassCull's dispatch buffer (GrassCull.shader CullDispatch, a buffer of this frame's graph): the camera
        // cull's and phase 2's VkDispatchIndirectCommand, each on a uvec4 (w: its groups), then per culled row a
        // uvec4 of its first groups. The CPU never reads it.
        constexpr uint32_t kCullDispatchHeader = 2u;
        constexpr uint64_t kPostDispatchOffset = sizeof( glm::uvec4 );
        // GrassCull.shader's modes (Hzb.w).
        constexpr uint32_t kCullModeDispatch       = 0u;
        constexpr uint32_t kCullModeCameraCull     = 1u;
        constexpr uint32_t kCullModeCameraFinalize = 2u;
        constexpr uint32_t kCullModePostOcclusion  = 3u;
        constexpr uint32_t kCullModePostFinalize   = 4u;
        constexpr uint32_t kCullModeCameraInstances = 5u;
        // GRASS-CULL-COST-4, the met chunks' list (GrassCull.shader MetChunks, a buffer of this frame's graph):
        // the instance pass's VkDispatchIndirectCommand, the met chunks' and the work items' counts on two uvec4,
        // then a uvec2 per met chunk, room for every culled row's chunks, then (FPS-GRASS-CULL-GPU) a uint per
        // work item - a kept record of a met chunk that holds a law row, room for every culled row's LawCapacity
        // (each such record owns one of its row's law rows). The CPU never reads it.
        constexpr uint64_t kMetHeaderBytes = 2u * sizeof( glm::uvec4 );
        constexpr uint64_t kMetEntryBytes  = sizeof( glm::uvec2 );
        constexpr uint64_t kWorkItemBytes  = sizeof( uint32_t );
        // A work item names its met chunk above GrassCull.shader kWorkRecordBits (6): the list's room stays below.
        constexpr uint64_t kMetRoomLimit = uint64_t( 1u ) << 26u;

        // GRASS-CULL-HIER-1, THE CASCADES' GUARD BAND (UE's cached shadow views keep their culled casters while the
        // view's cached region still covers the moved view): a camera that only turns carries each cascade's box -
        // fitted to its slice of the view - about the camera; a turn of kCascadeGuardTurn moves the slice's centre
        // by the chord 2 d sin( turn / 2 ) of its distance d from the camera. d is measured across the light, from
        // the box's centre - which is the slice's across the light, while along it a caster box reaches toward the
        // light past its slice - so a view along the light grows the box less and re-grows it sooner. GrassCull
        // culls a list against its box grown by that chord on every side (ViewVolumes::CascadePlanes), and the grown
        // box stays while each later box lies inside it, so a turning camera re-culls a cascade once per
        // kCascadeGuardTurn of turn, not on every texel the cascade scrolls. What the grown box adds past the
        // cascade's own falls outside its map: the rasterizer drops it, the picture does not change.
        constexpr float kCascadeGuardTurn = 0.5235988f; // 30 degrees, rad
        // Frustum.hpp's planes in opposite pairs: near / far, left / right, up / down.
        constexpr uint32_t kBoxPlanePairs[3][2] = { { Core::PLANE_NEAR, Core::PLANE_FAR },
                                                    { Core::PLANE_LEFT, Core::PLANE_RIGHT },
                                                    { Core::PLANE_UP, Core::PLANE_DOWN } };

        // The eight corners of the box six planes bound (xyz.n + w >= 0 inside, Frustum.hpp's order), each where one
        // plane of every pair meets; false for a box whose planes do not meet in a point (an unbuilt one).
        bool BoxCorners( const glm::vec4 ( &planes )[6], glm::vec3 ( &corners )[8] )
        {
            for ( uint32_t corner = 0; corner < 8u; ++corner )
            {
                const glm::vec4& a   = planes[kBoxPlanePairs[0][corner & 1u]];
                const glm::vec4& b   = planes[kBoxPlanePairs[1][( corner >> 1u ) & 1u]];
                const glm::vec4& c   = planes[kBoxPlanePairs[2][( corner >> 2u ) & 1u]];
                const glm::vec3  bc  = glm::cross( glm::vec3( b ), glm::vec3( c ) );
                const glm::vec3  ca  = glm::cross( glm::vec3( c ), glm::vec3( a ) );
                const glm::vec3  ab  = glm::cross( glm::vec3( a ), glm::vec3( b ) );
                const float      det = glm::dot( glm::vec3( a ), bc );
                if ( !( std::abs( det ) > 0.0f ) )
                    return false;
                corners[corner] = -( a.w * bc + b.w * ca + c.w * ab ) / det;
            }
            return true;
        }

        // Whether the box @p planes bound lies inside the box @p guard bounds (every corner on the inner side of
        // every guard plane).
        bool BoxInsideGuard( const glm::vec4 ( &planes )[6], const glm::vec4 ( &guard )[6] )
        {
            glm::vec3 corners[8];
            if ( !BoxCorners( planes, corners ) )
                return false;
            for ( const glm::vec3& corner : corners )
            {
                for ( const glm::vec4& plane : guard )
                {
                    if ( glm::dot( glm::vec3( plane ), corner ) + plane.w < 0.0f )
                        return false;
                }
            }
            return true;
        }

        // The guard box of the cascade box @p planes for a camera at @p camera: each plane pushed out by the chord
        // a kCascadeGuardTurn turn moves the box across the light (the near plane's normal). A box without corners
        // is its own.
        void GuardBox( const glm::vec4 ( &planes )[6], const glm::vec3& camera, glm::vec4 ( &guard )[6] )
        {
            std::copy( std::begin( planes ), std::end( planes ), std::begin( guard ) );
            glm::vec3 corners[8];
            if ( !BoxCorners( planes, corners ) )
                return;
            glm::vec3 centre( 0.0f );
            for ( const glm::vec3& corner : corners )
                centre += corner * 0.125f;
            const glm::vec3 light  = glm::normalize( glm::vec3( planes[Core::PLANE_NEAR] ) );
            const glm::vec3 offset = centre - camera;
            const glm::vec3 across = offset - light * glm::dot( offset, light );
            const float     chord  = 2.0f * glm::length( across ) * std::sin( 0.5f * kCascadeGuardTurn );
            for ( glm::vec4& plane : guard )
                plane.w += chord * glm::length( glm::vec3( plane ) );
        }

        // Mirrors GrassGenerate's GrassParams (std430).
        struct GrassParamsGpu
        {
            // GRASS-CULL-UNIFY-1: no view volume and no cascade in it - the tile cull keeps the field about the
            // field eye (FieldEye: xyz), the same for every view and every turn of the camera.
            glm::vec4  Camera;
            glm::vec4  Cull;
            glm::vec4  Range;
            glm::vec4  ScaleX;
            glm::vec4  ScaleY;
            glm::vec4  ScaleZ;
            glm::uvec4 Info;
            glm::uvec4 Buckets; // x = bucket count, y = the bucket table's first word in Args
            glm::vec4  Blade;   // x = LodDistance (0: one bucket), y = ClumpSize, z = Clumping, w = ClumpPull
            // x = Scatter (cells), y = Patchiness, z = PatchSize, w = the screen-width floor's distance (cm, 0: none)
            glm::vec4  Spread;
            // x = Tilt (radians), y = FacingJitter (radians), z = the world's SnowDepth (cm), w = the screen-width
            // floor's width per unit of distance (MinScreenWidth / pixels per unit; 0: none)
            glm::vec4  Lean;
            // A mesh's per-instance level by screen size (Geometry::BoundsScreenRadiusSquared): x = the lens's
            // screen multiple x the mesh's local half-diagonal, y = |Projection[2][3]| (0: orthographic). The far
            // LOD of a blade (GPU-FRAME-4) or a mesh (GPU-FRAME-6): z = the distance at which its bounding radius
            // at scale 1 stands at the Foliage quality's LodScreenPixels (0: none; GrassFieldShare.glslh
            // GrassBladeLodAt, GrassMeshFarLod). w, a blade: the distance its coarsest level's instances draw
            // from as the small bucket (BladeStrip's BladeSmall; 0: none); a mesh: 0.
            glm::vec4 Lod;
            // The variety's world patches: x = Coverage (1: none), y = Size cm, z = Softness, w = the noise seed's
            // bits (shared by a Group).
            glm::vec4 Patches;
            // A blade's cards' widths, cm at scale 1 (card i at [i / 4][i % 4], Buckets.z of them): the card an
            // instance draws (Mesh/Surface/GrassBlade.glslh BladeCard) is lifted to the screen-width floor.
            glm::vec4 CardWidths[2];
        };
        static_assert( sizeof( GrassParamsGpu ) == 240 );

        // Instances whose cell centre lies within [inner, outer) of the camera, the ring widened on each side by
        // one jittered cell and by @p moved, the farthest a blade's scatter and clump pull carry it from its cell.
        uint32_t RingCapacity( double inner, double outer, double cell, double moved )
        {
            const double lo = std::max( 0.0, inner - 1.5 * cell - moved );
            const double hi = outer + 1.5 * cell + moved;
            return static_cast<uint32_t>( std::ceil( 3.14159265 * ( hi * hi - lo * lo ) / ( cell * cell ) ) );
        }
        // A bucket's capacity rounded up to the next power of sqrt(2). The lens changes every frame while a scope
        // zooms, and an exact capacity would reallocate the variety's buffers on every one of those frames; on the
        // steps it reallocates about seven times across a 90-to-12-degree zoom and never wastes more than 41 %.
        uint32_t CapacityStep( uint32_t needed )
        {
            if ( needed == 0u )
                return 0u;
            const double step = std::exp2( 0.5 * std::ceil( 2.0 * std::log2( static_cast<double>( needed ) ) ) );
            return std::max( needed, static_cast<uint32_t>( std::ceil( step ) ) );
        }

        // UE FGrassBuilderBase: SqrtMaxInstances = sqrt(Extent^2 * GrassDensity / 1000 / 1000), i.e. one instance
        // per (1e6 / Density) cm^2 - the grid cell's side.
        float CellSizeCm( const GrassPlacement& placement )
        {
            return std::sqrt( 1.0e6f / placement.Density );
        }

        // The variety's placement under the view's Foliage quality (UE: grass.DensityScale x GrassDensity before
        // FGrassBuilderBase sizes its grid, grass.CullDistanceScale x the Start / End cull distances): fewer, larger
        // cells, and the thinning and the end pulled in by the same factor.
        GrassPlacement ScaledPlacement( const GrassPlacement& authored, const GrassQuality& quality )
        {
            GrassPlacement placement = authored;
            placement.Density *= quality.DensityScale;
            placement.StartCullDistance *= quality.CullDistanceScale;
            placement.EndCullDistance *= quality.CullDistanceScale;
            return placement;
        }

        // The view's four side planes (Gribb-Hartmann on the unjittered ViewProjection), normalised; the near and
        // far planes are left to the cull distance, so the depth convention does not matter.
        void SidePlanes( const glm::mat4& viewProjection, glm::vec4 ( &planes )[4] )
        {
            const glm::vec4 row0( viewProjection[0][0], viewProjection[1][0], viewProjection[2][0],
                                  viewProjection[3][0] );
            const glm::vec4 row1( viewProjection[0][1], viewProjection[1][1], viewProjection[2][1],
                                  viewProjection[3][1] );
            const glm::vec4 row3( viewProjection[0][3], viewProjection[1][3], viewProjection[2][3],
                                  viewProjection[3][3] );
            const glm::vec4 raw[4] = { row3 + row0, row3 - row0, row3 + row1, row3 - row1 };
            for ( int i = 0; i < 4; ++i )
                planes[i] = raw[i] / glm::length( glm::vec3( raw[i] ) );
        }

        // GRASS-CULL-UNIFY-1, THE FIELD EYE (UE: a landscape component's grass is built once and kept while the
        // component is within the cull distance - it does not follow the view): the camera snapped to the centre
        // of the tile grid's cell it stands in (@p side = one tile, cm), on all three axes. The tile cull's
        // distances, thinning and levels are measured from it, so its instances change only when it steps into the
        // next cell - the frame the resident tiles change (UpdateTiles is given the same eye) - never on a turn.
        // CSM-SCROLL-3: a cascade thins its blades from it too (GpuInstanceBladeShadowLod::ThinOrigin), so a cached
        // cascade's static layer holds while the eye stays in a tile and scrolls.
        glm::vec3 FieldEye( const glm::vec3& camera, const float side )
        {
            return ( glm::floor( camera / side ) + 0.5f ) * side;
        }

        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct GrassGenerateShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, Field );
            DESERT_SHADER_PARAMETER( StorageBuffer, Params );
            DESERT_SHADER_PARAMETER( StorageBuffer, TilePool );
            DESERT_SHADER_PARAMETER( StorageBuffer, TileWords );
            DESERT_SHADER_PARAMETER( StorageBuffer, Tiles );
            DESERT_SHADER_PARAMETER( StorageBuffer, TileFeedback );
            DESERT_SHADER_PARAMETER( StorageBuffer, Chunks );
        };

        // RENDER-JOB-CPU-4: Programs/Grass/GrassDrawCompact.shader's slots and push block (std430).
        struct GrassDrawCompactShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, CompactTable );
            DESERT_SHADER_PARAMETER( StorageBuffer, CompactCommands );
            DESERT_SHADER_PARAMETER( StorageBuffer, CompactDrawRows );
            DESERT_SHADER_PARAMETER( StorageBuffer, CompactCounts );
        };
        struct GrassDrawCompactPush
        {
            glm::uvec4 Compact{ 0u }; // x entries, y the longest entry's Count, z bins, w 0 clear / 1 gather
        };
        static_assert( sizeof( GrassDrawCompactPush ) == 16 );
        // The bins' table: at least kDrawBinMinSlots, at most kDrawBinMaxEntries (the gather's dispatch y, a row
        // per entry; a table holds no more slots than its entries can name). A frame a section did not fit
        // doubles it up to that bound (GpuDrawBinFrame::Overflow).
        inline constexpr uint32_t kDrawBinMinSlots   = 1024u;
        inline constexpr uint32_t kDrawBinMaxEntries = 65535u;

        uint64_t BufferAddress( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer )
        {
            if ( buffer == nullptr )
                return 0u;
            const Common::ResultStr<uint64_t> address = buffer->GetDeviceAddress();
            if ( !address )
                return 0u;
            return address.GetValue();
        }

        // GrassGenerate's push block, std430 (Programs/Grass/GrassGenerate.shader PushConstants).
        struct GrassGeneratePush
        {
            // x = kGrassTileMode*, y = the mode's first entry in the tile table, z = its entries (Init: the tile
            // slots), w = the pool's pages
            glm::uvec4 Mode{ 0u };
            // GRASS-VRAM-BUDGET-1: x = the free-page stack's room (VarietyGpu::TileRoom), y = the first page this
            // frame's growth adds, z = the pages it adds (Free), w = 0
            glm::uvec4 Pool{ 0u };
            // CPU-GRASS-POOL-2: x = the variety's header entry in the shared tile table
            // (VarietyGpu::TileTableBase; Mode.y counts from the table's start), y = its first word of the shared
            // tile feedback, zw = 0
            glm::uvec4 Base{ 0u };
            // CPU-GRASS-POOL-7, the variety's ranges of the shared pools: x = its params record of the params
            // pool, y = its first word of the chunks pool, z = its first entry of the segment tables pool, w = 0
            glm::uvec4 Ranges{ 0u };
        };
        static_assert( sizeof( GrassGeneratePush ) == 64 );

        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).

        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct GrassCullShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, Varieties );
            DESERT_SHADER_PARAMETER( StorageBuffer, CullDispatch );
            DESERT_SHADER_PARAMETER( StorageBuffer, MetChunks );
            DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
            DESERT_SHADER_PARAMETER( Texture, GrassHzbTexture );
        };

        // GrassCull's push block, std430 (Programs/Grass/GrassCull.shader PushConstants).
        struct GrassCullPush
        {
            glm::mat4  HzbViewProjection{ 1.0f }; // unjittered, what the bound HZB was built through
            glm::uvec4 Hzb{ 0u };  // xy = the HZB's mip-0 size, z = its mip count (0: nothing tested), w = kCullMode*
            glm::uvec4 Rows{ 0u }; // x = the variety table's rows the pass covers (its first), y / z = MetChunks'
                                   // rooms for met chunks / work items (GrassCull.shader MetRoom / WorkRoom)
        };
        static_assert( sizeof( GrassCullPush ) == 96 );

        // A pass over the variety table that runs ONE group (GrassCull's dispatch and finalize
        // modes): its one binding block, @p push, one dispatch.
        Common::BoolResultStr DispatchOneGroup( RDG::PassContext& context, const ComputePipeline& pipeline,
                                                const void* push, const size_t bytes )
        {
            RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            bindings.PushConstants( push, static_cast<uint32_t>( bytes ) );
            return Renderer::DispatchCompute( bindings, pipeline, 1, 1, 1 );
        }

        // WHAT A VARIETY ASKS OF ITS TEXTURES (TEX-STREAMING). UE streams a grass HISM's textures like any
        // primitive's: the distance from the view to the component's bounds, the mesh's size at that distance
        // (FRenderAssetInstanceView). The instances live only on the GPU here, so the nearest one is taken where
        // it can grow: the field's ground nearest the camera (its sample under the eye, the eye clamped onto the
        // field), out to the variety's EndCullDistance through this lens. Without it every grass texture kept the
        // tail it was loaded with (mip 4 of a 2K atlas) at the camera's feet: a near patch of thin grey stalks.
        void NoteVarietyTextureDemand( const GrassVarietyDraw& variety, const Desert::Mesh& mesh,
                                       const GrassFieldData& field, const ViewFrame& frame,
                                       const float endCullDistance )
        {
            const std::vector<uint32_t>& w          = field.Words;
            const uint32_t               viewHeight = frame.Split.Output.Height;
            if ( w.size() < 8 || w[0] == 0 || w[1] == 0 || viewHeight == 0 )
                return;
            const glm::vec3 eye     = frame.CameraPosition;
            const glm::vec2 origin  = { std::bit_cast<float>( w[4] ), std::bit_cast<float>( w[5] ) };
            const float     spacing = std::bit_cast<float>( w[6] );
            if ( !( spacing > 0.0f ) || w.size() < 8ull + static_cast<uint64_t>( w[0] ) * w[1] )
                return;
            const auto sampleOf = [&]( const float at, const float from, const uint32_t count )
            {
                const float i = std::round( ( at - from ) / spacing );
                return static_cast<uint32_t>( std::clamp( i, 0.0f, static_cast<float>( count - 1 ) ) );
            };
            const uint32_t sx     = sampleOf( eye.x, origin.x, w[0] );
            const uint32_t sz     = sampleOf( eye.z, origin.y, w[1] );
            const float    height = std::bit_cast<float>( w[8 + static_cast<size_t>( sz ) * w[0] + sx] );
            // No tile loaded under the nearest sample: the ground is not known, so the eye's own height stands for
            // it (the nearest an instance can be is then the camera itself - all the mips, never too few).
            const glm::vec3         ground = { origin.x + static_cast<float>( sx ) * spacing,
                                       height == kGrassNoHeight ? eye.y : height,
                                               origin.y + static_cast<float>( sz ) * spacing };
            const Geometry::LODView view{ eye, frame.Projection };
            if ( glm::length( ground - eye ) > endCullDistance * DrawDistanceScale( view ) )
                return;
            const GrassPlacement& p        = variety.Placement;
            const float           maxScale = std::max( { p.ScaleX.y, p.ScaleY.y, p.ScaleZ.y, 1.0e-3f } );
            float                 radius   = 0.0f;
            if ( variety.Blade )
                radius = 0.5f * variety.Blade->Height;
            else
            {
                const Common::Math::AABB bounds = Geometry::LocalBounds( mesh.GetSubmeshes() );
                radius                          = 0.5f * glm::length( bounds.Max - bounds.Min );
            }
            const float pixels = TextureStreamingDemand::ScreenPixels( ground + glm::vec3( 0.0f, radius, 0.0f ),
                                                                       radius * maxScale, view, viewHeight );
            for ( const GrassSectionDraw& section : variety.Sections )
                if ( section.Material )
                    TextureStreamingDemand::Get().Note( section.Material->GetParentMaterial(), pixels );
        }

    } // namespace

    GrassFieldLens MakeGrassFieldLens( const GrassQuality& quality, const ViewFrame& frame )
    {
        const bool perspective = std::abs( frame.Projection[2][3] ) != 0.0f;
        GrassFieldLens lens{
             .DistanceScale = quality.CullDistanceScale *
                              DrawDistanceScale( Geometry::LODView{ frame.CameraPosition, frame.Projection } ),
             .PixelsPerUnit =
                  perspective && frame.Split.Render.Width > 0u
                       ? std::abs( frame.Projection[0][0] ) * 0.5f * static_cast<float>( frame.Split.Render.Width )
                       : 0.0f };
        if ( quality.LodScreenPixels > 0.0f )
            lens.FarLodScale = lens.PixelsPerUnit / quality.LodScreenPixels;
        return lens;
    }

    float GrassFarLodRadius( const Common::Math::AABB& bounds )
    {
        const glm::vec3 centre = ( bounds.Min + bounds.Max ) * 0.5f;
        return glm::length( bounds.Max - bounds.Min ) * 0.5f + glm::length( glm::vec2( centre.x, centre.z ) );
    }

    float GrassBladeMeanWidth( const GrassBladeDraw& blade, const glm::vec2 scaleX )
    {
        const size_t cards = std::min( blade.Cards.size(), kGrassBladeMaxCards );
        if ( cards == 0u )
            return 0.0f;
        float sum = 0.0f;
        for ( size_t i = 0; i < cards; ++i )
        {
            const glm::vec4& card = blade.Cards[i];
            sum += blade.Height * ( card.z - card.x ) / std::max( card.w - card.y, 1.0e-4f );
        }
        return sum * 0.5f * ( scaleX.x + scaleX.y ) / static_cast<float>( cards );
    }

    Common::BoolResultStr GrassRenderer::Initialize()
    {
        // GRASS-CULL-2: GrassCull reads and writes the varieties' buffers by device address (the
        // variety table).
        if ( !EngineContext::GetInstance().GetCapabilities().SupportsBufferDeviceAddress )
            return Common::MakeError( "GrassRenderer: the grass passes address their varieties' buffers by device "
                                      "address, and the device has no bufferDeviceAddress" );
        auto* shaders = Runtime::ResourceRegistry::GetShaderService();
        for ( const auto& [name, into] :
              { std::pair{ "GrassGenerate", &m_Generate }, std::pair{ "GrassCull", &m_Cull },
                std::pair{ "GrassDrawCompact", &m_DrawCompact } } )
        {
            const auto shader = shaders->GetByName( name );
            if ( !shader )
                return Common::MakeError( std::format( "GrassRenderer: missing shader '{}'", name ) );
            const auto made = ComputePipeline::Create( { .Shader = shader, .DebugName = name } );
            if ( !made )
                return Common::MakeError( std::format( "GrassRenderer: {}", made.GetError() ) );
            *into = made.GetValue();
        }
        return BOOLSUCCESS;
    }

    Desert::Mesh* GrassRenderer::BladeStrip( const uint32_t segments )
    {
        auto& strip = m_BladeStrips[segments];
        if ( strip )
            return strip.get();
        // A strip in blade space: Position = (side -1/+1, height share 0..1, 0); the vertex stage bends, tapers
        // and places it (Vertex_GrassBlade.glslh). The normal frame is the stage's too.
        std::vector<Vertex>  vertices;
        std::vector<Index>   indices;
        std::vector<Submesh> submeshes;
        for ( uint32_t level = segments;; level = std::max( 1u, level / 2u ) )
        {
            Submesh sm{};
            sm.Name         = std::format( "Blade{}", level );
            sm.VertexOffset = static_cast<uint32_t>( vertices.size() );
            sm.IndexOffset  = static_cast<uint32_t>( indices.size() * 3u );
            const GrassBladeStripLevel blade = MakeGrassBladeStripLevel( level );
            sm.VertexCount                   = static_cast<uint32_t>( blade.Vertices.size() );
            sm.IndexCount                    = static_cast<uint32_t>( blade.Indices.size() * 3u );
            vertices.insert( vertices.end(), blade.Vertices.begin(), blade.Vertices.end() );
            indices.insert( indices.end(), blade.Indices.begin(), blade.Indices.end() );
            sm.Transform   = glm::mat4( 1.0f );
            sm.BoundingBox = Common::Math::AABB{ glm::vec3( -1.0f, 0.0f, 0.0f ), glm::vec3( 1.0f, 1.0f, 0.0f ) };
            submeshes.push_back( std::move( sm ) );
            if ( level == 1u )
                break;
        }
        // THE SMALL BUCKET (FPS-GRASS-RASTER; UE r.MinScreenRadiusForDepthPrepass over the grass HISM's
        // per-instance LOD): one more submesh over the coarsest level's own ranges - the same one-segment blade,
        // no new vertex - so the coarsest level's instances past the distance where every blade stands below
        // kMinScreenRadiusForDepthPrepass of the screen (GrassCull: p.Lod.w) draw as a bucket of their own, one
        // indirect draw, which the masked depth prepass leaves to the G-buffer (VarietyGpu::PrepassBuckets).
        // Without it the coarsest level runs from 2 x LodDistance to the field's end and is never small.
        {
            Submesh smallBucket = submeshes.back();
            smallBucket.Name    = "BladeSmall";
            submeshes.push_back( std::move( smallBucket ) );
        }
        strip = std::make_shared<StaticMesh>( vertices, indices, submeshes, std::vector<MeshVertexStreams>{} );
        if ( const auto uploaded = strip->Invalidate(); !uploaded.IsSuccess() )
        {
            LOG_ERROR( "[Grass] the {}-segment blade strip was not uploaded: {}", segments, uploaded.GetError() );
            strip.reset();
            return nullptr;
        }
        return strip.get();
    }

    // RENDER-PARALLEL-SETUP: one variety's frame between its three steps (GrassRenderer.hpp, PrepareVariety).
    struct GrassRenderer::VarietyPlan
    {
        // PrepareVariety's, serial: what the plan reads of the variety, its entry and the view.
        const GrassVarietyDraw* Variety   = nullptr;
        Desert::Mesh*           MeshAsset = nullptr; // only GetSubmeshes is read on the task
        VarietyGpu*             Gpu       = nullptr;
        const ViewFrame*        Frame     = nullptr;
        const ViewVolumes*      Volumes   = nullptr;
        const MeshRenderer*     Meshes    = nullptr;
        GrassPlacement          Placement{};
        GrassFieldLens          FieldLens{};
        float                   Cell = 0.0f, Lens = 0.0f, Radius = 0.0f, KeepShare = 0.0f, LodDistance = 0.0f;
        float                   FarLodDistance = 0.0f, BoundsRadius = 0.0f, BoundsHeight = 0.0f, SnowDepth = 0.0f;
        float                   SmallDistance = 0.0f; // a blade's small bucket's start (GrassCull p.Lod.w)
        double                  Moved    = 0.0;
        uint32_t                Capacity = 0u, Buckets = 0u, Records = 0u;
        uint64_t                FieldRevision = 0u;
        // PlanVariety's, on a task: the params and the tile build's inputs (UpdateTiles).
        GrassParamsGpu        Params{};
        std::vector<uint32_t> TilePlacement;
        std::vector<uint32_t> TileThin;
        glm::vec3             Eye{ 0.0f };
        float                 Stray = 0.0f;
        // FPS-GRASS-PLAN: ReadVarietyFeedback's, render thread, before PrepareVariety: the entry's feedback words
        // (its Buckets at the read, then the tail) and whether they were read; the field (an index of m_Queue)
        // and whether PrepareVariety kept the variety (PlanFrame plans and CommitFrame applies only those).
        std::array<uint32_t, kLawBuckets + kFeedbackTailWords> Counted{};
        uint32_t                                               CountedBuckets = 0u;
        bool                                                   CountedRead    = false;
        bool                                                   Prepared       = false;
        std::size_t                                            FieldIndex     = 0u;
    };

    // RISK 9 (UE ParallelFor MinBatchSize): a PlanVariety slice holds at least this many plans - a field of fewer
    // than twice as many varieties is planned by one task, with no hand-out cost.
    constexpr std::size_t kMinGrassPlansPerTask = 8;

    // FPS-GRASS-PLAN (UE FSceneRenderer setup tasks): the frame PrepareFrame fills on the render thread, PlanFrame
    // completes on its task and CommitFrame consumes after the join. The vectors are kept across frames.
    struct GrassRenderer::GrassFramePlan
    {
        const ViewFrame*    Frame  = nullptr;
        const MeshRenderer* Meshes = nullptr;
        ViewVolumes         Volumes{};
        GrassOcclusion      Occlusion{};
        bool                Parallel = false;
        bool                Ready    = false; // PrepareFrame planned varieties CommitFrame has not consumed
        // The planned varieties, in field and variety order, up to the first repeated key of a field.
        std::vector<VarietyPlan> Varieties;
        // Where the serial rest starts (CommitFrame): a field (m_Queue index) and its variety; past the queue:
        // none.
        std::size_t ResumeField  = std::numeric_limits<std::size_t>::max();
        std::size_t ResumeCursor = 0u;
        // CommitFrame's: per m_Queue field whether its buffer is in this frame's graph; the serial rest's plans.
        std::vector<uint8_t>     FieldReady;
        std::vector<VarietyPlan> Serial;
    };

    GrassRenderer::~GrassRenderer() = default;

    GrassRenderer::VarietyGpu* GrassRenderer::PrepareVariety( const GrassVarietyDraw& variety, Desert::Mesh& mesh,
                                                              const ViewFrame& frame, const ViewVolumes& volumes,
                                                              const MeshRenderer& meshes,
                                                              const uint64_t fieldRevision, VarietyPlan& plan )
    {
        const GrassPlacement  p    = ScaledPlacement( variety.Placement, m_Quality );
        const float           cell = CellSizeCm( p );
        // Every distance of the field is authored for a 90-degree lens and read through this view's (UE scales the
        // grass HISM's cull distances by CalculateFieldOfViewDistanceScale the same way): a zoomed scope draws the
        // field out to k times its EndCullDistance and picks the blade's levels at k times its LodDistance, so a
        // patch at 100 m through a 12-degree lens looks as it does at ~10 m through a 90-degree one.
        const Geometry::LODView lensView{ frame.CameraPosition, frame.Projection };
        const float             lens        = DrawDistanceScale( lensView );
        const bool              perspective = std::abs( frame.Projection[2][3] ) != 0.0f;
        const float             radius      = p.EndCullDistance * lens;
        // The floor of the share of cells generated (GrassGenerate LensShare): 1 / k through a zoomed lens - flat
        // ground far away, where it holds the authored lens's instances per pixel; slopes facing the camera and
        // ground under it keep more, up to every cell.
        const float             keepShare   = std::min( 1.0f, 1.0f / std::max( lens, 1.0e-6f ) );
        const auto&             submeshes   = mesh.GetSubmeshes();
        // A blade draws one level per bucket (submesh b = bucket b, by distance). A mesh draws one bucket per
        // level of its LOD chain, every instance in the coarsest level whose screen size it is still below (UE
        // grass HISM: the instance's LOD by ComputeStaticMeshLOD's screen size, Geometry::SelectLODFromBounds) -
        // the levels are the mesh asset's own, its records (level L x submesh) each draw that level's index range.
        // The sizes are the field chain (Mesh::GetFieldLODScreenSizes): a field's sparse cards coarsen by the
        // pixels they cover.
        const std::vector<float>& screenSizes = mesh.GetFieldLODScreenSizes();
        const uint32_t            levels =
             variety.Blade ? 1u
                                      : std::max( 1u, std::min( Geometry::MaxAvailableLOD( submeshes ) + 1u,
                                                                static_cast<uint32_t>( screenSizes.size() ) ) );
        const uint32_t buckets     = variety.Blade ? static_cast<uint32_t>( submeshes.size() ) : levels;
        if ( buckets > kLawBuckets )
        {
            LOG_ERROR( "[Grass] a variety draws {} buckets; GrassCull's law words hold at most {} - it is not drawn",
                       buckets, kLawBuckets );
            return nullptr;
        }
        // A blade's records come in sets, one per strip level (GpuInstances.hpp): set k
        // draws bucket s with level max(s, k) - set 0 is the camera's, a cascade draws the set its texel size asks
        // for (MeshRenderer::BladeShadowSet). A mesh has one set of records per level of its LOD chain.
        const uint32_t sets        = variety.Blade ? static_cast<uint32_t>( submeshes.size() ) : levels;
        const auto     records     = static_cast<uint32_t>( submeshes.size() ) * sets;
        const float    lodDistance = variety.Blade ? variety.Blade->LodDistance * lens : 0.0f;
        // The instance's bounding sphere in its local space: a blade's from its shape (the upright blade's
        // middle, reaching the farthest leaning tip - its own curvature draw and its root's tilt - and the wind's
        // sway), a mesh's from its bounds.
        float boundsRadius = 0.0f, boundsHeight = 0.0f;
        if ( variety.Blade )
        {
            const GrassBladeDraw& b = *variety.Blade;
            boundsHeight            = 0.5f * b.Height;
            boundsRadius            = boundsHeight * ( 1.0f + b.Curvature * ( 1.0f + b.CurvatureSpread ) ) +
                           b.Height * std::sin( glm::radians( b.Tilt ) ) + b.MaxWidth + variety.Wind.Strength;
        }
        else
        {
            const Common::Math::AABB bounds = Geometry::LocalBounds( submeshes );
            const glm::vec3          centre = ( bounds.Min + bounds.Max ) * 0.5f;
            boundsHeight                    = centre.y;
            boundsRadius                    = GrassFarLodRadius( bounds );
        }
        // THE FAR LOD (GPU-FRAME-4 blades, GPU-FRAME-6 meshes; UE grass HISM: an instance's LOD and its cull by
        // its screen size): the distance at which the variety's bounding radius at scale 1 stands at the Foliage
        // quality's LodScreenPixels render pixels through this view's lens (GrassFieldLens::PixelsPerUnit - render
        // pixels, so the LOD follows the pixels the view has). GrassGenerate reads it as p.Lod.z into
        // GrassFieldShare.glslh GrassFarLodShare - for a blade through GrassBladeLodAt (its strip level, and the
        // far-LOD share whose survivors widen), for a mesh through GrassMeshFarLod (the share, whose dropped
        // instances shrink out by GrassMeshShrink) - written into the instance's bucket and its scale, so the
        // GBuffer, the masked depth prepass and every shadow cascade draw the same. 0 = no far LOD: no quality
        // value, an orthographic lens or no render extent (PixelsPerUnit 0).
        const GrassFieldLens fieldLens      = UnpackGrassFieldLens( frame.GrassLens );
        const float          farLodDistance = boundsRadius * fieldLens.FarLodScale;
        // The nearest clump point lies within the 3x3 clump cells around the blade: at most 2 * sqrt(2) cells off;
        // scatter carries the blade up to 2 * Scatter grid cells before that.
        const double moved  = variety.Blade
                                   ? 2.0 * 1.41421356 * variety.Blade->ClumpSize * variety.Blade->ClumpPull +
                                         2.0 * variety.Blade->Scatter * cell
                                   : 0.0;
        // FPS-GRASS-PLAN: the entry is the render thread's (PrepareFrame / CommitFrame insert it): a task never
        // inserts into m_Varieties.
        VarietyGpu& gpu = *plan.Gpu;
        // The distance past which a blade's coarsest level is its small bucket (BladeStrip's BladeSmall).
        float        smallDistance = 0.0f;
        {
            const GrassPlacement& placement = variety.Placement;
            const float maxScale = std::max( { placement.ScaleX.y, placement.ScaleY.y, placement.ScaleZ.y, 1.0e-3f } );
            // A blade's sphere already holds the sway; a mesh's is its bounds.
            gpu.Reach = ( std::abs( boundsHeight ) + boundsRadius ) * maxScale +
                        ( variety.Blade ? 0.0f : WindBoundsPad( variety.Wind ) );
            // THE SMALL BUCKETS (UE r.MinScreenRadiusForDepthPrepass; GpuInstancedRenderData::PrepassBuckets): the
            // first bucket whose every instance stands below kMinScreenRadiusForDepthPrepass of the screen through
            // this lens, in GrassGenerate's measure (radius x max(P00, P11) / 2 over the distance): a mesh's level
            // b >= 1 holds the instances below its half screen size, a blade's bucket b >= 1 the ones at least
            // b x LodDistance away. Bucket 0 reaches the camera, so it is never small.
            gpu.PrepassBuckets = buckets;
            const float screenMultiple =
                 std::max( 0.5f * std::abs( frame.Projection[0][0] ), 0.5f * std::abs( frame.Projection[1][1] ) );
            // A blade's last bucket is the small one (BladeStrip's BladeSmall): the coarsest level's instances
            // from the distance where the largest blade - the bounding radius at the largest scale - stands below
            // kMinScreenRadiusForDepthPrepass of the screen in the same measure (GrassCull p.Lod.w). 0: none (an
            // orthographic lens) - the bucket stays empty and in the pair. Its levels' buckets keep their rings.
            if ( variety.Blade && perspective && buckets > 1u )
            {
                smallDistance = screenMultiple * boundsRadius * maxScale / kMinScreenRadiusForDepthPrepass;
                gpu.PrepassBuckets = buckets - 1u;
            }
            const uint32_t stripLevels = variety.Blade && buckets > 1u ? buckets - 1u : buckets;
            for ( uint32_t b = 1; perspective && b < stripLevels; ++b )
            {
                float largest = 1.0f;
                if ( variety.Blade && lodDistance > 0.0f )
                    largest = screenMultiple * boundsRadius * maxScale / ( static_cast<float>( b ) * lodDistance );
                else if ( !variety.Blade && b < screenSizes.size() )
                    largest = 0.5f * screenSizes[b];
                if ( largest < kMinScreenRadiusForDepthPrepass )
                {
                    gpu.PrepassBuckets = b;
                    break;
                }
            }
            // What a cascade picks a blade's shadow LOD and density by (GpuInstanceBladeShadowLod): the tallest
            // blade, the strip, where the generator's thinning starts (GrassGenerate p.Cull.x) and the world size
            // of one of this view's pixels per cm of distance (a cascade texel of size T is as fine as a pixel at
            // T / PixelSpread).
            gpu.ShadowLod = {};
            if ( variety.Blade )
            {
                const float tanHalfY = 1.0f / std::max( 1.0e-6f, std::abs( frame.Projection[1][1] ) );
                const auto  rows     = static_cast<float>( std::max( 1u, frame.Split.Render.Height ) );
                gpu.ShadowLod        = { .Height      = variety.Blade->Height * maxScale,
                                         .Segments    = variety.Blade->Segments,
                                         .Levels      = static_cast<uint32_t>( submeshes.size() ),
                                         .StartCull   = p.StartCullDistance * lens,
                                         .PixelSpread = perspective ? 2.0f * tanHalfY / rows : 0.0f };
            }
        }
        // What the GPU counted for each bucket FramesInFlight frames ago (GrassCull FinalizeFull), when this
        // frame's copy has been written since the feedback buffer was made for this bucket count. At most
        // kLawBuckets + kFeedbackTailWords words (buckets <= kLawBuckets above): the plan's array, no heap.
        // FPS-GRASS-PLAN: read by ReadVarietyFeedback on the render thread before this ran (the entry's Buckets
        // then are its Buckets now: only this function changes them).
        const std::array<uint32_t, kLawBuckets + kFeedbackTailWords>& counted = plan.Counted;
        bool                                                          fedBack = false;
        gpu.Drawn                                                             = std::nullopt;
        gpu.DrawnCascades                                                     = {};
        // CPU-GRASS-POOL-7: the variety's range of the shared readback pool, its own counts only where they carry
        // its mark (a range handed out anew, or a remade pool, holds none of its writes yet).
        if ( plan.CountedRead && plan.CountedBuckets == buckets )
        {
            fedBack = counted[buckets] == gpu.FeedbackMark;
            if ( fedBack )
            {
                gpu.Drawn = glm::uvec3( counted[buckets + 1u], counted[buckets + 2u], counted[buckets + 3u] );
                for ( uint32_t v = 0; v < kGrassCascadeLists; ++v )
                    gpu.DrawnCascades[v] = counted[buckets + 4u + v];
            }
        }
        if ( gpu.LowFrames.size() != buckets )
            gpu.LowFrames.assign( buckets, 0u );

        // Per bucket the capacity holds what the generator ACTUALLY keeps, not what it could (UE grass: a
        // component's instance buffer is sized by the instances its builder produced, never by density x area).
        // Every cell whose jittered instance can lie within the bucket's ring of the cull radius - and, through
        // a perspective lens, within what that lens can see of the ring - only BOUNDS it (`bound`, every cell
        // kept): the layer's weight, the density range, slopes and the lens share drop most of those cells, and
        // sizing for the bound held 932 MB of instances on FIELD_Barn (FIELD-MEM) for a fraction drawn. The
        // GPU's own uncapped count of last frames (GrassCull FinalizeFull) says what was kept: the capacity follows it,
        // growing at once to the count plus a quarter, shrinking only after kShrinkFrames frames in a row two
        // steps (half) above it. Until the first count comes back (FramesInFlight frames after the variety
        // appears) the capacity is zero: those frames only count, and the field appears once it is measured.
        constexpr uint32_t                    kShrinkFrames = 60;
        std::array<uint32_t, kLawBuckets> capacities{}; // [0, buckets) used
        uint32_t                              capacity = 0;
        // The first bucket whose read-back count exceeds the capacity it was drawn with (GrassGenerate dropped the
        // rest): the frames since drew a truncated field (InstancesSettling).
        std::optional<uint32_t> overflowed;
        for ( uint32_t b = 0; b < buckets; ++b )
        {
            // A blade's level is a ring of distance; a mesh level's screen size reaches anywhere in the disc (a
            // large instance keeps its detail farther than a small one), so it is bounded by the whole disc.
            // A blade's last bucket is its small one (BladeStrip's BladeSmall): the coarsest level's instances
            // from SmallDistance out (none without it); the levels before it are rings.
            const uint32_t levels = variety.Blade && buckets > 1u ? buckets - 1u : buckets;
            const bool     tail   = variety.Blade && buckets > 1u && b == levels; // the small bucket
            const bool     ring   = variety.Blade && levels > 1u && !tail;
            double         inner  = ring ? static_cast<double>( b ) * lodDistance : 0.0;
            if ( tail )
                inner = static_cast<double>( smallDistance > 0.0f ? smallDistance : radius );
            // The coarsest level also holds the far LOD's instances (GrassBladeLodAt), from where the smallest
            // blade's radius stands at its screen size.
            if ( ring && b + 1u == levels && farLodDistance > 0.0f )
            {
                const float minScale = std::min( { p.ScaleX.x, p.ScaleY.x, p.ScaleZ.x } );
                inner = std::min( inner, static_cast<double>( farLodDistance ) * std::max( minScale, 0.0f ) );
            }
            const double outer = !ring || b + 1u == levels ? static_cast<double>( radius )
                                                           : std::min<double>( radius, ( b + 1.0 ) * lodDistance );
            uint32_t       needed = inner >= radius ? 0u : RingCapacity( inner, outer, cell, moved );
            const uint32_t bound  = needed;
            const uint32_t target = fedBack ? std::min( bound, counted[b] + counted[b] / 4u + 64u ) : 0u;
            const uint32_t step = CapacityStep( target );
            const uint32_t held = gpu.BucketCapacities.size() == buckets ? gpu.BucketCapacities[b] : 0u;
            if ( fedBack && counted[b] > held && !overflowed )
                overflowed = b;
            if ( step >= held || held > CapacityStep( bound ) )
            {
                capacities[b]    = step;
                gpu.LowFrames[b] = 0u;
            }
            else if ( 2u * step <= held && ++gpu.LowFrames[b] >= kShrinkFrames )
            {
                capacities[b]    = step;
                gpu.LowFrames[b] = 0u;
            }
            else
            {
                capacities[b] = held;
                if ( 2u * step > held )
                    gpu.LowFrames[b] = 0u;
            }
            capacity += capacities[b];
        }

        // GRASS-CULL-COST-3, THE LAW'S SLOTS (UE GPU Scene: sized by the instances the law keeps, never by the pool's
        // candidates): one per record before its chunk's cut that LawChunk lawed this law generation - what the GPU
        // handed out (Feedback, uncapped) plus a quarter, grown at once, shrunk only after kShrinkFrames frames in a
        // row at half. A new Instances is a new law generation (LawKey: its address), so every chunk is lawed into it;
        // the frames the old one was short drew a truncated field (InstancesSettling, below).
        const uint32_t lawUsed     = fedBack ? counted[buckets + kFeedbackLawWord] : 0u;
        const bool     lawOverflow = fedBack && lawUsed > gpu.LawCapacity;
        {
            const uint32_t lawStep = CapacityStep( lawUsed + lawUsed / 4u + 64u );
            uint32_t       lawNext = gpu.LawCapacity;
            if ( lawStep > gpu.LawCapacity )
            {
                lawNext          = lawStep;
                gpu.LawLowFrames = 0u;
            }
            else if ( 2u * lawStep <= gpu.LawCapacity && ++gpu.LawLowFrames >= kShrinkFrames )
            {
                lawNext          = lawStep;
                gpu.LawLowFrames = 0u;
            }
            else if ( 2u * lawStep > gpu.LawCapacity )
                gpu.LawLowFrames = 0u;
            // FPS-ARCH-GRASSROWS (UE GPU Scene: an instance's data persists, the views' lists hold ids): THE ROWS
            // are the law's - one mat4 per slot, written by LawChunk once per law generation; the culled lists
            // only name them. A row's index is its offset in 64-byte rows (InstancesBase): the pool's 256-byte
            // alignment keeps every range on a whole row.
            if ( gpu.Instances.Bytes == 0u || lawNext != gpu.LawCapacity )
            {
                const uint64_t lawBytes = static_cast<uint64_t>( lawNext ) * sizeof( glm::mat4 );
                gpu.LawCapacity         = 0u;
                if ( !m_InstancesPool.Allocate( gpu.Instances, lawBytes ) )
                {
                    LOG_ERROR(
                         "[Grass] variety {:016x}: the view law's rows ({} bytes; the pool of every "
                         "variety's at most a storage buffer's 4 GB) were not handed out; asked anew next frame",
                         variety.Key, lawBytes );
                    return nullptr;
                }
                gpu.LawCapacity = lawNext;
            }
        }

        // Settling (InstancesSettling): nothing counted yet (capacity zero, the field is not drawn), or an
        // overflow read now - the frames still in flight were drawn with the old capacity too, so the variety
        // settles only after that many read-backs within capacity.
        const uint32_t settleReads = EngineContext::GetInstance().GetMaxFramesInFlight();
        if ( !fedBack )
            gpu.SettleReads = settleReads;
        else if ( overflowed )
        {
            if ( !gpu.Truncated )
                LOG_INFO( "[Grass] variety {:016x} (layer {}) truncated {}->{} in bucket {} of {}: the field drew "
                          "without the rest until the capacity grows",
                          variety.Key, variety.Layer, counted[*overflowed],
                          gpu.BucketCapacities.size() == buckets ? gpu.BucketCapacities[*overflowed] : 0u,
                          *overflowed, buckets );
            gpu.Truncated   = true;
            gpu.SettleReads = settleReads;
        }
        else if ( lawOverflow )
        {
            if ( !gpu.Truncated )
                LOG_INFO( "[Grass] variety {:016x} (layer {}) truncated: the view law handed out {} slots, past "
                          "the rows' room; the field drew without the rest until it grows",
                          variety.Key, variety.Layer, lawUsed );
            gpu.Truncated   = true;
            gpu.SettleReads = settleReads;
        }
        else if ( gpu.SettleReads > 0u && --gpu.SettleReads == 0u )
            gpu.Truncated = false;
        m_Settling = m_Settling || gpu.SettleReads > 0u;

        if ( gpu.Mesh != &mesh || gpu.Capacity != capacity || gpu.Buckets != buckets ||
             !std::ranges::equal( gpu.BucketCapacities, std::span( capacities.data(), buckets ) ) ||
             gpu.Args.Bytes == 0u || gpu.List != volumes.List ||
             gpu.CascadeMask != volumes.CascadeMask )
        {
            gpu.List             = volumes.List;
            gpu.Mesh             = &mesh;
            gpu.Capacity         = capacity;
            gpu.Buckets          = buckets;
            gpu.BucketCapacities.assign( capacities.begin(), capacities.begin() + buckets );
            if ( gpu.Index == 0 )
                gpu.Index = ++m_NextIndex;
            // GRASS-VIEW-LAW-1: the id lists below are written and read by the GPU alone, so they live in
            // device-local memory (StorageBuffer::CreateDeviceLocal), not in the host-visible device heap, and so
            // do the rows they name (Instances, REGRESS-CACHE-1: three regions of the lists' capacity, below).
            const auto submeshCount = static_cast<uint32_t>( submeshes.size() );
            const auto tableWord    = static_cast<uint32_t>( GpuInstanceArgsOffset( records ) / 4u );
            const auto argBytes     = ( tableWord + buckets * kBucketWords ) * 4u;
            // CPU-GRASS-POOL-5: the lists are ranges of the shared pools; what a failure below leaves handed out
            // is released by the next (re)build's Allocate, and Args stays empty so this block runs again.
            ++gpu.Builds;
            gpu.ArgsEpoch     = 0u;
            const auto refuse = [this, &gpu]() noexcept -> VarietyGpu*
            {
                m_ArgsPool.Release( gpu.Args );
                return nullptr;
            };
            // The records' static half: record L x submeshCount + si draws submesh si at level L (a submesh with a
            // shorter chain its coarsest level, one without a chain its base range); a blade's record k x
            // submeshCount + s draws strip level max(s, k) for bucket s (set k, the shadow LOD). instanceCount and
            // firstInstance are GrassCull's (FinalizeFull, FinalizeCamera). The bucket table: each bucket's first instance, capacity and - a
            // mesh level - half its screen size.
            std::vector<uint32_t>& args = gpu.ArgsWords;
            args.assign( argBytes / 4u, 0u );
            args[0] = buckets;
            args[1] = capacity;
            args[2] = records;
            for ( uint32_t level = 0; level < sets; ++level )
                for ( uint32_t si = 0; si < submeshCount; ++si )
                {
                    const auto& sm     = submeshes[variety.Blade ? std::max( si, level ) : si];
                    uint32_t*   record = &args[GpuInstanceArgsOffset( level * submeshCount + si ) / 4u];
                    if ( sm.LODs.empty() )
                    {
                        record[0] = sm.IndexCount;
                        record[2] = sm.IndexOffset;
                    }
                    else
                    {
                        const auto& lod = sm.LODs[std::min<size_t>( level, sm.LODs.size() - 1 )];
                        record[0]       = lod.IndexCount;
                        record[2]       = lod.IndexOffset;
                    }
                    record[3] = sm.VertexOffset;
                }
            for ( uint32_t b = 0, first = 0; b < buckets; first += capacities[b], ++b )
            {
                args[tableWord + b * kBucketWords + 1u] = first;
                args[tableWord + b * kBucketWords + 2u] = capacities[b];
                if ( !variety.Blade )
                    args[tableWord + b * kBucketWords + 3u] = std::bit_cast<uint32_t>( 0.5f * screenSizes[b] );
            }
            // CPU-GRASS-POOL-5: device-local ranges of the args pool, written by one range upload each after the
            // pool's Commit (CommitListPools), no longer host-visible buffers of their own.
            if ( !m_ArgsPool.Allocate( gpu.Args, argBytes ) )
            {
                LOG_ERROR(
                     "[Grass] variety {:016x}: its argument records ({} bytes) were not handed out (the args "
                     "pool is at a storage buffer's 4 GB)",
                     variety.Key, argBytes );
                return nullptr;
            }
            // The camera's list (GRASS-VIS): the same records and bucket table, its own counts; CameraArgs adds
            // the phase words, and only an HZB-tested list has candidates and phase 2's records (PlanesAndHzb).
            // What this frame's list does not use is a one-word stand-in (the passes bind it, Hzb.w / Mode.y keep
            // it idle).
            {
                const uint64_t idBytes = uint64_t{ std::max( capacity, 1u ) } * kGrassIdBytes;
                bool           handed  = m_IdsPool.Allocate( gpu.CameraIds, idBytes );
                // The full list (GrassCull.shader CullInstance): the records the views keep, in Args' buckets.
                handed = handed && m_IdsPool.Allocate( gpu.FullIds, idBytes );
                // The lists hold ids of the law's rows (FPS-ARCH-GRASSROWS; Instances, sized by LawCapacity).
                std::vector<uint32_t>& cameraArgs = gpu.CameraArgsWords;
                cameraArgs.assign( args.begin(), args.end() );
                cameraArgs.resize( args.size() + static_cast<size_t>( buckets ) * kPhaseWords, 0u );
                handed = handed && m_ArgsPool.Allocate( gpu.CameraArgs, cameraArgs.size() * 4u );
                if ( !handed )
                {
                    // No Args: this block runs again next frame, as a pool that was not made is (UpdateTiles).
                    LOG_ERROR( "[Grass] variety {:016x}: its id lists ({} bytes) or camera records were not "
                               "handed out (a pool is at a storage buffer's 4 GB); they are handed out anew next "
                               "frame",
                               variety.Key, idBytes );
                    return refuse();
                }
            }
            if ( volumes.List == CameraList::PlanesAndHzb )
            {
                const uint64_t candidateBytes = uint64_t{ std::max( capacity, 1u ) } * kCandidateBytes;
                if ( !m_CandidatesPool.Allocate( gpu.Candidates, candidateBytes ) )
                {
                    LOG_ERROR(
                         "[Grass] the camera list's candidates ({} bytes; the pool of every variety's at most "
                         "a storage buffer's 4 GB) were not handed out",
                         candidateBytes );
                    return refuse();
                }
                gpu.PostArgsWords.assign( args.begin(), args.end() );
            }
            else
            {
                // A one-word stand-in the passes bind and never touch.
                if ( !m_CandidatesPool.Allocate( gpu.Candidates, 16u ) )
                {
                    LOG_ERROR( "[Grass] the camera list's candidate stand-in was not handed out (the pool is at a "
                               "storage buffer's 4 GB)" );
                    return refuse();
                }
                gpu.PostArgsWords.assign( 4u, 0u );
            }
            if ( !m_ArgsPool.Allocate( gpu.PostArgs, gpu.PostArgsWords.size() * 4u ) )
            {
                LOG_ERROR( "[Grass] the camera list's phase-2 records were not handed out (the args pool is at a "
                           "storage buffer's 4 GB)" );
                return refuse();
            }
            // The cascades' lists (GPU-COST-21): per cascade view this frame draws a copy of the records and the
            // bucket table, list k's bucket firsts moved by k x capacity into CascadeIds; then its rest twin
            // (GRASS-SHADOW-1): the records once more, for the rest-pose casters at each bucket's end, and one
            // rest count per bucket (GrassGenerate.shader appends, GrassCull.shader FinalizeCascades).
            const auto recordsWord = static_cast<uint32_t>( GpuInstanceArgsOffset( 0u ) / 4u );
            gpu.CascadeMask        = volumes.CascadeMask;
            gpu.CascadeArgWords    = static_cast<uint32_t>( args.size() ) + ( tableWord - recordsWord ) + buckets;
            gpu.CascadeRestShift   = argBytes - recordsWord * 4u;
            std::vector<uint32_t>& cascadeArgs = gpu.CascadeArgsWords;
            cascadeArgs.clear();
            uint64_t cascadeIdBytes = 16u; // no live view: one-word stand-ins the passes bind
            if ( const auto lists = static_cast<uint32_t>( std::popcount( volumes.CascadeMask ) ); lists > 0u )
            {
                cascadeIdBytes = uint64_t{ std::max( lists * capacity, 1u ) } * kGrassIdBytes;
                cascadeArgs.reserve( static_cast<size_t>( gpu.CascadeArgWords ) * lists );
                for ( uint32_t k = 0; k < lists; ++k )
                {
                    const size_t at = cascadeArgs.size();
                    cascadeArgs.insert( cascadeArgs.end(), args.begin(), args.end() );
                    for ( uint32_t b = 0; b < buckets; ++b )
                        cascadeArgs[at + tableWord + b * kBucketWords + 1u] += k * capacity;
                    cascadeArgs.insert( cascadeArgs.end(), args.begin() + recordsWord, args.begin() + tableWord );
                    cascadeArgs.insert( cascadeArgs.end(), buckets, 0u );
                }
            }
            else
                cascadeArgs.assign( 4u, 0u );
            if ( !m_IdsPool.Allocate( gpu.CascadeIds, cascadeIdBytes ) ||
                 !m_ArgsPool.Allocate( gpu.CascadeArgs, cascadeArgs.size() * 4u ) )
            {
                LOG_ERROR(
                     "[Grass] variety {:016x}: the cascade lists ({} bytes of ids) were not handed out (a pool "
                     "is at a storage buffer's 4 GB)",
                     variety.Key, cascadeIdBytes );
                return refuse();
            }
            // CPU-GRASS-POOL-7: one params record of the params pool (kept over a rebuild) and the feedback's
            // range of the shared readback pool (kept when it fits), and a new mark on every (re)build: the
            // copies the frames in flight hold were written under the earlier layout.
            if ( gpu.Params.Bytes == 0u && !m_ParamsPool.Allocate( gpu.Params, sizeof( GrassParamsGpu ) ) )
            {
                LOG_ERROR( "[Grass] variety {:016x}: its params record was not handed out (the pool is at a "
                           "storage buffer's 4 GB)",
                           variety.Key );
                return refuse();
            }
            if ( const uint64_t feedbackBytes = ( buckets + kFeedbackTailWords ) * 4u;
                 ( gpu.Feedback.Bytes < feedbackBytes || gpu.Feedback.Bytes >= feedbackBytes + 16u ) &&
                 !m_FeedbackPool.Allocate( gpu.Feedback, feedbackBytes ) )
            {
                LOG_ERROR( "[Grass] variety {:016x}: its feedback ({} bytes) was not handed out (the pool is at a "
                           "storage buffer's 4 GB)",
                           variety.Key, feedbackBytes );
                return refuse();
            }
            m_FeedbackMarks  = m_FeedbackMarks + 1u == 0u ? 1u : m_FeedbackMarks + 1u;
            gpu.FeedbackMark = m_FeedbackMarks;
            // FPS-GRASS-PLAN: a new shape buffer (a blade's) or none, made by CommitVarietyObjects.
            gpu.ShapeRemade = true;
        }
        if ( variety.Blade )
        {
            const GrassBladeDraw&    b = *variety.Blade;
            const size_t             cards = std::min( b.Cards.size(), gpu.ShapeData.Cards.size() );
            gpu.ShapeData =
                 GrassBladeShapeGpu{ glm::vec4( b.Height, b.Stiffness, b.Curvature, static_cast<float>( cards ) ),
                                     glm::vec4( b.CurvatureSpread, b.BendSpread, 0.0f, 0.0f ) };
            std::copy_n( b.Cards.begin(), cards, gpu.ShapeData.Cards.begin() );
            // The variety's rows of this frame's raster stream (GRASS-VIS): instance i is row VisibilityRow + i
            // of the view's visibility buffer, the bucket table tells the resolve which strip level drew it.
            // Past kGrassVisibilityMaxRows the view refuses the path this frame (VisibilityRowsOverflow), so a
            // row cut to 32 bits here is never resolved.
            gpu.VisibilityRow = static_cast<uint32_t>( m_VisibilityRows );
            m_VisibilityRows += std::max( capacity, 1u );
            gpu.ShapeData.Visibility =
                 glm::uvec4( gpu.VisibilityRow, b.Segments, std::min( buckets, kGrassBladeVisibilityMaxBuckets ),
                             std::max( capacity, 1u ) ); // w = the rows it owns (its resolve's bound)
            // CPU-GRASS-POOL-5: the raster's instance index is a slot of the shared ids pool; less x, the variety's.
            gpu.ShapeData.Lists =
                 glm::uvec4( static_cast<uint32_t>( gpu.CameraIds.Offset / kGrassIdBytes ), 0u, 0u, 0u );
            for ( uint32_t bucket = 0, first = 0; bucket < std::min( buckets, kGrassBladeVisibilityMaxBuckets );
                  first += capacities[bucket], ++bucket )
                gpu.ShapeData.BucketFirst[bucket / 4u][bucket % 4u] = first;
            // FPS-GRASS-PLAN: written into Shape by CommitVarietyObjects (render thread).
        }

        // GRASS-VIS-MESH-1: a mesh variety's sections each take their rows of this frame's raster stream
        // (GrassMeshVisibilityGpu, Mesh/Surface/GrassMeshVisibility.glslh; UE 5 Nanite's visible clusters): slot i
        // owns RowsPerInstance rows, a level's submesh one row per 128 of its triangles. A section whose table
        // cannot be stated (rows past kGrassVisibilityMaxRows, more submeshes or ranges than the table holds, a
        // mesh without device addresses) has none and stays in the masked prepass and the EQUAL G-buffer
        // (MeshRenderer admits only a tabled section), said once.
        if ( !variety.Blade )
        {
            // FPS-GRASS-PLAN: the rows and the tabled flags here; the tables are made and written by
            // CommitVarietyObjects (render thread).
            gpu.MeshVisibilityRows.resize( variety.Sections.size() );
            gpu.MeshVisibilityTabled.assign( variety.Sections.size(), uint8_t{ 0u } );
            const auto&    geometry      = mesh.GetGeometry();
            const auto     submeshCount  = static_cast<uint32_t>( submeshes.size() );
            const uint64_t vertexAddress = geometry.Vertices ? geometry.Vertices->GetDeviceAddress() : 0u;
            const uint64_t indexAddress  = geometry.Indices ? geometry.Indices->GetDeviceAddress() : 0u;
            const uint64_t streamAddress = geometry.Streams ? geometry.Streams->GetDeviceAddress() : 0u;
            // A mesh without a stream buffer reads the default streams (MeshVertexStreams: white, UV1 0,0), as the
            // raster's binding 1 reads the shared default buffer: a zero stream address tells the resolve so
            // (GRASS-VIS-MESH-2; most grass meshes author no colours nor second UV).
            const bool addressed =
                 vertexAddress != 0u && indexAddress != 0u && ( !geometry.Streams || streamAddress != 0u );
            const bool fits = submeshCount <= kGrassMeshVisibilityMaxSubmeshes &&
                              buckets <= kGrassBladeVisibilityMaxBuckets &&
                              buckets * submeshCount <= kGrassMeshVisibilityMaxRanges;
            constexpr uint32_t kRowTriangles = 1u << VirtualGeometry::kVisTriangleBits;
            for ( size_t s = 0; s < variety.Sections.size(); ++s )
            {
                const uint64_t          hidden = variety.Sections[s].HiddenSubmeshes;
                GrassMeshVisibilityGpu& rows   = gpu.MeshVisibilityRows[s];
                rows                           = GrassMeshVisibilityGpu{};
                uint32_t perInstance           = 0;
                for ( uint32_t level = 0; fits && level < buckets; ++level )
                {
                    uint32_t offset = 0;
                    for ( uint32_t si = 0; si < submeshCount; ++si )
                    {
                        if ( ( ( hidden >> si ) & 1u ) != 0u )
                            continue;
                        const auto     indices   = SubmeshIndices( submeshes[si], level );
                        const uint32_t triangles = indices.Count / 3u;
                        rows.Ranges[level * submeshCount + si] =
                             glm::uvec4( indices.First, triangles, offset, 0u );
                        offset += ( triangles + kRowTriangles - 1u ) / kRowTriangles;
                    }
                    perInstance = std::max( perInstance, offset );
                }
                // The raster names a vertex's submesh by its vertex range among the section's drawn submeshes
                // (Vertex_Instanced.glslh GrassMeshVisibilityRow): a drawn submesh with no range, or two drawn
                // ones sharing vertices, would write one's triangles under the other's rows - such a section is
                // not tabled.
                bool distinct = true;
                for ( uint32_t si = 0; fits && distinct && si < submeshCount; ++si )
                {
                    if ( ( ( hidden >> si ) & 1u ) != 0u )
                        continue;
                    const Submesh& one = submeshes[si];
                    if ( one.VertexCount == 0u )
                        distinct = false;
                    for ( uint32_t sj = si + 1u; distinct && sj < submeshCount; ++sj )
                    {
                        const Submesh& other = submeshes[sj];
                        if ( ( ( hidden >> sj ) & 1u ) == 0u &&
                             uint64_t{ one.VertexOffset } < uint64_t{ other.VertexOffset } + other.VertexCount &&
                             uint64_t{ other.VertexOffset } < uint64_t{ one.VertexOffset } + one.VertexCount )
                            distinct = false;
                    }
                }
                const uint64_t need = uint64_t{ capacity } * perInstance;
                if ( !addressed || !fits || !distinct || perInstance == 0u ||
                     m_VisibilityRows + need > kGrassVisibilityMaxRows )
                {
                    // Untabled (MeshVisibilityTabled 0): CommitVarietyObjects drops the table. One thread at a
                    // time says it: the plan task, or the render thread's serial rest after the join.
                    static std::unordered_set<uint64_t> s_Untabled;
                    if ( s_Untabled.insert( variety.Key * 64u + s ).second )
                    {
                        LOG_INFO( "[Grass] variety {:016x} section {}: not drawn through the visibility buffer "
                                  "(device addresses {}, {} submeshes x {} levels, distinct vertex ranges {}, {} "
                                  "rows past {}); it stays in the masked depth prepass and the G-buffer.",
                                  variety.Key, s, addressed, submeshCount, buckets, distinct,
                                  m_VisibilityRows + need, kGrassVisibilityMaxRows );
                    }
                    continue;
                }
                rows.Visibility =
                     glm::uvec4( static_cast<uint32_t>( m_VisibilityRows ), perInstance, buckets, submeshCount );
                const auto cameraFirst = static_cast<uint32_t>( gpu.CameraIds.Offset / kGrassIdBytes );
                rows.Slots             = glm::uvec4( capacity, cameraFirst, 0u, 0u );
                for ( uint32_t bucket = 0, first = 0; bucket < buckets; first += capacities[bucket], ++bucket )
                    rows.BucketFirst[bucket / 4u][bucket % 4u] = first;
                const uint64_t vertexAt = vertexAddress + geometry.VertexByteOffset;
                uint64_t       streamAt = 0u; // 0: the default streams
                if ( streamAddress != 0u )
                    streamAt = streamAddress + geometry.StreamByteOffset;
                const uint64_t indexAt = indexAddress + geometry.IndexByteOffset;
                rows.Geometry[0] =
                     glm::uvec4( static_cast<uint32_t>( vertexAt ), static_cast<uint32_t>( vertexAt >> 32u ),
                                 static_cast<uint32_t>( streamAt ), static_cast<uint32_t>( streamAt >> 32u ) );
                rows.Geometry[1] =
                     glm::uvec4( static_cast<uint32_t>( indexAt ), static_cast<uint32_t>( indexAt >> 32u ),
                                 geometry.VertexStride, static_cast<uint32_t>( sizeof( MeshVertexStreams ) ) );
                for ( uint32_t si = 0; si < submeshCount; ++si )
                {
                    const uint32_t drawn = ( ( hidden >> si ) & 1u ) == 0u ? 1u : 0u;
                    rows.Submeshes[si] =
                         glm::uvec4( submeshes[si].VertexOffset, submeshes[si].VertexCount, drawn, 0u );
                }
                gpu.MeshVisibilityTabled[s] = 1u;
                m_VisibilityRows += need;
            }
        }

        // RENDER-PARALLEL-SETUP: the rest of the variety's frame is its plan's (PlanVariety, pure, on a task) and
        // its apply's (ApplyVariety, serial); what they read of this half travels in @p plan.
        plan.Variety        = &variety;
        plan.MeshAsset      = &mesh;
        plan.Gpu            = &gpu;
        plan.Frame          = &frame;
        plan.Volumes        = &volumes;
        plan.Meshes         = &meshes;
        plan.Placement      = p;
        plan.FieldLens      = fieldLens;
        plan.Cell           = cell;
        plan.Lens           = lens;
        plan.Radius         = radius;
        plan.KeepShare      = keepShare;
        plan.LodDistance    = lodDistance;
        plan.FarLodDistance = farLodDistance;
        plan.BoundsRadius   = boundsRadius;
        plan.SmallDistance  = smallDistance;
        plan.BoundsHeight   = boundsHeight;
        plan.Moved          = moved;
        plan.Capacity       = capacity;
        plan.Buckets        = buckets;
        plan.Records        = records;
        plan.FieldRevision  = fieldRevision;
        // The world's snow depth (PlanVariety, params.Lean.z), read by PrepareFrame: the scene renderer is not a
        // task's to touch.
        plan.SnowDepth = m_FrameSnowDepth;
        return &gpu;
    }

    void GrassRenderer::ReadVarietyFeedback( VarietyPlan& plan ) const
    {
        // Render thread (a StorageBuffer read picks the frame in flight's copy). The words of the entry's
        // current bucket count; PrepareVariety uses them only when it keeps that count.
        const VarietyGpu& gpu                                           = *plan.Gpu;
        plan.CountedRead                                                = false;
        plan.CountedBuckets                                             = gpu.Buckets;
        const std::shared_ptr<ShaderResources::StorageBuffer>& feedback = m_FeedbackPool.Buffer();
        if ( !feedback || gpu.Feedback.Bytes == 0u || gpu.Buckets > kLawBuckets )
            return;
        if ( const auto read = feedback->ReadBack( plan.Counted.data(), ( gpu.Buckets + kFeedbackTailWords ) * 4u,
                                                   static_cast<uint32_t>( gpu.Feedback.Offset ) );
             !read.IsSuccess() )
        {
            LOG_ERROR( "[Grass] the generated counts were not read back: {}", read.GetError() );
            return;
        }
        plan.CountedRead = true;
    }

    bool GrassRenderer::CommitVarietyObjects( const VarietyPlan& plan )
    {
        // Render thread, after PrepareVariety: what it decided, as GPU objects.
        const GrassVarietyDraw& variety = *plan.Variety;
        VarietyGpu&             gpu     = *plan.Gpu;
        if ( gpu.ShapeRemade )
        {
            gpu.Shape.reset();
            if ( variety.Blade )
                gpu.Shape =
                     ShaderResources::StorageBuffer::Create( "GrassBladeShape", sizeof( GrassBladeShapeGpu ), 13 );
            gpu.ShapeRemade = false;
        }
        if ( variety.Blade )
        {
            if ( !gpu.Shape )
            {
                LOG_ERROR( "[Grass] variety {:016x}: the blade shape buffer was not made", variety.Key );
                gpu.ShapeRemade = true; // made anew next frame
                return false;
            }
            if ( const auto wrote = gpu.Shape->SetData( &gpu.ShapeData, sizeof( gpu.ShapeData ) );
                 !wrote.IsSuccess() )
            {
                LOG_ERROR( "[Grass] the blade shape was not written: {}", wrote.GetError() );
                return false;
            }
            return true;
        }
        gpu.MeshVisibility.resize( gpu.MeshVisibilityTabled.size() );
        for ( std::size_t s = 0; s < gpu.MeshVisibilityTabled.size(); ++s )
        {
            std::shared_ptr<ShaderResources::StorageBuffer>& table = gpu.MeshVisibility[s];
            if ( gpu.MeshVisibilityTabled[s] == 0u )
            {
                table.reset();
                continue;
            }
            if ( !table )
                table = ShaderResources::StorageBuffer::Create( "GrassMeshVisibility",
                                                                sizeof( GrassMeshVisibilityGpu ), 13 );
            if ( !table )
            {
                LOG_ERROR( "[Grass] variety {:016x} section {}: its visibility table was not made; the section "
                           "stays in the masked depth prepass and the G-buffer this frame",
                           variety.Key, s );
                continue;
            }
            if ( const auto wrote = table->SetData( &gpu.MeshVisibilityRows[s], sizeof( GrassMeshVisibilityGpu ) );
                 !wrote.IsSuccess() )
            {
                LOG_ERROR( "[Grass] a mesh section's visibility rows were not written: {}", wrote.GetError() );
                table.reset();
            }
        }
        return true;
    }

    Desert::Mesh* GrassRenderer::DrawableMesh( const GrassVarietyDraw& variety )
    {
        Desert::Mesh* mesh = variety.Blade ? BladeStrip( variety.Blade->Segments ) : variety.Mesh;
        if ( mesh == nullptr || variety.Sections.empty() || !mesh->GetGeometry().IsResident() ||
             mesh->GetGeometry().IndexCount == 0 )
            return nullptr;
        return mesh;
    }

    bool GrassRenderer::RepeatedKey( const std::span<const VarietyPlan> plans, const GrassVarietyDraw& variety )
    {
        const bool repeated = std::any_of( plans.begin(), plans.end(), [&variety]( const VarietyPlan& plan )
                                           { return plan.Variety->Key == variety.Key; } );
        if ( repeated )
        {
            // A content fault (two layers resolving to one variety): drawn twice at two Prepares' cost, as the
            // sequential code drew it - said once per key, not once per frame (render thread only).
            static std::unordered_set<uint64_t> s_WarnedRepeatedKey;
            if ( s_WarnedRepeatedKey.insert( variety.Key ).second )
                LOG_ERROR( "[Grass] a field queues variety key {:#x} more than once; each repeat is "
                           "prepared and drawn again (two layers resolve to one variety)",
                           variety.Key );
        }
        return repeated;
    }

    void GrassRenderer::PlanVariety( VarietyPlan& plan ) const
    {
        // RENDER-PARALLEL-SETUP: on a TaskGroup task (PlanFrame, or CommitFrame's serial rest). It reads the plan,
        // the view and the mesh renderer fitted cascades (const; MeshRenderer::UpdateCascades ran first); it
        // writes the plan and its own VarietyGpu's ShadowLod and CascadeRings - a distinct entry per task
        // (PrepareFrame ends the plan, and CommitFrame starts a segment, at a repeated key).
        const GrassVarietyDraw& variety        = *plan.Variety;
        VarietyGpu&             gpu            = *plan.Gpu;
        const ViewFrame&        frame          = *plan.Frame;
        const ViewVolumes&      volumes        = *plan.Volumes;
        const MeshRenderer&     meshes         = *plan.Meshes;
        const auto&             submeshes      = plan.MeshAsset->GetSubmeshes();
        const GrassPlacement&   p              = plan.Placement;
        const GrassFieldLens&   fieldLens      = plan.FieldLens;
        const float             cell           = plan.Cell;
        const float             lens           = plan.Lens;
        const float             radius         = plan.Radius;
        const float             keepShare      = plan.KeepShare;
        const float             lodDistance    = plan.LodDistance;
        const float             farLodDistance = plan.FarLodDistance;
        const float             boundsRadius   = plan.BoundsRadius;
        const float             boundsHeight   = plan.BoundsHeight;
        const double            moved          = plan.Moved;
        const uint32_t          capacity       = plan.Capacity;
        const uint32_t          buckets        = plan.Buckets;
        const uint32_t          records        = plan.Records;
        const uint64_t          fieldRevision  = plan.FieldRevision;
        GrassParamsGpu&         params         = plan.Params;
        params                                 = GrassParamsGpu{};
        const float     tileSide = static_cast<float>( kGrassTileCells ) * cell;
        const glm::vec3 eye      = FieldEye( frame.CameraPosition, tileSide );
        params.Camera            = glm::vec4( eye, radius );
        gpu.ShadowLod.ThinOrigin = eye; // CSM-SCROLL-3: a cascade thins its blades from the field eye too
        params.Cull   = glm::vec4( p.StartCullDistance * lens, radius, cell,
                                   std::clamp( p.PlacementJitter, 0.0f, 0.99f ) * 0.5f );
        params.Range = glm::vec4( p.AllowedDensityRange.x, p.AllowedDensityRange.y, p.MaxScaleWeightAttenuation,
                                  boundsRadius );
        const uint32_t flags =
             ( p.RandomRotation ? kFlagRandomRotation : 0u ) | ( p.AlignToSurface ? kFlagAlignToSurface : 0u ) |
             ( p.WeightAttenuatesMaxScale ? kFlagWeightAttenuates : 0u ) | ( variety.Blade ? kFlagBlade : 0u );
        params.ScaleX  = glm::vec4( p.ScaleX, static_cast<float>( p.Scaling ), std::bit_cast<float>( flags ) );
        params.ScaleY  = glm::vec4( p.ScaleY, boundsHeight, 0.0f );
        params.ScaleZ  = glm::vec4( p.ScaleZ, 0.0f, keepShare );
        params.Info    = glm::uvec4( variety.Layer, capacity, static_cast<uint32_t>( submeshes.size() ),
                                     static_cast<uint32_t>( variety.Key * 0x9E3779B97F4A7C15ull >> 32 ) );
        params.Buckets = glm::uvec4( buckets, static_cast<uint32_t>( GpuInstanceArgsOffset( records ) / 4u ), 0u, 0u );
        // A blade's cards' widths (the vertex stage's: Height x du / dv x the instance's width scale) and their
        // mean, the mean blade's width at the ScaleX range's middle (GrassBladeMeanWidth - the ground's far cover
        // measures its floor by the same, LandscapeECSSystem::EmitGrass).
        static_assert( sizeof( GrassParamsGpu::CardWidths ) / sizeof( glm::vec4 ) * 4u == kGrassBladeMaxCards );
        float meanBladeWidth = 0.0f;
        if ( variety.Blade )
        {
            const GrassBladeDraw& b     = *variety.Blade;
            const size_t          cards = std::min( b.Cards.size(), kGrassBladeMaxCards );
            for ( size_t i = 0; i < cards; ++i )
            {
                const glm::vec4& card = b.Cards[i];
                params.CardWidths[i / 4u][i % 4u] =
                     b.Height * ( card.z - card.x ) / std::max( card.w - card.y, 1.0e-4f );
            }
            params.Buckets.z = static_cast<uint32_t>( cards );
            meanBladeWidth   = GrassBladeMeanWidth( b, p.ScaleX );
        }
        params.Blade = variety.Blade ? glm::vec4( lodDistance, variety.Blade->ClumpSize, variety.Blade->Clumping,
                                                  variety.Blade->ClumpPull )
                                     : glm::vec4( 0.0f );
        params.Spread = variety.Blade ? glm::vec4( 2.0f * variety.Blade->Scatter, variety.Blade->Patchiness,
                                                   variety.Blade->PatchSize, 0.0f )
                                      : glm::vec4( 0.0f, 0.0f, 1.0f, 0.0f );
        params.Lean   = variety.Blade ? glm::vec4( glm::radians( variety.Blade->Tilt ),
                                                   glm::radians( variety.Blade->FacingJitter ), 0.0f, 0.0f )
                                      : glm::vec4( 0.0f );
        // THE BLADE'S SCREEN-WIDTH FLOOR (GRASS-WIDTH): the distance at which the mean blade - the cards' mean
        // width at the ScaleX range's middle - stands MinScreenWidth render pixels wide through this lens
        // (|Projection[0][0]| x Render.Width / 2 pixels per unit of x / depth; the width axis is the blade's
        // horizontal). Past it GrassGenerate keeps (D / d)^2 of the blades (the law its distance thinning widens
        // by) and lifts each instance's own card to the floor width, so no blade triangle is narrower than the
        // floor and the pixel shader runs about once per covered pixel instead of up to four times (quad
        // overshade of sub-pixel strips; GRASS-VIS-9, GPU-COST-18). 0 - no floor: none authored, an
        // orthographic lens, no render extent.
        // GrassFloorDistance through the view's lens (ViewFrame::GrassLens, made once per view by SceneRenderer):
        // the ground's far cover reads its D by the same two (CameraUB::GrassLens).
        float floorDistance = 0.0f;
        if ( variety.Blade )
            floorDistance = GrassFloorDistance( meanBladeWidth, variety.Blade->MinScreenWidth, fieldLens );
        if ( floorDistance > 0.0f )
        {
            params.Lean.w   = variety.Blade->MinScreenWidth / fieldLens.PixelsPerUnit;
            params.Spread.w = floorDistance;
            // The generator keeps min((S / d)^2, (D / d)^2) - one thinning from min(S, D) (GrassFieldShare.glslh
            // GrassThinShare) - so a cascade thins from there too (GpuInstanceBladeShadowLod::StartCull).
            float& thinFrom = gpu.ShadowLod.StartCull;
            thinFrom        = thinFrom > 0.0f ? std::min( thinFrom, params.Spread.w ) : params.Spread.w;
        }
        {
            const uint32_t seed =
                 p.PatchGroup != 0u ? ( p.PatchGroup * 0x9E3779B1u ) ^ 0x68E31DA4u : params.Info.w ^ 0x2C1B3C6Du;
            params.Patches = glm::vec4( std::clamp( p.PatchCoverage, 0.0f, 1.0f ), std::max( p.PatchSize, 1.0f ),
                                        std::clamp( p.PatchSoftness, 0.0f, 1.0f ), std::bit_cast<float>( seed ) );
        }
        // The world's snow buries every instance's root by its depth (Common/WeatherSnow.glslh): a blade shows only
        // what stands above the snow, and one shorter than it is gone, in every pass that draws or shades it.
        params.Lean.z = plan.SnowDepth;
        // The cascades' lists are GrassCull's (per frame, CascadeCull): each list's movers' ring for this
        // variety's reach (GRASS-SHADOW-1, MeshRenderer::ShadowWindInnerOf; x = inner, y = the wind distance, 0:
        // no mover).
        const bool sways = variety.Wind.Sways();
        const auto lists = static_cast<uint32_t>( std::popcount( gpu.CascadeMask ) );
        for ( uint32_t k = 0; k < kGrassCascadeLists; ++k )
        {
            glm::vec2 ring( 0.0f );
            if ( k < lists && sways )
                ring = glm::vec2( meshes.ShadowWindInnerOf( volumes.CascadeViews[k], gpu.Reach ),
                                  volumes.CascadeWind[k].w );
            gpu.CascadeRings[k] = ring;
        }
        if ( !variety.Blade )
        {
            const Common::Math::AABB bounds   = Geometry::LocalBounds( submeshes );
            const float              multiple = std::max( 0.5f * std::abs( frame.Projection[0][0] ),
                                                          0.5f * std::abs( frame.Projection[1][1] ) );
            params.Lod = glm::vec4( multiple * 0.5f * glm::length( bounds.Max - bounds.Min ),
                                    std::abs( frame.Projection[2][3] ), farLodDistance, 0.0f );
        }
        else
            params.Lod = glm::vec4( 0.0f, 0.0f, farLodDistance, plan.SmallDistance ); // w: the small bucket
        // GRASS-TILE-1, THE VARIETY'S TILES AROUND THE EYE (UpdateTiles). The build reads the placement's params
        // alone - the grid and its jitter, the layer and its seed, the density range and the scales, a blade's
        // clump, scatter, patchiness and lean, the patches - and the field (its revision); the share its tiles
        // keep reads the thinning's distances. The view's planes, position and cascades are the tile cull's.
        // RENDER-JOB-CPU-2: its 40 words (nine vec4, the layer, the seed, the field's revision) in one allocation,
        // not the ten regrowths of 40 push_backs, per variety per frame.
        constexpr std::size_t kPlacementWords = 9u * 4u + 4u;
        std::vector<uint32_t>& placement = plan.TilePlacement;
        placement.clear();
        placement.reserve( kPlacementWords );
        for ( const glm::vec4& words :
              { params.Range, params.ScaleX, glm::vec4( glm::vec3( params.ScaleY ), 0.0f ),
                glm::vec4( glm::vec3( params.ScaleZ ), 0.0f ),
                glm::vec4( params.Cull.z, params.Cull.w, 0.0f, 0.0f ),
                glm::vec4( 0.0f, params.Blade.y, params.Blade.z, params.Blade.w ),
                glm::vec4( glm::vec3( params.Spread ), 0.0f ),
                glm::vec4( params.Lean.x, params.Lean.y, 0.0f, 0.0f ), params.Patches } )
            for ( int i = 0; i < 4; ++i )
                placement.push_back( std::bit_cast<uint32_t>( words[i] ) );
        placement.push_back( params.Info.x );
        placement.push_back( params.Info.w );
        placement.push_back( static_cast<uint32_t>( fieldRevision ) );
        placement.push_back( static_cast<uint32_t>( fieldRevision >> 32u ) );
        plan.TileThin = { std::bit_cast<uint32_t>( params.Cull.x ), std::bit_cast<uint32_t>( params.Cull.y ),
                          std::bit_cast<uint32_t>( params.Spread.w ) };
        // How far an instance can lie outside its cell: its jitter (Cull.w cells), a blade's scatter and clump
        // pull.
        const auto stray = static_cast<float>( static_cast<double>( params.Cull.w * cell ) + moved );
        plan.Stray = stray;
        plan.Eye   = eye;
    }

    bool GrassRenderer::ApplyVariety( VarietyPlan& plan )
    {
        // RENDER-PARALLEL-SETUP: serial, in variety order (AddFrame). UpdateTiles weighs this variety's pool vs
        // every other's (TilePoolBytes) and the frame's cap, so the order of the applies is the budget's.
        const GrassVarietyDraw& variety = *plan.Variety;
        VarietyGpu&             gpu     = *plan.Gpu;
        const GrassParamsGpu&   params  = plan.Params;
        // CPU-GRASS-POOL-7: uploaded into the variety's params record once every variety applied
        // (CommitVarietyPools).
        static_assert( sizeof( GrassParamsGpu ) == kParamsWords * sizeof( uint32_t ) );
        std::memcpy( gpu.ParamsWords.data(), &params, sizeof( GrassParamsGpu ) );
        const glm::vec2 eyeXZ( plan.Eye.x, plan.Eye.z );
        if ( !UpdateTiles( gpu, std::move( plan.TilePlacement ), std::move( plan.TileThin ), eyeXZ, plan.Cell,
                           plan.Radius, plan.Stray ) )
            return false;

        // GRASS-CULL-2: what GrassCull reads of these params, through the variety table (GrassVarietyRowGpu).
        const float swayPad = variety.Blade ? 0.0f : static_cast<float>( WindBoundsPad( variety.Wind ) );
        gpu.CullBounds      = glm::vec4( params.Range.w, params.ScaleY.z, swayPad, 0.0f );
        gpu.TableWord  = params.Buckets.y;
        // What this frame's tile cull would read (VarietyGpu::FrameKey); AddFrame adds the field's revision.
        static_assert( sizeof( GrassParamsGpu ) % sizeof( uint32_t ) == 0 );
        gpu.FrameKey.assign( sizeof( GrassParamsGpu ) / sizeof( uint32_t ), 0u );
        std::memcpy( gpu.FrameKey.data(), &params, sizeof( GrassParamsGpu ) );
        gpu.FrameKey.push_back( gpu.TileVersion );
        gpu.FrameKey.push_back( gpu.Capacity );
        // The pool's identity: a new pool is a new generation (UpdateTiles).
        gpu.FrameKey.push_back( gpu.PoolGeneration );
        // CPU-GRASS-POOL-5: the rows' and records' identity - the build and their ranges. CPU-GRASS-POOL-8: their
        // pools' Epochs are committed after every apply; CommitListPools makes a remade pool a new generation.
        for ( const uint64_t identity : { gpu.Builds, gpu.Instances.Offset, gpu.Args.Offset } )
        {
            gpu.FrameKey.push_back( static_cast<uint32_t>( identity ) );
            gpu.FrameKey.push_back( static_cast<uint32_t>( identity >> 32u ) );
        }
        // GRASS-CULL-COST-3: what the view law reads (VarietyGpu::LawKey) - the params (the field eye among them), the
        // pool's identity, Args (a mesh level's half screen size) and Instances; AddFrame adds the field's revision. A
        // tile built or freed is no new law generation: the build writes its chunks unlawed (law word 0).
        gpu.LawKey.assign( gpu.FrameKey.begin(),
                           gpu.FrameKey.begin() +
                                static_cast<std::ptrdiff_t>( sizeof( GrassParamsGpu ) / sizeof( uint32_t ) ) );
        gpu.LawKey.push_back( gpu.PoolGeneration );
        // The rows' room too: a range handed out anew may land on the freed one's offset, and every new range
        // changes the room. CPU-GRASS-POOL-1: and its offset in the pool; a remade pool, which moves every range
        // after this key is taken, is a new generation by the pool's Epoch (WriteVarietyTable).
        gpu.LawKey.push_back( gpu.LawCapacity );
        gpu.LawKey.push_back( static_cast<uint32_t>( gpu.Instances.Offset ) );
        gpu.LawKey.push_back( static_cast<uint32_t>( gpu.Instances.Offset >> 32u ) );
        for ( const uint64_t identity : { gpu.Builds, gpu.Args.Offset } ) // the Epoch: CommitListPools
        {
            gpu.LawKey.push_back( static_cast<uint32_t>( identity ) );
            gpu.LawKey.push_back( static_cast<uint32_t>( identity >> 32u ) );
        }
        return true;
    }

    namespace
    {
        // CPU-GRASS-POOL-1: a range starts on this many bytes (any record a pooled kind holds is at most a uvec4,
        // and the variety table's addresses are read as 16-byte aligned), and a pool is at most a storage buffer's
        // 4 GB (StorageBuffer::CreateDeviceLocal's size), kept aligned.
        constexpr uint64_t kPoolAlignment = 256u;
        constexpr uint64_t kPoolMaxBytes  = uint64_t{ UINT32_MAX } & ~( kPoolAlignment - 1u );
    } // namespace

    bool GrassRenderer::BufferPool::Allocate( PoolRange& range, const uint64_t bytes )
    {
        Release( range );
        const uint64_t size =
             ( std::max( bytes, uint64_t{ 1u } ) + m_Alignment - 1u ) & ~( m_Alignment - 1u );
        for ( auto run = m_Free.begin(); run != m_Free.end(); ++run )
        {
            if ( run->Bytes < size )
                continue;
            range = PoolRange{ run->Offset, size };
            run->Offset += size;
            run->Bytes -= size;
            if ( run->Bytes == 0u )
                m_Free.erase( run );
            return true;
        }
        if ( size > kPoolMaxBytes - m_End )
            return false;
        range = PoolRange{ m_End, size };
        m_End += size;
        return true;
    }

    void GrassRenderer::BufferPool::Release( PoolRange& range )
    {
        if ( range.Bytes == 0u )
            return;
        auto run = std::lower_bound( m_Free.begin(), m_Free.end(), range.Offset,
                                     []( const PoolRange& free, const uint64_t offset ) noexcept
                                     { return free.Offset < offset; } );
        run      = m_Free.insert( run, range );
        range    = PoolRange{};
        if ( const auto next = std::next( run ); next != m_Free.end() && run->Offset + run->Bytes == next->Offset )
        {
            run->Bytes += next->Bytes;
            m_Free.erase( next );
        }
        if ( run != m_Free.begin() )
        {
            const auto previous = std::prev( run );
            if ( previous->Offset + previous->Bytes == run->Offset )
            {
                previous->Bytes += run->Bytes;
                run = std::prev( m_Free.erase( run ) );
            }
        }
        if ( run->Offset + run->Bytes == m_End )
        {
            m_End = run->Offset;
            m_Free.erase( run );
        }
    }

    Common::BoolResultStr GrassRenderer::BufferPool::Commit( RDG::Builder& graph )
    {
        // CPU-GRASS-POOL-8: the buffer a growth replaces, which this frame's graph copies from; parked with this
        // frame on every way out (FrameManager::RetireAfterFrames: it dies once no frame in flight names it).
        std::shared_ptr<ShaderResources::StorageBuffer> previous;
        struct RetireOnExit
        {
            std::shared_ptr<ShaderResources::StorageBuffer>* Buffer;
            ~RetireOnExit()
            {
                ::Desert::Engine::FrameManager::GetInstance().RetireAfterFrames( std::move( *Buffer ) );
            }
        } retire{ &previous };
        const uint64_t need    = std::max( m_End, kPoolAlignment );
        uint64_t       carried = 0u; // the predecessor's bytes this frame's graph copies into the new buffer
        if ( !m_Buffer || m_Room < need )
        {
            // Half again over what is asked, so a variety's growth rarely remakes the pool. UE's GPU Scene
            // (AddCopyBufferPass on resize): the old buffer's bytes are copied into the new one, so every range
            // keeps its offset AND its contents; only its address moves.
            const uint64_t room = std::min( need + need / 2u, kPoolMaxBytes );
            // CPU-GRASS-POOL-7: a readback pool's copies are the frames in flight's GPU writes, which a copy
            // of this frame's cannot carry: a remade one starts empty (Epoch).
            const bool readback = m_Memory == PoolMemory::Readback;
            previous            = std::move( m_Buffer );
            if ( !readback && previous )
                carried = m_Room;
            m_Address  = 0u;
            m_Room     = 0u;
            const auto size = static_cast<uint32_t>( room );
            if ( readback )
                m_Buffer = ShaderResources::StorageBuffer::CreateReadback( m_Name, size, m_Binding );
            else
                m_Buffer = ShaderResources::StorageBuffer::CreateDeviceLocal( m_Name, size, m_Binding );
            // Nothing to carry into on a refusal: what the ranges held is lost; the next Commit makes a pool.
            if ( !m_Buffer )
            {
                ++m_Epoch;
                return Common::MakeFormattedError( "the {} pool ({} bytes) was not made", m_Name, room );
            }
            const Common::ResultStr<uint64_t> address = m_Buffer->GetDeviceAddress();
            if ( !address )
            {
                m_Buffer.reset(); // never imported: no graph names it
                ++m_Epoch;
                return Common::MakeFormattedError( "the {} pool has no device address: {}", m_Name,
                                                   address.GetError() );
            }
            m_Address = address.GetValue();
            m_Room    = room;
            // REGR-CPU13: a growth moves the pool's address; every cache keyed on the Epoch (rows, args, the
            // cull's keys) is a new generation, so nothing keeps the retired buffer's address.
            ++m_Epoch;
            if ( carried != 0u )
                LOG_WARN( "[Grass] the {} pool grew to {} bytes (epoch {})", m_Name, room, m_Epoch );
        }
        if ( const Common::BoolResultStr imported = Renderer::ImportBuffer( m_Buffer, m_Import ); !imported )
        {
            if ( carried != 0u )
                ++m_Epoch; // the copy never runs: the new buffer's ranges start empty
            return imported;
        }
        if ( m_Memory == PoolMemory::Readback )
        {
            // The frame in flight's copy (the one ImportBuffer imported): taken every frame, never kept.
            const Common::ResultStr<uint64_t> address = m_Buffer->GetDeviceAddress();
            if ( !address )
                return Common::MakeFormattedError( "the {} pool's frame copy has no device address: {}", m_Name,
                                                   address.GetError() );
            m_Address = address.GetValue();
        }
        m_Ref = graph.RegisterExternal( m_Import, m_Name );
        if ( carried != 0u )
        {
            if ( const Common::BoolResultStr imported = Renderer::ImportBuffer( previous, m_PreviousImport );
                 !imported )
            {
                ++m_Epoch; // the copy cannot run: the ranges start empty in the new buffer
                return imported;
            }
            const RDG::BufferRef source = graph.RegisterExternal( m_PreviousImport, m_Name );
            graph.AddCopyBufferPass( source, 0u, m_Ref, 0u, carried );
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr GrassRenderer::CommitListPools( RDG::Builder& graph )
    {
        if ( m_Frame.empty() )
            return BOOLSUCCESS; // nothing reads the pools this frame
        for ( BufferPool* pool :
              { &m_InstancesPool, &m_IdsPool, &m_ArgsPool, &m_CandidatesPool } )
            if ( const Common::BoolResultStr committed = pool->Commit( graph ); !committed )
                return committed;
        // GPU-COST-22: every list reads the one Instances through its ids (GpuInstanceRefs::Ids); the ids are
        // absolute, so only Args' offset is the list's own.
        const auto refsOf = [this]( GpuInstanceRefs& refs, const PoolRange& args ) noexcept
        {
            refs.Instances   = m_InstancesPool.Ref();
            refs.Ids         = m_IdsPool.Ref();
            refs.Args        = m_ArgsPool.Ref();
            refs.ArgsOffset  = args.Offset;
            refs.ArgsAddress = m_ArgsPool.Address( args ); // a compacted bin's gather reads them
        };
        // After the commit: its grow copy (a Copy pass of this graph) carries every range first, then each
        // upload writes its own range, ordered after that copy by the graph.
        const uint64_t instancesEpoch = m_InstancesPool.Epoch();
        const uint64_t epoch          = m_ArgsPool.Epoch();
        for ( VarietyGpu* const gpu : m_Frame )
        {
            // A remade pool's ranges hold nothing: the lists are culled anew (and lawed anew: Args holds a mesh
            // level's half screen size the law reads).
            if ( gpu->InstancesPoolEpoch != instancesEpoch || gpu->ArgsPoolEpoch != epoch )
                ++gpu->Generation;
            // ...and the law's rows (FPS-ARCH-GRASSROWS: Instances holds them) are lawed anew.
            if ( gpu->InstancesPoolEpoch != instancesEpoch || gpu->ArgsPoolEpoch != epoch )
                ++gpu->LawGeneration;
            gpu->InstancesPoolEpoch = instancesEpoch;
            gpu->ArgsPoolEpoch      = epoch;
            refsOf( gpu->Refs, gpu->Args );
            refsOf( gpu->CameraRefs, gpu->CameraArgs );
            refsOf( gpu->PostRefs, gpu->PostArgs );
            refsOf( gpu->CascadeRefs, gpu->CascadeArgs ); // the cascades' bins gather them
            if ( gpu->ArgsEpoch == epoch || gpu->Args.Bytes == 0u )
                continue;
            for ( const auto& [range, words] : { std::pair{ &gpu->Args, &gpu->ArgsWords },
                                                 std::pair{ &gpu->CameraArgs, &gpu->CameraArgsWords },
                                                 std::pair{ &gpu->PostArgs, &gpu->PostArgsWords },
                                                 std::pair{ &gpu->CascadeArgs, &gpu->CascadeArgsWords } } )
                if ( range->Bytes != 0u && !words->empty() )
                {
                    const std::span<const uint32_t> view( words->data(), words->size() );
                    graph.QueueBufferUpload( m_ArgsPool.Ref(), range->Offset, std::as_bytes( view ) );
                }
            gpu->ArgsEpoch = epoch;
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr GrassRenderer::CommitVarietyPools( RDG::Builder& graph )
    {
        if ( m_Frame.empty() )
            return BOOLSUCCESS; // nothing reads the pools this frame
        for ( BufferPool* pool : { &m_ParamsPool, &m_ChunksPool, &m_SegmentTablesPool, &m_FeedbackPool } )
            if ( const Common::BoolResultStr committed = pool->Commit( graph ); !committed )
                return committed;
        // After the commits: their grow copies run first, each upload writes its own range after them.
        const uint64_t chunksEpoch = m_ChunksPool.Epoch();
        const uint64_t tableEpoch  = m_SegmentTablesPool.Epoch();
        // GRASS-CULL-COST-4 (UE GPU Scene: the primitives' uploads gathered into one scatter a frame, not a copy
        // each): every variety's params land in the pool's host image, and the span the frame's records cover goes
        // up in ONE upload - one Copy pass, not a pass (and a write-after-write barrier) per variety. The span's
        // other words are the image's: the last params uploaded to them, so a record outside the frame is written
        // what it already holds.
        uint64_t paramsLo = UINT64_MAX;
        uint64_t paramsHi = 0u;
        for ( const VarietyGpu* gpu : m_Frame )
        {
            paramsLo = std::min( paramsLo, gpu->Params.Offset );
            paramsHi = std::max( paramsHi, gpu->Params.Offset + sizeof( GrassParamsGpu ) );
        }
        if ( m_ParamsImage.size() * sizeof( uint32_t ) < paramsHi )
            m_ParamsImage.resize( paramsHi / sizeof( uint32_t ), 0u );
        for ( VarietyGpu* gpu : m_Frame )
        {
            std::copy( gpu->ParamsWords.begin(), gpu->ParamsWords.end(),
                       m_ParamsImage.begin() +
                            static_cast<std::ptrdiff_t>( gpu->Params.Offset / sizeof( uint32_t ) ) );
            if ( gpu->ChunksEpoch != 0u && gpu->ChunksEpoch != chunksEpoch && gpu->Chunks.Bytes != 0u )
            {
                // A remade chunks pool held none of the build's chunks: empty ones this frame, the tile pool
                // made anew from the next (its tiles built again).
                gpu->ChunkInit.assign( gpu->Chunks.Bytes / 4u, 0u );
                if ( gpu->ChunkInit.size() > 2u )
                {
                    gpu->ChunkInit[0] = gpu->ChunkRoom;
                    gpu->ChunkInit[2] = gpu->ChunkRoom;
                }
                gpu->TilePoolMade    = false;
                gpu->TileWalkSettled = false;
            }
            if ( !gpu->ChunkInit.empty() )
            {
                const std::span<const uint32_t> words( gpu->ChunkInit.data(), gpu->ChunkInit.size() );
                graph.QueueBufferUpload( m_ChunksPool.Ref(), gpu->Chunks.Offset, std::as_bytes( words ) );
                gpu->ChunkInit.clear();
                gpu->ChunkInit.shrink_to_fit(); // a pool's chunks are uploaded once, not kept on the host
            }
            gpu->ChunksEpoch = chunksEpoch;
            if ( gpu->TilePoolEpoch != tableEpoch && !gpu->TileSegments.empty() )
            {
                m_SegmentTableScratch.clear();
                for ( const uint32_t index : gpu->TileSegments )
                    m_SegmentTableScratch.push_back( m_TileSegments[index].Address );
                const std::span<const uint64_t> table( m_SegmentTableScratch.data(),
                                                       m_SegmentTableScratch.size() );
                graph.QueueBufferUpload( m_SegmentTablesPool.Ref(), gpu->TilePool.Offset, std::as_bytes( table ) );
            }
            gpu->TilePoolEpoch = tableEpoch;
        }
        const std::span<const uint32_t> params(
             m_ParamsImage.data() + static_cast<std::ptrdiff_t>( paramsLo / sizeof( uint32_t ) ),
             static_cast<size_t>( ( paramsHi - paramsLo ) / sizeof( uint32_t ) ) );
        graph.QueueBufferUpload( m_ParamsPool.Ref(), paramsLo, std::as_bytes( params ) );
        return BOOLSUCCESS;
    }

    uint64_t GrassRenderer::TilePoolBytes( const VarietyGpu* const except ) const
    {
        uint64_t segments = m_TileSegmentCount;
        if ( except != nullptr )
            segments -= static_cast<uint64_t>( except->TileSegments.size() );
        return segments * kGrassTileSegmentBytes;
    }

    uint64_t GrassRenderer::TilePoolCap()
    {
        if ( m_FrameTilePoolCap )
            return *m_FrameTilePoolCap;
        const std::shared_ptr<Engine::Device> device = EngineContext::GetInstance().GetDevice();
        if ( !device )
            return 0u;
        const Engine::DeviceMemoryReport report = device->QueryMemory();
        uint64_t                         local  = 0u;
        for ( const Engine::DeviceMemoryHeap& heap : report.Heaps )
        {
            if ( !heap.DeviceLocal )
                continue;
            if ( report.BudgetKnown )
                local += heap.Budget;
            else
                local += heap.Size;
        }
        m_FrameTilePoolCap = static_cast<uint64_t>( static_cast<double>( local ) * kGrassTilePoolBudgetShare );
        return *m_FrameTilePoolCap;
    }

    bool GrassRenderer::TakeTileSegment( uint32_t& index )
    {
        if ( m_FreeTileSegments.empty() )
        {
            m_TileSegments.emplace_back();
            m_FreeTileSegments.push_back( static_cast<uint32_t>( m_TileSegments.size() - 1u ) );
        }
        const uint32_t taken   = m_FreeTileSegments.back();
        TileSegment&   segment = m_TileSegments[taken];
        if ( !segment.Buffer )
        {
            segment.Buffer = ShaderResources::StorageBuffer::CreateDeviceLocal(
                 "GrassTilePool", static_cast<uint32_t>( kGrassTileSegmentBytes ), 14 );
            if ( !segment.Buffer )
            {
                LOG_ERROR( "[Grass] a tile pool segment was not made; its pool is made anew next frame" );
                return false;
            }
            const Common::ResultStr<uint64_t> address = segment.Buffer->GetDeviceAddress();
            if ( !address )
            {
                LOG_ERROR( "[Grass] a tile pool segment has no device address; its pool is made anew next "
                           "frame: {}",
                           address.GetError() );
                segment.Buffer.reset();
                return false;
            }
            segment.Address = address.GetValue();
        }
        m_FreeTileSegments.pop_back();
        segment.Held = true;
        ++m_TileSegmentCount;
        index = taken;
        return true;
    }

    void GrassRenderer::ReleaseTileSegments( VarietyGpu& gpu )
    {
        for ( const uint32_t index : gpu.TileSegments )
        {
            m_TileSegments[index].Held = false;
            m_TileSegments[index].Ref  = {};
            m_FreeTileSegments.push_back( index );
        }
        m_TileSegmentCount -= static_cast<uint64_t>( gpu.TileSegments.size() );
        gpu.TileSegments.clear();
    }

    void GrassRenderer::TrimTileSegments()
    {
        // As a pool's own segments were before CPU-GRASS-POOL-2: a released buffer is deleted once no frame in
        // flight holds it. The index stays free and takes a new buffer when a pool grows.
        for ( const uint32_t index : m_FreeTileSegments )
        {
            TileSegment& segment = m_TileSegments[index];
            segment.Buffer.reset();
            segment.Address = 0u;
            segment.Import  = RDG::ExternalBuffer{};
        }
    }

    bool GrassRenderer::ImportTileSegments( RDG::Builder& graph )
    {
        for ( TileSegment& segment : m_TileSegments )
        {
            segment.Ref = {};
            if ( !segment.Held )
                continue;
            if ( const Common::BoolResultStr imported = Renderer::ImportBuffer( segment.Buffer, segment.Import );
                 !imported )
            {
                LOG_ERROR( "[Grass] a tile pool segment is not in the frame graph; no tile pass or cull runs this "
                           "frame: {}",
                           imported.GetError() );
                return false;
            }
            segment.Ref = graph.RegisterExternal( segment.Import, "GrassTilePool" );
        }
        return true;
    }

    void GrassRenderer::DeclareTileSegments( RDG::PassBuilder& pass, const RDG::Access access ) const
    {
        for ( const TileSegment& segment : m_TileSegments )
        {
            if ( !segment.Held )
                continue;
            if ( access == RDG::Access::StorageWrite )
                pass.Write( segment.Ref, access );
            else
                pass.Read( segment.Ref, access );
        }
    }

    bool GrassRenderer::EnsureTileFeedback( VarietyGpu& gpu )
    {
        if ( gpu.FeedbackSlot == kNoFeedbackSlot )
            gpu.FeedbackSlot = m_TileFeedbackSlots++;
        if ( m_TileFeedback && gpu.FeedbackSlot < m_TileFeedbackRoom )
            return true;
        // Twice the slots handed out, so a scene's varieties arriving one by one remake it a few times at most.
        constexpr uint32_t kLeastSlots = 64u;
        const uint32_t     room        = std::max( kLeastSlots, m_TileFeedbackSlots * 2u );
        m_TileFeedbackRoom             = 0u;
        m_TileFeedback                 = ShaderResources::StorageBuffer::CreateReadback( "GrassTileFeedback",
                                                                                         room * kGrassTileFeedbackWords * 4u, 13 );
        if ( !m_TileFeedback )
        {
            LOG_ERROR( "[Grass] the tile feedback ({} varieties) was not made; no tile is built this frame",
                       room );
            return false;
        }
        m_TileFeedbackRoom = room;
        // The frames in flight's reports were in the old buffer: a build that ran out of pages there would go
        // unseen, its tile holding none, so every pool is made anew (its tiles built again).
        for ( auto& [key, other] : m_Varieties )
        {
            other.TilePoolMade    = false;
            other.TileWalkSettled = false;
        }
        return true;
    }

    void GrassRenderer::CommitTileWords( RDG::Builder& graph )
    {
        if ( m_FrameTileFree.empty() && m_FrameTileBuild.empty() )
            return; // no tile pass reads or writes the words this frame
        const Common::BoolResultStr committed = m_TileWordsPool.Commit( graph );
        if ( committed && m_TileWordsPool.Epoch() == m_TileWordsEpoch )
            return; // every held range kept its words (a grown pool's were copied over)
        // The words held were not carried: only a pool Init writes this frame (in m_FrameTileFree, PoolFresh)
        // has words; every other variety's pool is made anew next frame, and no tile pass of it runs on this one.
        const auto initialising = [this]( const VarietyGpu& gpu ) noexcept
        {
            if ( !gpu.PoolFresh )
                return false;
            return std::find( m_FrameTileFree.begin(), m_FrameTileFree.end(), &gpu ) != m_FrameTileFree.end();
        };
        if ( !committed )
        {
            LOG_ERROR( "[Grass] the tile pool words are not in the frame graph; no tile pass runs this frame: {}",
                       committed.GetError() );
            for ( auto& [key, gpu] : m_Varieties )
                gpu.TilePoolMade = false;
            m_FrameTileFree.clear();
            m_FrameTileBuild.clear();
            return;
        }
        m_TileWordsEpoch = m_TileWordsPool.Epoch();
        for ( auto& [key, gpu] : m_Varieties )
            if ( !initialising( gpu ) )
                gpu.TilePoolMade = false;
        std::erase_if( m_FrameTileFree, [&]( const VarietyGpu* gpu ) { return !initialising( *gpu ); } );
        std::erase_if( m_FrameTileBuild, [&]( const VarietyGpu* gpu ) { return !initialising( *gpu ); } );
    }

    void GrassRenderer::CommitTileTable( RDG::Builder& graph )
    {
        m_TileTableRef    = {};
        m_TileFeedbackRef = {};
        if ( m_FrameTileFree.empty() && m_FrameTileBuild.empty() )
            return;
        const auto            entries = static_cast<uint32_t>( m_TileTableImage.size() );
        Common::BoolResultStr done    = BOOLSUCCESS;
        if ( !m_TileFeedback )
            done = Common::MakeError( "the tile feedback was not made" );
        if ( done && ( !m_TileTable || m_TileTableRoom < entries ) )
        {
            // Half again over this frame's entries, so the table is rarely remade (per frame in flight, written
            // whole every tile frame: nothing in it is kept).
            const uint32_t room = std::max( entries + entries / 2u, 64u );
            m_TileTable         = ShaderResources::StorageBuffer::Create(
                 "GrassTileTable", room * static_cast<uint32_t>( sizeof( glm::uvec4 ) ), 12 );
            m_TileTableRoom = 0u;
            if ( !m_TileTable )
                done = Common::MakeFormattedError( "the tile table ({} entries) was not made", room );
            else
                m_TileTableRoom = room;
        }
        if ( done )
            done = m_TileTable->SetData( m_TileTableImage.data(),
                                         entries * static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
        if ( done )
            done = Renderer::ImportBuffer( m_TileTable, m_TileTableImport );
        if ( done )
            done = Renderer::ImportBuffer( m_TileFeedback, m_TileFeedbackImport );
        if ( !done )
        {
            // No tile pass this frame; a new pool next frame builds the varieties' tiles again.
            LOG_ERROR( "[Grass] the tile table is not in the frame graph; no tile is built this frame: {}",
                       done.GetError() );
            for ( VarietyGpu* gpu : m_FrameTileFree )
                gpu->TilePoolMade = false;
            for ( VarietyGpu* gpu : m_FrameTileBuild )
                gpu->TilePoolMade = false;
            m_FrameTileFree.clear();
            m_FrameTileBuild.clear();
            return;
        }
        m_TileTableRef    = graph.RegisterExternal( m_TileTableImport, "GrassTileTable" );
        m_TileFeedbackRef = graph.RegisterExternal( m_TileFeedbackImport, "GrassTileFeedback" );
        // A fresh pool's Init pass runs this frame (every fresh pool frees: m_FrameTileFree).
        for ( VarietyGpu* gpu : m_FrameTileFree )
            if ( gpu->PoolFresh )
                gpu->TilePoolMade = true;
    }

    bool GrassRenderer::UpdateTiles( VarietyGpu& gpu, std::vector<uint32_t> placement, std::vector<uint32_t> thin,
                                     const glm::vec2 eye, const float cell, const float radius, const float stray )
    {
        const float side = static_cast<float>( kGrassTileCells ) * cell;
        // The tiles any of whose instances can lie within the radius (and the band of one tile past it a tile is
        // kept in) fit a square of `span` tiles around the eye's: the pool's slots.
        const int32_t  reach = static_cast<int32_t>( std::ceil( ( radius + stray ) / side ) ) + 1;
        const auto     span  = static_cast<uint32_t>( 2 * reach + 1 );
        const uint32_t slots = span * span;
        // The nearest an instance of tile (x, z) can lie to the eye on the ground (its 3D distance is at least
        // that).
        const auto nearestOf = [&]( const int32_t x, const int32_t z )
        {
            const glm::vec2 lo = glm::vec2( static_cast<float>( x ), static_cast<float>( z ) ) * side - stray;
            const glm::vec2 hi = lo + ( side + 2.0f * stray );
            return glm::length( glm::max( glm::max( lo - eye, eye - hi ), glm::vec2( 0.0f ) ) );
        };
        // The nearest a tile @p nearest from the eye is built for: less a tile and an eighth of the distance, so
        // walking rebuilds a tile every eighth of the way towards it, not every frame.
        const auto builtFor = [side]( const float nearest )
        { return std::max( 0.0f, nearest - side - 0.125f * nearest ); };
        const auto atX = static_cast<int32_t>( std::floor( eye.x / side ) );
        const auto atZ = static_cast<int32_t>( std::floor( eye.y / side ) );

        // THE POOL'S FEEDBACK (UE: a component's instance buffer holds what its builder produced): TileFeedback,
        // FramesInFlight frames old, says a build of this pool generation ran out of pages - its tile holds none.
        // A copy is cleared once read, so a frame that builds nothing leaves no stale word.
        // CPU-GRASS-POOL-2: the variety's words of the shared feedback (EnsureTileFeedback handed them out).
        bool ranOut = false;
        if ( m_TileFeedback && gpu.FeedbackSlot < m_TileFeedbackRoom )
        {
            constexpr uint32_t                            feedbackBytes = kGrassTileFeedbackWords * 4u;
            const uint32_t                                at            = gpu.FeedbackSlot * feedbackBytes;
            std::array<uint32_t, kGrassTileFeedbackWords> read{};
            if ( const auto got = m_TileFeedback->ReadBack( read.data(), feedbackBytes, at ); !got.IsSuccess() )
            {
                LOG_ERROR( "[Grass] the tile pool's feedback was not read back: {}", got.GetError() );
            }
            else if ( read != std::array<uint32_t, kGrassTileFeedbackWords>{} )
            {
                ranOut = read[0] == gpu.PoolGeneration && read[1] != 0u;
                const std::array<uint32_t, kGrassTileFeedbackWords> cleared{};
                if ( const auto wrote = m_TileFeedback->SetData( cleared.data(), feedbackBytes, at );
                     !wrote.IsSuccess() )
                {
                    LOG_ERROR( "[Grass] the tile pool's feedback was not cleared: {}", wrote.GetError() );
                }
            }
        }
        // THE POOL'S SIZE (GRASS-TILE-2): the most the resident tiles hold. A tile built for the nearest b keeps
        // the cells whose fade draw beats the field's share there (GrassFieldShare.glslh GrassFieldShareAt: (S /
        // b)^2 past StartCullDistance S, UE's fade to nothing at EndCullDistance, the screen-width floor): a
        // binomial count over its cells, bounded by its mean and three deviations at full layer weight; the pool
        // holds it in whole segments (THE POOL'S GROWTH below). TURN-CPU-SETUP-1: the bound and the walk below
        // read only the walk's inputs (VarietyGpu::TileWalkKey); the same inputs give the same bound, and - after
        // a walk that changed no tile - the same table.
        const std::array<uint32_t, 8> walkKey{ std::bit_cast<uint32_t>( eye.x ),
                                               std::bit_cast<uint32_t>( eye.y ),
                                               std::bit_cast<uint32_t>( cell ),
                                               std::bit_cast<uint32_t>( radius ),
                                               std::bit_cast<uint32_t>( stray ),
                                               thin[0],
                                               thin[1],
                                               thin[2] };
        const bool                    sameWalk = gpu.TileBoundKnown && gpu.TileWalkKey == walkKey;
        if ( !sameWalk )
        {
            const float startCull  = std::bit_cast<float>( thin[0] );
            const float endCull    = std::bit_cast<float>( thin[1] );
            const float floorReach = std::bit_cast<float>( thin[2] );
            const auto  thinShare  = []( const float distance, const float from )
            { return from > 0.0f ? std::min( 1.0f, from * from / std::max( distance * distance, 1.0f ) ) : 1.0f; };
            constexpr auto cells = static_cast<float>( kGrassTileCells * kGrassTileCells );
            uint32_t       bound = 0u;
            for ( int32_t z = atZ - reach; z <= atZ + reach; ++z )
                for ( int32_t x = atX - reach; x <= atX + reach; ++x )
                {
                    const float nearest = nearestOf( x, z );
                    if ( nearest > radius + side )
                        continue;
                    const float built = builtFor( nearest );
                    const float faded = std::clamp( ( built - startCull ) / std::max( endCull - startCull, 1.0f ), 0.0f, 1.0f );
                    const float share =
                         std::min( thinShare( built, startCull ) * ( 1.0f - faded ), thinShare( built, floorReach ) );
                    const float mean = cells * share;
                    const float most = std::min( cells, mean + 3.0f * std::sqrt( mean ) );
                    bound += static_cast<uint32_t>( std::ceil( most / static_cast<float>( kGrassTilePageRecords ) ) );
                }
            gpu.TileBound      = bound;
            gpu.TileWalkKey    = walkKey;
            gpu.TileBoundKnown = true;
        }
        // THE POOL'S GROWTH (GRASS-VRAM-BUDGET-1; UE: a pooled buffer grows by pages inside its pool's budget -
        // r.Streaming.PoolSize against VK_EXT_memory_budget - and never holds two full copies): the pool holds the
        // bound in whole segments of kGrassTileSegmentPages pages, each its own buffer, and grows by appending
        // segments - nothing is copied, no tile is built again, nothing is released (the old doubling made a new
        // pool and owed the old one's deletion for FramesInFlight frames). A build that ran out of pages (the bound
        // is three deviations) grows it by a segment and resets it on the same buffers: every tile is built again,
        // as the tiles that found no page hold none. It never passes its room (every slot's tile at its most
        // pages) nor the grass pools' share of the device-local budget (TilePoolCap): a pool held there keeps its
        // pages, and a build that finds none stays unbuilt (logged once). A new placement, grid or more slots make
        // the pool anew.
        const bool     fresh = !gpu.TilePoolMade || gpu.PlacementKey != placement || gpu.TileSide != side ||
                          slots > gpu.TileSlots;
        const uint32_t poolSlots = fresh ? slots : gpu.TileSlots;
        const uint32_t room      = SegmentPages( poolSlots * kGrassTileMostPages );
        const uint32_t held  = fresh ? 0u : gpu.TilePages;
        uint32_t       pages = std::max( held, gpu.TileBound );
        if ( ranOut )
            pages = std::max( pages, held + kGrassTileSegmentPages );
        pages = std::min( SegmentPages( pages ), room );
        if ( pages > held )
        {
            const uint64_t others = TilePoolBytes( &gpu );
            const uint64_t cap    = TilePoolCap();
            const uint64_t fits   = cap > others ? ( cap - others ) / kGrassTilePageBytes : 0u;
            const auto     most   = static_cast<uint32_t>(
                 std::min<uint64_t>( fits / kGrassTileSegmentPages * kGrassTileSegmentPages, pages ) );
            if ( most < pages )
            {
                if ( !gpu.TilePoolCapped )
                {
                    LOG_INFO( "[Grass] a variety's tile pool is held at {} of the {} pages it wants: the grass pools "
                              "hold {:.0f} MB of their {:.0f} MB share of the device-local budget; builds that find "
                              "no page stay unbuilt",
                              std::max( held, most ), pages, static_cast<double>( others ) / ( 1024.0 * 1024.0 ),
                              static_cast<double>( cap ) / ( 1024.0 * 1024.0 ) );
                }
                gpu.TilePoolCapped = true;
                // A new pool holds one segment at least: it is what its nearest tiles build in.
                const uint32_t least = fresh ? kGrassTileSegmentPages : 0u;
                pages                = std::max( { held, most, least } );
            }
            else
            {
                gpu.TilePoolCapped = false;
            }
        }
        const bool reset = !fresh && ranOut && pages > held;
        gpu.PoolFresh    = fresh || reset;
        if ( gpu.PoolFresh )
        {
            gpu.TileWalkSettled = false; // every tile is built again: no table is kept
            for ( VarietyGpu::ResidentTile& tile : gpu.TileRing )
                tile.Held = false;
            gpu.FreeSlots.clear();
            gpu.TilePoolMade   = false; // this frame's Init pass writes the words first (AddFrame, TilePoolMade)
            gpu.PoolGeneration = gpu.PoolGeneration + 1u == 0u ? 1u : gpu.PoolGeneration + 1u;
        }
        if ( fresh )
        {
            gpu.TileSlots    = slots;
            gpu.TileRoom     = room;
            gpu.TileSide     = side;
            gpu.TilePages    = 0u;
            gpu.PlacementKey = std::move( placement );
            // CPU-GRASS-POOL-2: the ring holds the residents of a square of span tiles about the eye (the reach
            // and the band a resident is kept in, see VarietyGpu::TileRing), plus a tile each side for the
            // rounding of floor( eye / side ).
            gpu.TileRingSide = span + 2u;
            gpu.TileRing.assign( static_cast<std::size_t>( gpu.TileRingSide ) * gpu.TileRingSide,
                                 VarietyGpu::ResidentTile{} );
            // GRASS-TILE-3 / GRASS-VIEW-LAW-1: the segments and the pool's words live in video memory,
            // device-local storage buffers the GPU alone writes (StorageBuffer::CreateDeviceLocal): kept across
            // graphs and frames, read by address where a pass covers many varieties. The old words are released
            // once no frame in flight holds them; the segments go back to the shared ones (CPU-GRASS-POOL-2), the
            // words are a new range of the shared words pool (CPU-GRASS-POOL-3; this frame's Init writes them).
            ReleaseTileSegments( gpu );
            m_SegmentTablesPool.Release( gpu.TilePool );
            m_TileWordsPool.Release( gpu.TileWords );
            m_ChunksPool.Release( gpu.Chunks );
            gpu.ChunkInit.clear();
            const uint64_t wordBytes =
                 ( 1u + static_cast<uint64_t>( room ) + static_cast<uint64_t>( slots ) * kGrassTileHeaderWords ) * 4u;
            if ( wordBytes > UINT32_MAX )
            {
                LOG_ERROR( "[Grass] a variety's tile pool words ({} bytes) exceed a storage buffer's 4 GB; no tile is "
                           "built",
                           wordBytes );
                return false;
            }
            // CPU-GRASS-POOL-7: the segment table is a range of the shared tables pool, written whole by
            // CommitVarietyPools (TilePoolEpoch 0: not yet).
            const uint64_t tableBytes =
                 std::max<uint64_t>( room / kGrassTileSegmentPages, 1u ) * sizeof( uint64_t );
            gpu.TilePoolEpoch         = 0u;
            if ( !m_SegmentTablesPool.Allocate( gpu.TilePool, tableBytes ) )
                LOG_ERROR( "[Grass] the segment tables pool would pass a storage buffer's 4 GB; the variety's "
                           "table ({} bytes) was not handed out",
                           tableBytes );
            if ( !m_TileWordsPool.Allocate( gpu.TileWords, wordBytes ) )
                LOG_ERROR( "[Grass] the tile pool words pool would pass a storage buffer's 4 GB; the variety's "
                           "words ({} bytes) were not handed out",
                           wordBytes );
            // GRASS-VIEW-LAW-1, THE CHUNKS (Common/GrassVarietyRows.glslh, the chunk contract): the build's
            // chunks, one per tile slot and cluster - a fresh buffer's are empty, Free empties a freed slot's, Init
            // a reset pool's. The rows the view law writes are the lists' (REGRESS-CACHE-1: sized by the
            // capacities, made with the id lists).
            const uint64_t chunkCount = static_cast<uint64_t>( slots ) * kGrassTileClusters;
            if ( ( kChunkHeaderWords + chunkCount * kChunkWords ) * 4u > UINT32_MAX )
            {
                LOG_ERROR( "[Grass] a variety's chunks ({}) exceed a storage buffer's 4 GB; no tile is built",
                           chunkCount );
                return false;
            }
            // CPU-GRASS-POOL-7: a range of the shared chunks pool; its words (ChunkInit) are uploaded by
            // CommitVarietyPools before any pass of the frame reads them.
            std::vector<uint32_t>& chunks = gpu.ChunkInit;
            chunks.assign( kChunkHeaderWords + static_cast<size_t>( chunkCount ) * kChunkWords, 0u );
            gpu.ChunkRoom   = static_cast<uint32_t>( chunkCount );
            chunks[0]       = gpu.ChunkRoom;
            chunks[2]       = gpu.ChunkRoom;
            gpu.ChunksEpoch = 0u;
            if ( !m_ChunksPool.Allocate( gpu.Chunks, chunks.size() * 4u ) )
            {
                LOG_ERROR( "[Grass] the chunks pool would pass a storage buffer's 4 GB; the variety's chunks ({}) "
                           "were not handed out",
                           chunkCount );
                chunks.clear();
            }
        }
        if ( gpu.TilePool.Bytes == 0u || gpu.TileWords.Bytes == 0u || gpu.Chunks.Bytes == 0u )
        {
            LOG_ERROR( "[Grass] a variety's tile pool, pool words or chunks were not made; its pool is made anew "
                       "next frame" );
            gpu.TilePoolMade    = false;
            gpu.TileWalkSettled = false;
            return false;
        }
        // The segments the pool grows by, appended; each one's address goes into the table past the earlier ones,
        // which no frame in flight reads (its pages lie in the segments it had). A growth that is not a reset adds
        // its pages to the stack in this frame's Free pass (TileGrowFrom).
        gpu.TileGrowFrom = held;
        for ( std::size_t s = gpu.TileSegments.size(); s < pages / kGrassTileSegmentPages; ++s )
        {
            uint32_t index = 0u;
            if ( !TakeTileSegment( index ) )
            {
                gpu.TilePoolMade    = false;
                gpu.TileWalkSettled = false;
                return false;
            }
            // Held by the pool from here: a failed table write leaves it to the pool's next ReleaseTileSegments.
            gpu.TileSegments.push_back( index );
            gpu.TilePoolEpoch = 0u; // the table is written anew (CommitVarietyPools)
        }
        gpu.TilePages = static_cast<uint32_t>( gpu.TileSegments.size() ) * kGrassTileSegmentPages;
        if ( gpu.PoolFresh )
        {
            // Every page is free from the Init pass; nothing is grown on top of it.
            gpu.TileGrowFrom = gpu.TilePages;
            for ( uint32_t s = gpu.TileSlots; s > 0u; --s )
                gpu.FreeSlots.push_back( s - 1u );
        }
        // Another lens or Foliage quality thins by other distances: every tile keeps drawing until rebuilt.
        if ( gpu.ThinKey != thin )
        {
            for ( VarietyGpu::ResidentTile& tile : gpu.TileRing )
                tile.Stale = tile.Stale || tile.Held;
            gpu.ThinKey = std::move( thin );
        }
        // TURN-CPU-SETUP-1: the same inputs as the last walk, which freed and built nothing in this pool - so the
        // resident tiles, their slots and their BuiltNear are what that walk read: it frees none, builds none
        // (every tile it wants still waits for a slot) and culls the same tiles in the same order. Its table
        // stands as written (the header, no frees, no builds, TileCulls tiles nearest first); nothing is uploaded
        // for it (AddFrame writes the table only on a frame that frees or builds).
        if ( sameWalk && !gpu.PoolFresh && gpu.TileWalkSettled )
        {
            gpu.TileFrees  = 0u;
            gpu.TileBuilds = 0u;
            m_Settling     = m_Settling || gpu.TilePending;
            return true;
        }

        // The table: the header, the slots freed (the tiles past the radius by more than a tile - the band keeps
        // the eye's sway from freeing and rebuilding one - and the tiles rebuilt), the builds, the tiles culled.
        // CPU-GRASS-POOL-2: the ring's cells are walked in place (a dense array, no hashing); a tile is found by
        // its cell, which holds its coordinates.
        gpu.TileEntries.assign( 1u, glm::uvec4( gpu.PoolGeneration, 0u, 0u, 0u ) );
        for ( VarietyGpu::ResidentTile& tile : gpu.TileRing )
        {
            if ( !tile.Held || nearestOf( tile.X, tile.Z ) <= radius + side )
                continue;
            gpu.TileEntries.push_back( glm::uvec4( tile.Slot, 0u, 0u, 0u ) );
            gpu.FreeSlots.push_back( tile.Slot );
            tile.Held = false;
        }
        const auto residentAt = [&gpu]( const int32_t x, const int32_t z ) -> VarietyGpu::ResidentTile*
        {
            VarietyGpu::ResidentTile& tile = gpu.TileRing[RingCell( x, z, gpu.TileRingSide )];
            if ( tile.Held && tile.X == x && tile.Z == z )
                return &tile;
            return nullptr;
        };

        // THE BUILDS (UE grass.MaxCreatePerFrame, nearest first): a tile within the radius that is missing, stale,
        // nearer than it was built for (its far draws were not kept), or past twice what it was built for (it
        // holds several times the instances it needs). A tile is built for builtFor( its nearest ).
        std::vector<TileWant>& wanted = m_TileWants;
        wanted.clear();
        for ( int32_t z = atZ - reach; z <= atZ + reach; ++z )
            for ( int32_t x = atX - reach; x <= atX + reach; ++x )
            {
                const float nearest = nearestOf( x, z );
                if ( nearest > radius )
                    continue;
                const VarietyGpu::ResidentTile* found = residentAt( x, z );
                if ( found == nullptr || found->Stale || nearest < found->BuiltNear ||
                     nearest > 2.0f * ( found->BuiltNear + side ) )
                    wanted.push_back( { nearest, x, z } );
            }
        std::sort( wanted.begin(), wanted.end(),
                   []( const TileWant& a, const TileWant& b ) { return a.Nearest < b.Nearest; } );
        // Tiles that draw short of what the view keeps (missing, stale, approached) until built:
        // InstancesSettling.
        bool                     pending = false;
        std::vector<glm::uvec4>& builds  = m_TileBuildScratch;
        builds.clear();
        for ( const TileWant& want : wanted )
        {
            VarietyGpu::ResidentTile* found = residentAt( want.X, want.Z );
            const bool drawsShort           = found == nullptr || found->Stale || want.Nearest < found->BuiltNear;
            if ( builds.size() == kGrassTileBuildsPerFrame || ( found == nullptr && gpu.FreeSlots.empty() ) )
            {
                pending = pending || drawsShort;
                continue;
            }
            if ( found == nullptr )
            {
                // The cell is empty: the residents left in the ring lie within its span of the eye (TileRing).
                found = &gpu.TileRing[RingCell( want.X, want.Z, gpu.TileRingSide )];
                if ( found->Held )
                {
                    // Never expected (span covers the kept band); should a resident still sit here, its slot is
                    // freed rather than leaked with the tile it held.
                    gpu.TileEntries.push_back( glm::uvec4( found->Slot, 0u, 0u, 0u ) );
                    gpu.FreeSlots.push_back( found->Slot );
                }
                found->X    = want.X;
                found->Z    = want.Z;
                found->Slot = gpu.FreeSlots.back();
                found->Held = true;
                gpu.FreeSlots.pop_back();
            }
            else
                gpu.TileEntries.push_back( glm::uvec4( found->Slot, 0u, 0u, 0u ) );
            found->BuiltNear = builtFor( want.Nearest );
            found->Stale     = false;
            builds.push_back( glm::uvec4( found->Slot, static_cast<uint32_t>( want.X ),
                                          static_cast<uint32_t>( want.Z ),
                                          std::bit_cast<uint32_t>( found->BuiltNear ) ) );
        }
        gpu.TileFrees  = static_cast<uint32_t>( gpu.TileEntries.size() - 1u );
        gpu.TileBuilds = static_cast<uint32_t>( builds.size() );
        gpu.TileEntries.insert( gpu.TileEntries.end(), builds.begin(), builds.end() );
        // The tiles culled, nearest first (GRASS-TILE-2): the cull's groups run in about that order, so the full
        // list - and the camera's, which keeps its order - holds the field front to back, and the depth prepass
        // and the GBuffer reject the far blades behind the near ones early. GRASS-TILE-4: not the band past the
        // radius the residents keep - none of a tile there lies within it (an instance's distance from the eye is
        // at least nearestOf), and the cull runs a group per cluster of every tile it is given.
        std::vector<std::pair<float, uint32_t>>& culled = m_TileCullScratch;
        culled.clear();
        for ( const VarietyGpu::ResidentTile& tile : gpu.TileRing )
        {
            if ( !tile.Held )
                continue;
            const float nearest = nearestOf( tile.X, tile.Z );
            if ( nearest <= radius )
                culled.emplace_back( nearest, tile.Slot );
        }
        std::sort( culled.begin(), culled.end() );
        for ( const auto& [nearest, slot] : culled )
            gpu.TileEntries.push_back( glm::uvec4( slot, 0u, 0u, 0u ) );
        gpu.TileCulls = static_cast<uint32_t>( culled.size() );
        if ( gpu.PoolFresh || gpu.TileFrees != 0u || gpu.TileBuilds != 0u )
            ++gpu.TileVersion;
        // The next frame keeps this table while the walk's inputs stay (TileWalkSettled): only a walk that changed
        // no tile walks to itself again.
        gpu.TileWalkSettled = !gpu.PoolFresh && gpu.TileFrees == 0u && gpu.TileBuilds == 0u;
        gpu.TilePending     = pending;
        m_Settling          = m_Settling || pending;
        return true;
    }

    void GrassRenderer::ApplyPlanned( VarietyPlan& plan, const FieldGpu& fieldGpu, const GrassFieldDraw& field,
                                      const ViewFrame& frame )
    {
        // Render thread, in variety order (CommitFrame): the serial apply of a prepared and planned variety.
        const GrassVarietyDraw& variety = *plan.Variety;
        Desert::Mesh* const     mesh    = plan.MeshAsset;
        DESERT_PROFILE_PHASES( varietyPhases );
        DESERT_PROFILE_PHASE( varietyPhases, "Grass variety: Apply params + tiles" );
        if ( !ApplyVariety( plan ) )
            return;
        VarietyGpu* const gpu = plan.Gpu;
        DESERT_PROFILE_PHASE( varietyPhases, "Grass variety: Register refs" );
        // CPU-GRASS-POOL-7: no buffer of the variety's own is imported - its params, chunks, segment
        // table and feedback are ranges of the shared pools CommitVarietyPools commits once after each
        // variety; CPU-GRASS-POOL-8: its rows, ids, records, candidates and law scales are ranges
        // of the pools CommitListPools commits once after the field loop, which sets the list refs.
        gpu->FieldRef    = fieldGpu.Ref;
        gpu->FieldBuffer = fieldGpu.Buffer;
        DESERT_PROFILE_PHASE( varietyPhases, "Grass variety: Keys + tile table" );
        m_Frame.push_back( gpu );
        if ( gpu->Drawn )
            DrawCounter::RecordGrass( gpu->Drawn->x, gpu->Drawn->y, gpu->Drawn->z, gpu->DrawnCascades );
        const auto revision = static_cast<uint64_t>( fieldGpu.Revision );
        gpu->FrameKey.push_back( static_cast<uint32_t>( revision ) );
        gpu->FrameKey.push_back( static_cast<uint32_t>( revision >> 32u ) );
        if ( gpu->FrameKey != gpu->GenerateKey )
        {
            gpu->GenerateKey = gpu->FrameKey;
            ++gpu->Generation;
        }
        gpu->LawKey.push_back( static_cast<uint32_t>( revision ) );
        gpu->LawKey.push_back( static_cast<uint32_t>( revision >> 32u ) );
        if ( gpu->LawKey != gpu->LawedKey )
        {
            gpu->LawedKey = gpu->LawKey;
            ++gpu->LawGeneration;
        }
        // GRASS-VIEW-LAW-1: nothing is generated per view - the law is GrassCull's, every frame, over
        // the build's chunks. The tile passes run on a frame that frees or builds tiles and read the
        // table written then (a per-frame-in-flight copy); a fresh pool's Init pass writes its words
        // first.
        const bool grown = gpu->TileGrowFrom != gpu->TilePages; // GRASS-VRAM-BUDGET-1
        bool       tiles = gpu->PoolFresh || grown || gpu->TileFrees != 0u || gpu->TileBuilds != 0u;
        if ( tiles )
        {
            // CPU-GRASS-POOL-2: the variety's run of the shared tile table, written once for every
            // variety (CommitTileTable, which marks a fresh pool made). CPU-GRASS-POOL-3: its words
            // are a range of the shared words pool, imported once after every variety
            // (CommitTileWords).
            gpu->TileTableBase = static_cast<uint32_t>( m_TileTableImage.size() );
            m_TileTableImage.insert( m_TileTableImage.end(), gpu->TileEntries.begin(), gpu->TileEntries.end() );
        }
        if ( tiles && ( gpu->PoolFresh || grown || gpu->TileFrees != 0u ) )
            m_FrameTileFree.push_back( gpu );
        if ( tiles && gpu->TileBuilds != 0u )
            m_FrameTileBuild.push_back( gpu );
        // The camera's list depends on last frame's depth when it is HZB-tested, so it is culled every
        // such frame; by the camera's planes alone it follows the generation's inputs (the planes are
        // among them), so it is culled when the variety generates.
        // Every frame: the camera's planes and the cascades' boxes are this frame's
        // (GRASS-CULL-UNIFY-1).
        m_FrameCull.push_back( gpu );
        DESERT_PROFILE_PHASE( varietyPhases, "Grass variety: Texture demand" );
        NoteVarietyTextureDemand( variety, *mesh, *field.Field, frame,
                                  ScaledPlacement( variety.Placement, m_Quality ).EndCullDistance );

        // CPU-GRASS-POOL-8: the sections name the list pools' buffers and refs, so they are built
        // after CommitListPools (below the field loop).
        m_FrameSubmits.push_back( FrameSubmit{ .Variety = &variety, .MeshAsset = mesh, .Gpu = gpu } );
    }

    void GrassRenderer::PrepareFrame( const ViewFrame& frame, const MeshRenderer& meshes, const bool rayTracing,
                                      const GrassOcclusion& occlusion, const bool parallelSetup )
    {
        // TURN-CPU-SETUP-1: the frame's grass by stage, one profile row each (inside SceneRenderer's "Setup:
        // Grass plan").
        DESERT_PROFILE_PHASES( grassPhases );
        DESERT_PROFILE_PHASE( grassPhases, "Grass: View volumes" );
        if ( !m_Plan )
            m_Plan = std::make_unique<GrassFramePlan>();
        GrassFramePlan& framePlan = *m_Plan;
        framePlan.Frame           = &frame;
        framePlan.Meshes          = &meshes;
        framePlan.Occlusion       = occlusion;
        framePlan.Parallel        = parallelSetup;
        framePlan.Ready           = false;
        framePlan.Varieties.clear();
        framePlan.ResumeField  = std::numeric_limits<std::size_t>::max();
        framePlan.ResumeCursor = 0u;
        const bool testsHzb    = occlusion.Hzb.IsValid();
        // The two volumes (GRASS-VIS-6): the camera's side planes, and the same planes swept along every light
        // whose cascades this frame fitted (UpdateCascades ran before this) for the full list the cascades and
        // the TLAS draw. While they differ the camera keeps a list of its own even with no HZB to test.
        ViewVolumes& volumes = framePlan.Volumes;
        volumes              = ViewVolumes{};
        SidePlanes( frame.ViewProjection, volumes.Camera );
        volumes.List = testsHzb ? CameraList::PlanesAndHzb : CameraList::Planes;
        // GPU-COST-21: the cascade views this frame draws, each with its own list culled by its caster box - the
        // six planes of the matrix it draws with (UpdateCascades fitted it above).
        {
            const MeshRenderer::CascadeCasterBoxes boxes = meshes.CascadeCasterBoxSet();
            volumes.CascadeMask                          = boxes.Live;
            uint32_t list                                = 0;
            for ( uint32_t v = 0; v < kGrassCascadeLists; ++v )
            {
                if ( ( boxes.Live & ( 1u << v ) ) == 0u )
                    continue;
                // The CLOSED box here: the guard band needs its eight corners (BoxCorners), which an open side has
                // not; the list's copy below opens the sun's side (the blades draw pancaked,
                // CascadeCasterFrustum).
                const Core::Frustum box( boxes.ViewProj[v], glm::mat4( 1.0f ) );
                glm::vec4           planes[6];
                for ( uint32_t i = 0; i < 6u; ++i )
                {
                    const Core::Frustum::Plane& plane = box.GetPlane( static_cast<Core::FrustumPlane>( i ) );
                    planes[i]                         = glm::vec4( plane.Normal, plane.Distance );
                }
                // GRASS-SHADOW-1: who sways in this list's cascade, by the rule its caster draws keep
                // (BuildGpuInstancedShadowDraws: the light camera's CasterWindOrigin).
                volumes.CascadeViews[list] = v;
                volumes.CascadeWind[list]  = meshes.CasterWindOrigin( v );
                // GRASS-CULL-HIER-1: the list is culled against its guard box, grown anew only once this frame's box
                // leaves it or the list draws another view (kCascadeGuardTurn).
                CascadeGuard& guard = m_CascadeGuards[list];
                if ( guard.View != v || !BoxInsideGuard( planes, guard.Planes ) )
                {
                    guard.View = v;
                    GuardBox( planes, glm::vec3( volumes.CascadeWind[list] ), guard.Planes );
                }
                std::copy( std::begin( guard.Planes ), std::end( guard.Planes ), volumes.CascadePlanes + list * 6u );
                // Without the sun's side, as Graphic::CascadeCasterFrustum culls a sun cascade's casters: a zero
                // plane (Frustum::Open) holds every blade.
                volumes.CascadePlanes[list * 6u + Core::PLANE_FAR] = glm::vec4( 0.0f );
                ++list;
            }
            // A list no view draws this frame forgets its guard box: the view it draws next grows its own.
            for ( ; list < kGrassCascadeLists; ++list )
                m_CascadeGuards[list].View = kNoCascadeView;
        }
        m_Frame.clear();
        m_FrameSubmits.clear();
        m_FrameCull.clear();
        m_FrameTileFree.clear();
        m_FrameTileBuild.clear();
        m_FrameTilePoolCap.reset();
        m_TileTableImage.clear();
        m_Settling       = false;
        m_FrameTested    = testsHzb;
        m_FrameNeutral   = occlusion.Neutral;
        m_FrameTable     = false;
        m_VisibilityRows = 0;
        m_BladeRayTracing.BeginFrame( rayTracing );
        if ( m_Queue.empty() || !m_Generate || !m_Cull )
            return;

        // FPS-GRASS-PLAN: the varieties the plan task prepares and plans, in field and variety order - each with
        // its mesh (BladeStrip makes a strip on first use), its entry (inserted here: a task never inserts) and
        // its read-back counts. A field's repeated key ends the plan there (RISK 8: the repeat is prepared after
        // its first's apply); CommitFrame runs the rest serially from that variety on.
        DESERT_PROFILE_PHASE( grassPhases, "Grass: Pick varieties" );
        m_FrameSnowDepth = std::max( m_SceneRenderer->GetWorldParameterScalar( "SnowDepth" ), 0.0f );
        framePlan.Ready  = true;
        for ( std::size_t f = 0; f < m_Queue.size(); ++f )
        {
            const GrassFieldDraw& field = m_Queue[f];
            if ( !field.Field || field.Field->Words.size() < 8 )
                continue;
            const std::size_t fieldFirst = framePlan.Varieties.size();
            for ( std::size_t cursor = 0; cursor < field.Varieties.size(); ++cursor )
            {
                const GrassVarietyDraw& variety = field.Varieties[cursor];
                Desert::Mesh* const     mesh    = DrawableMesh( variety );
                if ( mesh == nullptr )
                    continue;
                const std::span<const VarietyPlan> fieldPlans =
                     std::span<const VarietyPlan>( framePlan.Varieties ).subspan( fieldFirst );
                if ( RepeatedKey( fieldPlans, variety ) )
                {
                    framePlan.ResumeField  = f;
                    framePlan.ResumeCursor = cursor;
                    return;
                }
                VarietyPlan& plan = framePlan.Varieties.emplace_back();
                plan.Variety      = &variety;
                plan.MeshAsset    = mesh;
                plan.Gpu          = &m_Varieties[variety.Key];
                plan.FieldIndex   = f;
                ReadVarietyFeedback( plan );
            }
        }
    }

    void GrassRenderer::PlanFrame() noexcept
    {
        // FPS-GRASS-PLAN, on the caller's setup task (SceneRenderer "Grass: Plan frame"). It writes the grass's
        // own state only - the planned entries, the list pools' CPU ranges, the visibility rows, the feedback
        // marks, m_Settling - and reads the view, the queue, the field words and the mesh renderer's fitted
        // cascades (const; UpdateCascades ran before PrepareFrame). No GPU object, no service, no graph: those are
        // CommitFrame's. The same steps, in the same variety order, whichever thread runs them.
        if ( !m_Plan || !m_Plan->Ready )
            return;
        GrassFramePlan&           framePlan = *m_Plan;
        std::vector<VarietyPlan>& plans     = framePlan.Varieties;
        for ( VarietyPlan& plan : plans )
        {
            const GrassFieldDraw& field    = m_Queue[plan.FieldIndex];
            const auto            revision = static_cast<uint64_t>( field.Field->Revision );
            plan.Prepared = PrepareVariety( *plan.Variety, *plan.MeshAsset, *framePlan.Frame, framePlan.Volumes,
                                            *framePlan.Meshes, revision, plan ) != nullptr;
        }
        // RISK 9 (UE ParallelFor MinBatchSize / FParallelMeshDrawCommandPass): the plans in contiguous slices, one
        // task each (a TaskGroup inside a task: its Join runs the unstarted slices here), never under
        // kMinGrassPlansPerTask plans a slice. Each plan is written by exactly one task, in variety order.
        Common::TaskGroup tasks( framePlan.Parallel );
        tasks.LaunchSlices( "Grass variety: Plan", plans.size(), kMinGrassPlansPerTask,
                            [this, &plans]( const std::size_t first, const std::size_t last ) noexcept
                            {
                                for ( std::size_t i = first; i < last; ++i )
                                    if ( plans[i].Prepared )
                                        PlanVariety( plans[i] );
                            } );
        tasks.Join();
    }

    void GrassRenderer::CommitFrame( RDG::Builder& graph, MeshRenderer& meshes )
    {
        // FPS-GRASS-PLAN, render thread after the plan task's join: the field buffers, the varieties' GPU objects
        // and applies in variety order, the serial rest past a repeated key, then the pools, sections and passes.
        if ( !m_Plan || !m_Plan->Ready )
            return;
        GrassFramePlan& framePlan       = *m_Plan;
        framePlan.Ready                 = false;
        const ViewFrame&      frame     = *framePlan.Frame;
        const ViewVolumes&    volumes   = framePlan.Volumes;
        const GrassOcclusion& occlusion = framePlan.Occlusion;
        const bool            testsHzb  = m_FrameTested;
        DESERT_PROFILE_PHASES( grassPhases );
        DESERT_PROFILE_PHASE( grassPhases, "Grass: Field buffers" );
        framePlan.FieldReady.assign( m_Queue.size(), uint8_t{ 0u } );
        for ( std::size_t f = 0; f < m_Queue.size(); ++f )
        {
            const GrassFieldDraw& field = m_Queue[f];
            if ( !field.Field || field.Field->Words.size() < 8 )
                continue;
            FieldGpu& fieldGpu = m_Fields[field.Root];
            if ( !fieldGpu.Buffer || fieldGpu.Revision != field.Field->Revision )
            {
                const auto bytes = static_cast<uint32_t>( field.Field->Words.size() * sizeof( uint32_t ) );
                fieldGpu.Buffer  = ShaderResources::StorageBuffer::Create( "GrassField", bytes, 0, true );
                if ( const auto wrote = fieldGpu.Buffer->SetData( field.Field->Words.data(), bytes );
                     !wrote.IsSuccess() )
                {
                    LOG_ERROR( "[Grass] the landscape's grass field was not uploaded: {}", wrote.GetError() );
                    fieldGpu.Buffer.reset();
                    continue;
                }
                fieldGpu.Revision = field.Field->Revision;
            }
            if ( const auto imported = Renderer::ImportBuffer( fieldGpu.Buffer, fieldGpu.Import ); !imported )
            {
                LOG_ERROR( "[Grass] the grass field is not in the frame graph: {}", imported.GetError() );
                continue;
            }
            fieldGpu.Ref = graph.RegisterExternal( fieldGpu.Import, "GrassField" );
            framePlan.FieldReady[f] = 1u;
        }

        // RENDER-PARALLEL-SETUP: the planned varieties' serial half after their plans, in variety order: the GPU
        // objects PrepareVariety decided on, every variety's words of the shared tile feedback before any reads
        // them (CPU-GRASS-POOL-2: a remade feedback makes every pool anew, so it is not remade between two
        // varieties' tile updates), then the applies. Every shared write keeps the variety order:
        // RenderPipeline.ParallelSetup 0, 1 make the same frame.
        DESERT_PROFILE_PHASE( grassPhases, "Grass: Apply varieties" );
        std::vector<VarietyPlan>& planned = framePlan.Varieties;
        for ( VarietyPlan& plan : planned )
            plan.Prepared =
                 plan.Prepared && framePlan.FieldReady[plan.FieldIndex] != 0u && CommitVarietyObjects( plan );
        for ( VarietyPlan& plan : planned )
            if ( plan.Prepared && !EnsureTileFeedback( *plan.Gpu ) )
                break;
        for ( VarietyPlan& plan : planned )
        {
            if ( !plan.Prepared )
                continue;
            const GrassFieldDraw& field = m_Queue[plan.FieldIndex];
            ApplyPlanned( plan, m_Fields[field.Root], field, frame );
        }

        // RISK 8: past a field's repeated key the varieties run here as before the plan task, in contiguous
        // SEGMENTS of unique keys - each prepared, planned and applied before the next one is prepared, so the
        // repeat is prepared after its first's apply. With no repeat (ResumeField past the queue) nothing runs.
        DESERT_PROFILE_PHASE( grassPhases, "Grass: Serial varieties" );
        for ( std::size_t f = framePlan.ResumeField; f < m_Queue.size(); ++f )
        {
            if ( framePlan.FieldReady[f] == 0u )
                continue;
            const GrassFieldDraw&     field    = m_Queue[f];
            const FieldGpu&           fieldGpu = m_Fields[field.Root];
            std::vector<VarietyPlan>& plans    = framePlan.Serial;
            std::size_t               cursor   = 0u;
            if ( f == framePlan.ResumeField )
                cursor = framePlan.ResumeCursor;
            while ( cursor < field.Varieties.size() )
            {
                plans.clear();
                for ( ; cursor < field.Varieties.size(); ++cursor )
                {
                    const GrassVarietyDraw& variety = field.Varieties[cursor];
                    Desert::Mesh* const     mesh    = DrawableMesh( variety );
                    if ( mesh == nullptr )
                        continue;
                    if ( RepeatedKey( plans, variety ) )
                        break; // the next segment starts at the repeat, after this one's applies
                    DESERT_PROFILE_PHASES( varietyPhases );
                    DESERT_PROFILE_PHASE( varietyPhases, "Grass variety: Prepare" );
                    VarietyPlan& plan = plans.emplace_back();
                    plan.Variety      = &variety;
                    plan.MeshAsset    = mesh;
                    plan.Gpu          = &m_Varieties[variety.Key];
                    plan.FieldIndex   = f;
                    ReadVarietyFeedback( plan );
                    const auto revision = static_cast<uint64_t>( fieldGpu.Revision );
                    plan.Prepared =
                         PrepareVariety( variety, *mesh, frame, volumes, meshes, revision, plan ) != nullptr &&
                         CommitVarietyObjects( plan );
                    if ( !plan.Prepared )
                        plans.pop_back();
                }
                {
                    DESERT_PROFILE_PHASES( planPhases );
                    DESERT_PROFILE_PHASE( planPhases, "Grass: Plan varieties (tasks + join)" );
                    Common::TaskGroup tasks( framePlan.Parallel );
                    tasks.LaunchSlices( "Grass variety: Plan", plans.size(), kMinGrassPlansPerTask,
                                        [this, &plans]( const std::size_t first, const std::size_t last ) noexcept
                                        {
                                            for ( std::size_t i = first; i < last; ++i )
                                                PlanVariety( plans[i] );
                                        } );
                    tasks.Join();
                }
                for ( VarietyPlan& plan : plans )
                    if ( !EnsureTileFeedback( *plan.Gpu ) )
                        break;
                for ( VarietyPlan& plan : plans )
                    ApplyPlanned( plan, fieldGpu, field, frame );
            }
        }
        // CPU-GRASS-POOL-8 (UE GPUScene: each pool committed and registered once a frame, after every upload was
        // gathered): every field's varieties applied, so every Allocate of the frame ran - the list, candidate
        // and law pools are committed once, the (re)built records uploaded and the list refs set, before any
        // section or pass names them.
        if ( const Common::BoolResultStr committed = CommitListPools( graph ); !committed )
        {
            LOG_ERROR( "[Grass] the varieties' rows, lists and records are not in the frame graph; no grass is "
                       "generated, culled or drawn this frame: {}",
                       committed.GetError() );
            for ( VarietyGpu* gpu : m_Frame )
                gpu->TilePoolMade = false; // a pool the skipped Init pass was to write is made anew
            m_Frame.clear();
            m_FrameSubmits.clear();
            m_FrameCull.clear();
            m_FrameTileFree.clear();
            m_FrameTileBuild.clear();
        }
        for ( const FrameSubmit& submit : m_FrameSubmits )
        {
            const GrassVarietyDraw& variety = *submit.Variety;
            Desert::Mesh* const     mesh    = submit.MeshAsset;
            VarietyGpu* const       gpu     = submit.Gpu;
            DESERT_PROFILE_PHASES( varietyPhases );
            DESERT_PROFILE_PHASE( varietyPhases, "Grass variety: Sections" );
            // RENDER-CPU-COST-2: the rows are built into scratch kept across varieties and frames (no
            // allocation once grown) and handed to the mesh renderer by move, so no buffer handle is
            // copied twice.
            thread_local std::vector<MeshRenderer::GpuInstancedRenderData> t_Sections;
            std::vector<MeshRenderer::GpuInstancedRenderData>&             sections = t_Sections;
            sections.clear();
            sections.reserve( variety.Sections.size() );
            // GRASS-VIS-MESH-1: a mesh section's rows (null: the prepass pair this frame) and the mesh its
            // visibility resolve's full-screen triangle draws (a blade strip: its first three indices are
            // 0, 1, 2). Without a resident strip no mesh section leaves the pair: the resolve would draw
            // the grass mesh's own first triangle, not the screen.
            Desert::Mesh* resolveMesh = variety.Blade ? nullptr : BladeStrip( 1u );
            if ( resolveMesh != nullptr && !resolveMesh->GetGeometry().IsResident() )
                resolveMesh = nullptr;
            gpu->BinSlots     = 0u;
            gpu->BinCastSlots = 0u;
            for ( uint32_t s = 0; s < variety.Sections.size(); ++s )
            {
                std::shared_ptr<ShaderResources::StorageBuffer> meshRows;
                if ( resolveMesh != nullptr && s < gpu->MeshVisibility.size() )
                    meshRows = gpu->MeshVisibility[s];
                sections.push_back(
                     { .Mesh     = mesh,
                       .Path     = variety.Blade ? MeshVertexPath::GrassBlade : MeshVertexPath::Instanced,
                       .Material = variety.Sections[s].Material,
                       .HiddenSubmeshes  = variety.Sections[s].HiddenSubmeshes,
                       .CastShadows      = variety.Placement.CastDynamicShadow,
                       .Wind             = variety.Wind,
                       .Instances        = m_InstancesPool.Buffer(),
                       .CameraIds        = m_IdsPool.Buffer(),
                       .BladeShape       = gpu->Shape,
                       .MeshVisibility   = meshRows,
                       .ResolveMesh      = resolveMesh,
                       .Refs             = &gpu->Refs,
                       .CameraRefs       = &gpu->CameraRefs,
                       .PostRefs         = testsHzb ? &gpu->PostRefs : nullptr,
                       .CascadeIds       = m_IdsPool.Buffer(),
                       .FullIds          = m_IdsPool.Buffer(),
                       .CascadeRefs      = gpu->CascadeMask != 0u ? &gpu->CascadeRefs : nullptr,
                       .CascadeMask      = gpu->CascadeMask,
                       .CascadeArgBytes  = gpu->CascadeArgWords * 4u,
                       .CascadeRestShift = gpu->CascadeRestShift,
                       .Capacity         = gpu->Capacity,
                       .BucketCapacities = gpu->BucketCapacities,
                       .Levels           = variety.Blade ? 1u : gpu->Buckets,
                       .PrepassBuckets   = gpu->PrepassBuckets,
                       .ShadowLod        = gpu->ShadowLod,
                       .Key              = gpu->Index * 64u + s,
                       // The grass stands still but for the wind: a cached cascade keeps its rest pose.
                       .Still = MeshRenderer::GpuInstancedRenderData::StillInstances{
                            .Generation = gpu->Generation, .Reach = gpu->Reach } } );
                // RENDER-JOB-CPU-4, GRASS-SHADOW-BINS-1
                sections.back().DrawBins = { &m_DrawBins[0], &m_DrawBins[1], &m_DrawBins[2] };
                // The section's slots of one bin list: what AddDrawGather sizes the tables by.
                const uint32_t binSlots = MeshRenderer::DrawBinSlots( sections.back() );
                gpu->BinSlots += binSlots;
                if ( sections.back().CastShadows )
                    gpu->BinCastSlots += binSlots;
            }
            // A Blade variety's levels in the view's TLAS: one shared BLAS per level, built from the shape
            // (GrassBladeRayTracing.hpp). A level a Masked section draws runs the alpha test, as MasksOf
            // rules for a mesh (RayTracingGather.cpp).
            if ( variety.Blade && m_BladeRayTracing.IsRequired() )
            {
                uint64_t nonOpaque = 0;
                for ( const MeshRenderer::GpuInstancedRenderData& section : sections )
                    if ( MeshRenderer::ShadingOf( section ).Masked )
                        nonOpaque |= ~section.HiddenSubmeshes;
                const auto blas = m_BladeRayTracing.PrepareVariety( graph, variety.Key, *mesh,
                                                                    gpu->ShapeData, nonOpaque );
                if ( blas )
                    for ( MeshRenderer::GpuInstancedRenderData& section : sections )
                        section.BladeGeometry = blas.GetValue();
                else
                    LOG_ERROR(
                         "[Grass] a blade variety is not in the view's TLAS; rays miss it this frame: {}",
                         blas.GetError() );
            }
            for ( MeshRenderer::GpuInstancedRenderData& section : sections )
                meshes.SubmitGpuInstanced( std::move( section ) );
            sections.clear();
        }
        m_FrameSubmits.clear();
        // CPU-GRASS-POOL-2: the shared tile buffers, once for every variety: the segments no pool took back this
        // frame released, the held ones imported (every cull reads them), the tile table written and imported.
        TrimTileSegments();
        if ( !m_Frame.empty() && !ImportTileSegments( graph ) )
        {
            for ( VarietyGpu* gpu : m_FrameTileFree )
                gpu->TilePoolMade = false;
            for ( VarietyGpu* gpu : m_FrameTileBuild )
                gpu->TilePoolMade = false;
            m_FrameTileFree.clear();
            m_FrameTileBuild.clear();
            m_FrameCull.clear();
        }
        // CPU-GRASS-POOL-7: every variety applied - the params, chunks, segment tables and feedback pools, once.
        if ( const Common::BoolResultStr committed = CommitVarietyPools( graph ); !committed )
        {
            LOG_ERROR( "[Grass] the varieties' params, chunks, segment tables or feedback are not in the frame "
                       "graph; no grass is generated or culled this frame: {}",
                       committed.GetError() );
            m_FrameCull.clear();
            m_FrameTileFree.clear();
            m_FrameTileBuild.clear();
            for ( VarietyGpu* gpu : m_Frame )
                gpu->TilePoolMade = false; // a pool the skipped Init pass was to write is made anew
        }
        CommitTileWords( graph ); // a failure is logged and runs no tile pass (both tile lists emptied)
        CommitTileTable( graph ); // a failure is logged and runs no tile pass (m_FrameTileBuild empty)
        // GRASS-TILE-1: the varieties that built tiles (a turning camera builds none; it only re-culls).
        DrawCounter::RecordGrassGenerated( static_cast<uint32_t>( m_FrameTileBuild.size() ) );
        if ( m_Frame.empty() )
            return;

        // A frame whose varieties all keep their inputs generates nothing: the instances, their kept counts and
        // the draw records GrassCull left stay in their persistent buffers (VarietyGpu::GenerateKey).
        // GRASS-CULL-2 (UE GPUScene / InstanceCulling: one table describes every primitive's instances, so a pass over
        // all of them binds the table and dispatches once): every pass but the generation itself records ONE binding
        // block and ONE dispatch for all its varieties, whose buffers it reads and writes by device address through
        // this frame's variety table (WriteVarietyTable). Each still declares every variety buffer it reads or writes
        // (PassBuilder::Read / Write): the graph places the barriers by them.
        DESERT_PROFILE_PHASE( grassPhases, "Grass: Variety table" );
        const auto cullRows = static_cast<uint32_t>( m_FrameCull.size() );
        if ( cullRows != 0u )
        {
            if ( const Common::BoolResultStr table = WriteVarietyTable( graph, volumes, occlusion ); !table )
            {
                LOG_ERROR( "[Grass] the variety table was not written; no grass is generated or culled this frame: {}",
                           table.GetError() );
            }
            else
            {
                m_FrameTable = true;
            }
        }

        // GRASS-TILE-1, THE TILE PASSES (GrassGenerate.shader): every variety's binding block is the same; the
        // builds write the pool, its words and the chunks (GRASS-VIEW-LAW-1).
        DESERT_PROFILE_PHASE( grassPhases, "Grass: Tile + cull passes" );
        const auto bindGenerate = [this]( RDG::PassBuilder& pass, const VarietyGpu& gpu )
        {
            const RDG::Access pool = RDG::Access::StorageWrite;
            pass.Bindings( m_GenerateLayout.Get( m_Generate->GetSpecification().Shader ),
                           Renderer::GetPipelineRouteFill( *m_Generate ) )
                 .Storage( GrassGenerateShaderParameters::Field, gpu.FieldRef, RDG::Access::StorageRead )
                 .Storage( GrassGenerateShaderParameters::Params, m_ParamsPool.Ref(), RDG::Access::StorageRead )
                 .Storage( GrassGenerateShaderParameters::TilePool, m_SegmentTablesPool.Ref(),
                           RDG::Access::StorageRead )
                 .Storage( GrassGenerateShaderParameters::TileWords, m_TileWordsPool.Ref(), pool )
                 .Storage( GrassGenerateShaderParameters::Tiles, m_TileTableRef, RDG::Access::StorageRead )
                 .Storage( GrassGenerateShaderParameters::TileFeedback, m_TileFeedbackRef, pool )
                 .Storage( GrassGenerateShaderParameters::Chunks, m_ChunksPool.Ref(), pool )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( GrassGeneratePush ) ) );
        };
        // One dispatch per variety of @p list, with the push and the groups @p shape gives it (no groups: none).
        const auto dispatchTiles = [this]( RDG::PassContext& context, const std::vector<VarietyGpu*>& list,
                                           const auto& shape ) -> Common::BoolResultStr
        {
            for ( uint32_t i = 0; i < list.size(); ++i )
            {
                const auto [push, groups] = shape( *list[i] );
                if ( groups.x == 0u || groups.y == 0u || groups.z == 0u )
                    continue;
                RDG::PassBindings bindings( context, context.GetBindingBlock( i ) );
                bindings.PushConstants( &push, sizeof( push ) );
                if ( auto dispatched =
                          Renderer::DispatchCompute( bindings, *m_Generate, groups.x, groups.y, groups.z );
                     !dispatched )
                    return dispatched;
            }
            return BOOLSUCCESS;
        };

        // THE TILE BUILDS (UE FAsyncGrassBuilder per component, grass.MaxCreatePerFrame): a new pool initialised
        // or the freed tiles' pages returned, then the built tiles' candidates counted, their pages handed out and
        // their records written. A frame that only turns the camera adds none of these passes.
        if ( !m_FrameTileFree.empty() )
        {
            const auto freeShape = []( const VarietyGpu& gpu )
            {
                if ( gpu.PoolFresh )
                {
                    const uint32_t threads = std::max( gpu.TilePages, gpu.TileSlots * kGrassTileHeaderWords );
                    return std::pair{ GrassGeneratePush{ glm::uvec4( kGrassTileModeInit, gpu.TileTableBase,
                                                                     gpu.TileSlots, gpu.TilePages ),
                                                         glm::uvec4( gpu.TileRoom, 0u, 0u, 0u ), TileBase( gpu ),
                                                         TileRanges( gpu ) },
                                      glm::uvec3( ( threads + 63u ) / 64u, 1u, 1u ) };
                }
                // GRASS-VRAM-BUDGET-1: the Free pass also pushes a grown pool's new pages on the stack.
                const uint32_t          grown = gpu.TilePages - gpu.TileGrowFrom;
                const GrassGeneratePush push{
                     glm::uvec4( kGrassTileModeFree, gpu.TileTableBase + 1u, gpu.TileFrees, gpu.TilePages ),
                     glm::uvec4( gpu.TileRoom, gpu.TileGrowFrom, grown, 0u ), TileBase( gpu ), TileRanges( gpu ) };
                const uint32_t          threads = std::max( gpu.TileFrees, grown );
                return std::pair{ push, glm::uvec3( ( threads + 63u ) / 64u, 1u, 1u ) };
            };
            graph.AddPass(
                 "Grass: Generate Free", RDG::PassFlags::Compute,
                 [this, bindGenerate]( RDG::PassBuilder& pass )
                 {
                     for ( const VarietyGpu* gpu : m_FrameTileFree )
                         bindGenerate( pass, *gpu );
                     // GRASS-VRAM-BUDGET-1: the builds write the records through the segment tables.
                     DeclareTileSegments( pass, RDG::Access::StorageWrite );
                 },
                 [this, dispatchTiles, freeShape]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return dispatchTiles( context, m_FrameTileFree, freeShape ); } );
        }
        if ( !m_FrameTileBuild.empty() )
        {
            for ( const auto& [name, mode] : { std::pair{ "Grass: Generate Count", kGrassTileModeCount },
                                               std::pair{ "Grass: Generate Allocate", kGrassTileModeAllocate },
                                               std::pair{ "Grass: Generate Write", kGrassTileModeWrite } } )
            {
                const uint32_t tileMode   = mode;
                const auto     buildShape = [tileMode]( const VarietyGpu& gpu )
                {
                    const GrassGeneratePush push{ glm::uvec4( tileMode, gpu.TileTableBase + 1u + gpu.TileFrees,
                                                              gpu.TileBuilds, gpu.TilePages ),
                                                  glm::uvec4( gpu.TileRoom, 0u, 0u, 0u ), TileBase( gpu ),
                                                  TileRanges( gpu ) };
                    if ( tileMode == kGrassTileModeAllocate )
                        return std::pair{ push, glm::uvec3( ( gpu.TileBuilds + 63u ) / 64u, 1u, 1u ) };
                    // One thread per cell: a tile's cells in thread groups, one tile per z.
                    constexpr uint32_t blocks = kGrassTileCells / kGrassGroupSize;
                    return std::pair{ push, glm::uvec3( blocks, blocks, gpu.TileBuilds ) };
                };
                graph.AddPass(
                     name, RDG::PassFlags::Compute,
                     [this, bindGenerate]( RDG::PassBuilder& pass )
                     {
                         for ( const VarietyGpu* gpu : m_FrameTileBuild )
                             bindGenerate( pass, *gpu );
                         DeclareTileSegments( pass, RDG::Access::StorageWrite );
                     },
                     [this, dispatchTiles, buildShape]( RDG::PassContext& context ) -> Common::BoolResultStr
                     { return dispatchTiles( context, m_FrameTileBuild, buildShape ); } );
            }
        }

        // THE CAMERA CULL (GRASS-VIS-24; UE: the instance cull of every frame over instances built once), one
        // dispatch over every culled variety (GrassCull.shader). GRASS-CULL-HIER-1, two levels in one pass (UE
        // InstanceCulling / Nanite: a node's bound is culled by its group of lanes, which then cull its elements):
        // the rows' groups from their chunks, then one subgroup per chunk - the cascades' boxes, the camera's
        // planes and last frame's HZB against the chunk's box, then a lane per instance of a chunk a view met into
        // the camera's list and the cascades' - then the records, phase words and phase 2's groups. Its cost
        // follows the chunks and the instances the views can take, not the field. GRASS-CULL-COST-4 (UE foliage
        // culling): the two levels are two dispatches - the chunk pass appends the chunks a view met to the met
        // chunks' list and grows its indirect dispatch, the instance pass runs one group per met chunk, a lane per
        // record.
        if ( m_FrameTable && cullRows != 0u )
        {
            RDG::BufferDesc dispatchDesc;
            dispatchDesc.Bytes = ( kCullDispatchHeader + cullRows ) * sizeof( glm::uvec4 );
            m_FrameDispatch    = graph.CreateBuffer( dispatchDesc, "GrassCullDispatch" );
            uint64_t chunkRoom = 0u;
            uint64_t workRoom  = 0u;
            for ( const VarietyGpu* gpu : m_FrameCull )
            {
                chunkRoom += gpu->ChunkRoom;
                workRoom += gpu->LawCapacity;
            }
            chunkRoom = std::min( chunkRoom, kMetRoomLimit );
            workRoom  = std::min( workRoom, uint64_t( 0xFFFFFFFFu ) );
            RDG::BufferDesc metDesc;
            metDesc.Bytes = kMetHeaderBytes + chunkRoom * kMetEntryBytes +
                            std::max( workRoom, uint64_t( 1u ) ) * kWorkItemBytes;
            m_FrameMet                = graph.CreateBuffer( metDesc, "GrassCullMetChunks" );
            const RDG::TextureRef hzb = testsHzb ? occlusion.Hzb : occlusion.Neutral;
            GrassCullPush         layOut;
            layOut.HzbViewProjection = occlusion.PrevViewProjection;
            // Hzb.z = 0: the camera cull tests the camera's planes alone.
            layOut.Hzb  = testsHzb ? glm::uvec4( occlusion.Extent, kCullModeDispatch )
                                   : glm::uvec4( 0u, 0u, 0u, kCullModeDispatch );
            // Rows.y / z: MetChunks' room for met chunks and for work items (FPS-GRASS-CULL-GPU).
            layOut.Rows =
                 glm::uvec4( cullRows, static_cast<uint32_t>( chunkRoom ), static_cast<uint32_t>( workRoom ), 0u );
            GrassCullPush cull     = layOut;
            cull.Hzb.w             = kCullModeCameraCull;
            GrassCullPush instances = layOut;
            instances.Hzb.w         = kCullModeCameraInstances;
            GrassCullPush finalize = layOut;
            finalize.Hzb.w         = kCullModeCameraFinalize;
            // GPU-HZB-SPD (UE InstanceCulling on the async pipe): the dispatch, the cull, the finalize and the
            // gathers below request async compute; the graph forks after the tile builds' last write and joins
            // before the first pass that reads the lists (CullPassFlags).
            const RDG::PassFlags cullFlags = CullPassFlags();
            graph.AddPass(
                 "Grass: Cull Dispatch", cullFlags,
                 [this, hzb]( RDG::PassBuilder& pass )
                 {
                     // The met chunks' list emptied (its count and dispatch).
                     BindCull( pass, hzb, RDG::Access::StorageWrite, RDG::Access::StorageWrite );
                     for ( const VarietyGpu* gpu : m_FrameCull )
                     {
                         // The law's slots restarted (Chunks' header).
                         pass.Write( m_ChunksPool.Ref(), RDG::Access::StorageWrite );
                         pass.Write( gpu->Refs.Args, RDG::Access::StorageWrite ); // the full list's counts reset
                     }
                 },
                 [this, layOut]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return DispatchOneGroup( context, *m_Cull, &layOut, sizeof( layOut ) ); } );

            graph.AddPass(
                 "Grass: Camera Cull", cullFlags,
                 [this, hzb]( RDG::PassBuilder& pass )
                 {
                     // The chunk pass: the law and the chunks' views; it appends the met chunks.
                     BindCull( pass, hzb, RDG::Access::StorageRead, RDG::Access::StorageWrite );
                     pass.Read( m_FrameDispatch, RDG::Access::IndirectArgs );
                     // FPS-ARCH-GRASSROWS: every variety's law rows (LawChunk), one pool.
                     pass.Write( m_InstancesPool.Ref(), RDG::Access::StorageWrite );
                     // The view law's records: every held segment, declared once (CPU-GRASS-POOL-2).
                     DeclareTileSegments( pass, RDG::Access::StorageRead );
                     for ( const VarietyGpu* gpu : m_FrameCull )
                     {
                         pass.Write( m_ChunksPool.Ref(), RDG::Access::StorageWrite ); // the law words (LawChunk)
                         pass.Read( m_SegmentTablesPool.Ref(), RDG::Access::StorageRead );
                         pass.Read( m_ParamsPool.Ref(), RDG::Access::StorageRead );
                         pass.Read( gpu->FieldRef, RDG::Access::StorageRead );
                         pass.Read( gpu->Refs.Args, RDG::Access::StorageRead ); // the law's bucket table
                     }
                 },
                 [this, cull,
                  dispatchBuffer = m_FrameDispatch]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     // GrassCull kChunksPerGroup chunks of a row per group: the groups the Cull Dispatch laid out.
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &cull, sizeof( cull ) );
                     return Renderer::DispatchComputeIndirect( bindings, *m_Cull, dispatchBuffer, 0u );
                 } );

            graph.AddPass(
                 "Grass: Camera Cull Instances", cullFlags,
                 [this, hzb]( RDG::PassBuilder& pass )
                 {
                     // The instance pass: a group per met chunk, its kept records into the lists.
                     BindCull( pass, hzb, RDG::Access::StorageRead, RDG::Access::StorageRead );
                     pass.Read( m_FrameMet, RDG::Access::IndirectArgs );
                     // FPS-ARCH-GRASSROWS: the law's rows, read by id (no pool record or params is read here).
                     pass.Read( m_InstancesPool.Ref(), RDG::Access::StorageRead );
                     pass.Write( m_CandidatesPool.Ref(), RDG::Access::StorageWrite );
                     for ( const VarietyGpu* gpu : m_FrameCull )
                     {
                         pass.Read( m_ChunksPool.Ref(), RDG::Access::StorageRead ); // the law words
                         pass.Write( gpu->Refs.Ids, RDG::Access::StorageWrite );       // the full list
                         pass.Write( gpu->Refs.Args, RDG::Access::StorageWrite );
                         pass.Write( gpu->CameraRefs.Ids, RDG::Access::StorageWrite );
                         pass.Write( gpu->CameraRefs.Args, RDG::Access::StorageWrite );
                         pass.Write( gpu->CascadeRefs.Ids, RDG::Access::StorageWrite );
                         pass.Write( gpu->CascadeRefs.Args, RDG::Access::StorageWrite );
                     }
                 },
                 [this, instances, metBuffer = m_FrameMet]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     // GrassCull mode 5, a group per met chunk: the dispatch the chunk pass grew (MetCommand).
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &instances, sizeof( instances ) );
                     return Renderer::DispatchComputeIndirect( bindings, *m_Cull, metBuffer, 0u );
                 } );

            graph.AddPass(
                 "Grass: Camera Cull Finalize", cullFlags,
                 [this, hzb]( RDG::PassBuilder& pass )
                 {
                     BindCull( pass, hzb, RDG::Access::StorageWrite, RDG::Access::StorageRead );
                     for ( const VarietyGpu* gpu : m_FrameCull )
                     {
                         pass.Write( gpu->CameraRefs.Args, RDG::Access::StorageWrite );
                         pass.Write( gpu->Refs.Args, RDG::Access::StorageWrite ); // the full list's records
                         pass.Write( m_FeedbackPool.Ref(), RDG::Access::StorageWrite );
                         pass.Write( gpu->CascadeRefs.Args, RDG::Access::StorageWrite );
                         pass.Read( m_ChunksPool.Ref(), RDG::Access::StorageRead ); // the law's slots handed out
                     }
                 },
                 [this, finalize]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return DispatchOneGroup( context, *m_Cull, &finalize, sizeof( finalize ) ); } );
            // RENDER-JOB-CPU-4: the first phase's compacted bins, gathered from the records just finalized.
            if ( const Common::BoolResultStr gathered = AddDrawGather( graph, kGpuDrawBinCameraPhase ); !gathered )
                LOG_ERROR( "[Grass] the grass draws per variety this frame: {}", gathered.GetError() );
            // GRASS-SHADOW-BINS-1: the cascades' casters, gathered from the cascade lists the same finalize wrote
            // (and the full list a list-less cascade draws).
            if ( const Common::BoolResultStr gathered = AddDrawGather( graph, kGpuDrawBinCascadesPhase );
                 !gathered )
                LOG_ERROR( "[Grass] the grass casts per variety this frame: {}", gathered.GetError() );
        }

        // The blade levels whose BLAS this frame builds: their vertices, before the view's TLAS block requests the
        // builds (SceneRenderer).
        DESERT_PROFILE_PHASE( grassPhases, "Grass: Blade BLAS pass" );
        if ( const Common::BoolResultStr blades = m_BladeRayTracing.AddPass( graph ); !blades )
            LOG_ERROR( "[Grass] the blade grass is not in the view's TLAS; rays miss it this frame: {}",
                       blades.GetError() );
    }

    Common::BoolResultStr GrassRenderer::WriteVarietyTable( RDG::Builder& graph, const ViewVolumes& volumes,
                                                            const GrassOcclusion& occlusion )
    {
        // CPU-GRASS-POOL-1/8: the candidates and law scales pools were committed once this frame, after every
        // Allocate of it (CommitListPools), before their addresses go into the rows.
        const auto split = []( const uint64_t value ) noexcept
        { return glm::uvec2( static_cast<uint32_t>( value ), static_cast<uint32_t>( value >> 32u ) ); };
        GrassVarietyTableGpu header{};
        std::copy( std::begin( volumes.Camera ), std::end( volumes.Camera ), std::begin( header.CameraPlanes ) );
        std::copy( std::begin( volumes.CascadePlanes ), std::end( volumes.CascadePlanes ),
                   std::begin( header.CascadePlanes ) );
        header.CasterWindOrigin = glm::vec4( glm::vec3( volumes.CascadeWind[0] ), 0.0f );
        std::vector<uint8_t>& image = m_TableImage;
        image.reserve( sizeof( GrassVarietyTableGpu ) + m_FrameCull.size() * sizeof( GrassVarietyRowGpu ) );
        image.resize( sizeof( GrassVarietyTableGpu ) );
        std::memcpy( image.data(), &header, sizeof( header ) );
        for ( const std::vector<VarietyGpu*>* list : { &m_FrameCull } )
        {
            for ( VarietyGpu* gpu : *list )
            {
                GrassVarietyRowGpu row{};
                // CPU-GRASS-POOL-7: the field is the one buffer of its own; params, chunks, segment table and
                // feedback are the variety's ranges of their pools (CommitVarietyPools).
                if ( gpu->FieldBuffer == nullptr )
                    return Common::MakeError( "a variety's field buffer was not made" );
                if ( const Common::ResultStr<uint64_t> address = gpu->FieldBuffer->GetDeviceAddress(); !address )
                    return Common::MakeFormattedError( "a variety's field buffer has no device address: {}",
                                                       address.GetError() );
                else
                    row.Field = split( address.GetValue() );
                if ( gpu->Params.Bytes == 0u || gpu->Chunks.Bytes == 0u || gpu->TilePool.Bytes == 0u ||
                     gpu->Feedback.Bytes == 0u )
                    return Common::MakeError( "a variety's params, chunks, table or feedback range is empty" );
                row.Params       = split( m_ParamsPool.Address( gpu->Params ) );
                row.Chunks       = split( m_ChunksPool.Address( gpu->Chunks ) );
                row.TilePool     = split( m_SegmentTablesPool.Address( gpu->TilePool ) );
                row.Feedback     = split( m_FeedbackPool.Address( gpu->Feedback ) );
                row.FeedbackMark = gpu->FeedbackMark;
                // CPU-GRASS-POOL-1: the pooled kinds, at the variety's range of their pool.
                if ( gpu->Candidates.Bytes == 0u || gpu->Instances.Bytes == 0u || gpu->Args.Bytes == 0u )
                    return Common::MakeError( "a variety's pool range was not handed out" );
                // CPU-GRASS-POOL-5: the rows, ids and records at their ranges; the bases make the ids GrassCull
                // writes and the records' firstInstance absolute in the pools (GrassVarietyRowGpu).
                row.Instances      = split( m_InstancesPool.Address( gpu->Instances ) );
                row.Args           = split( m_ArgsPool.Address( gpu->Args ) );
                row.CameraIds      = split( m_IdsPool.Address( gpu->CameraIds ) );
                row.CameraArgs     = split( m_ArgsPool.Address( gpu->CameraArgs ) );
                row.PostArgs       = split( m_ArgsPool.Address( gpu->PostArgs ) );
                row.CascadeArgs    = split( m_ArgsPool.Address( gpu->CascadeArgs ) );
                row.CascadeIds     = split( m_IdsPool.Address( gpu->CascadeIds ) );
                row.FullIds        = split( m_IdsPool.Address( gpu->FullIds ) );
                row.InstancesBase  = static_cast<uint32_t>( gpu->Instances.Offset / sizeof( glm::mat4 ) );
                row.FullIdsBase    = static_cast<uint32_t>( gpu->FullIds.Offset / kGrassIdBytes );
                row.CameraIdsBase  = static_cast<uint32_t>( gpu->CameraIds.Offset / kGrassIdBytes );
                row.CascadeIdsBase = static_cast<uint32_t>( gpu->CascadeIds.Offset / kGrassIdBytes );
                row.Candidates = split( m_CandidatesPool.Address( gpu->Candidates ) );
                uint32_t flags = 0u;
                if ( gpu->Shape )
                    flags = kRowBlade;
                row.Info   = glm::uvec4( gpu->Buckets, gpu->TableWord, flags, gpu->CascadeMask );
                // Never 0 (a chunk the build wrote carries 0), and consecutive generations differ.
                row.LawGeneration = static_cast<uint32_t>( gpu->LawGeneration % 0xFFFFFFFFull ) + 1u;
                row.LawCapacity   = gpu->LawCapacity;
                row.Bounds = gpu->CullBounds;
                row.CascadeWords = glm::uvec2( gpu->CascadeArgWords, gpu->CascadeArgWords - gpu->Buckets );
                for ( uint32_t k = 0; k < kGrassCascadeLists; k += 2u )
                    row.CascadeRings[k / 2u] = glm::vec4( gpu->CascadeRings[k], gpu->CascadeRings[k + 1u] );
                // GRASS-CULL-HIER-1, THE CASCADES' LISTS FOLLOW CHANGE (UE: a cached shadow's casters are culled when
                // the cache is redrawn, not every frame): what GrassCull's cascade cull reads - the caster boxes, the
                // wind origin, the row's buffers, counts, bounds and rings, and the instances' generation. A frame
                // a new generation (the law's inputs: the field eye, the tiles) or a new pool culls them anew; so
                // does any other change. Once the lists were culled from the same inputs they hold (kRowCascadesHeld), from the
                // next frame on - GrassCull then copies their drawn counts into that frame's Feedback copy - and the
                // caster boxes are the guard boxes (kCascadeGuardTurn): a still or turning camera culls no cascade.
                if ( list == &m_FrameCull )
                {
                    std::vector<uint32_t>& key = m_CascadeKeyScratch;
                    key.clear();
                    const auto append = [&key]( const auto& value )
                    {
                        static_assert( sizeof( std::remove_cvref_t<decltype( value )> ) % sizeof( uint32_t ) == 0u );
                        const size_t at = key.size();
                        key.resize( at + sizeof( value ) / sizeof( uint32_t ) );
                        std::memcpy( key.data() + at, &value, sizeof( value ) );
                    };
                    append( header.CascadePlanes );
                    append( header.CasterWindOrigin );
                    append( row.Instances );
                    append( row.Args );
                    append( row.CascadeArgs );
                    append( row.CascadeIds );
                    append( row.Chunks );
                    append( row.TilePool );
                    append( row.FullIds );
                    append( row.Info );
                    append( row.Bounds );
                    append( row.CascadeWords );
                    append( row.CascadeRings );
                    append( gpu->Generation );
                    // GRASS-CULL-COST-3: and the view law's generation - a frame whose law ran out of the rows' slots
                    // culled the lists without those chunks (kGrassLawNoSlot), so the grown rows (a new law
                    // generation) culls them anew instead of holding the short lists.
                    append( row.LawGeneration );
                    if ( key != gpu->CascadeKey )
                    {
                        gpu->CascadeKey.swap( key ); // the old key's storage is the next row's scratch
                        gpu->CascadesCulled = false;
                    }
                    if ( gpu->CascadesCulled )
                        row.Info.z |= kRowCascadesHeld;
                    gpu->CascadesCulled = true;
                    // GPU-GRASS-CULL-1, THE CAMERA'S LISTS FOLLOW CHANGE TOO (UE grass / instanced foliage: a
                    // cluster's cull results stand until the cluster or the view changes): a frame whose cascades
                    // hold (the instances, the law and its rows unchanged) and whose camera planes, HZB
                    // matrix and camera buffers equal the frame that last culled the camera holds that frame's
                    // phase-1 list and candidates - no camera cull runs for the row. Phase 2 still retests the
                    // held candidates against this frame's HZB, so grass an occluder uncovers is drawn; grass one
                    // covers is drawn until the view changes (depth-tested, never lost).
                    CameraCullKey camera{};
                    std::copy( std::begin( volumes.Camera ), std::end( volumes.Camera ),
                               std::begin( camera.Planes ) );
                    const bool testsHzb = occlusion.Hzb.IsValid();
                    if ( testsHzb )
                    {
                        camera.HzbViewProjection = occlusion.PrevViewProjection;
                        camera.Hzb               = glm::uvec4( occlusion.Extent, 1u );
                    }
                    camera.CameraIds     = row.CameraIds;
                    camera.CameraArgs    = row.CameraArgs;
                    camera.PostArgs      = row.PostArgs;
                    camera.Candidates    = row.Candidates;
                    camera.CameraIdsBase = row.CameraIdsBase;
                    camera.InstancesBase = row.InstancesBase;
                    camera.Builds        = gpu->Builds;
                    if ( !( camera == gpu->CameraKey ) )
                    {
                        gpu->CameraKey    = camera;
                        gpu->CameraCulled = false;
                    }
                    if ( gpu->CameraCulled && ( row.Info.z & kRowCascadesHeld ) != 0u )
                        row.Info.z |= kRowCameraHeld;
                    gpu->CameraCulled = true;
                }
                const size_t at = image.size();
                image.resize( at + sizeof( GrassVarietyRowGpu ) );
                std::memcpy( image.data() + at, &row, sizeof( row ) );
            }
        }
        const auto bytes = static_cast<uint32_t>( image.size() );
        if ( !m_Table )
            m_Table = ShaderResources::StorageBuffer::Create( "GrassVarieties", bytes, 0 );
        if ( const auto wrote = m_Table->SetData( image.data(), bytes ); !wrote.IsSuccess() )
            return wrote;
        if ( const auto imported = Renderer::ImportBuffer( m_Table, m_TableImport ); !imported )
            return imported;
        m_TableRef = graph.RegisterExternal( m_TableImport, "GrassVarieties" );
        return BOOLSUCCESS;
    }

    void GrassRenderer::BindCull( RDG::PassBuilder& pass, const RDG::TextureRef hzb, const RDG::Access dispatch,
                                  const RDG::Access met )
    {
        pass.Bindings( m_CullLayout.Get( m_Cull->GetSpecification().Shader ),
                       Renderer::GetPipelineRouteFill( *m_Cull ) )
             .Storage( GrassCullShaderParameters::Varieties, m_TableRef, RDG::Access::StorageRead )
             .Storage( GrassCullShaderParameters::CullDispatch, m_FrameDispatch, dispatch )
             .Storage( GrassCullShaderParameters::MetChunks, m_FrameMet, met )
             .Sampler( GrassCullShaderParameters::PointClampSampler, RDG::SamplerDesc::PointClamp() )
             .Texture( GrassCullShaderParameters::GrassHzbTexture, hzb, RDG::Access::SampledCompute,
                       RDG::SubresourceRange::All() )
             .PushConstantBytes( static_cast<uint32_t>( sizeof( GrassCullPush ) ) );
    }

    bool GrassRenderer::AddFramePostOcclusion( RDG::Builder& graph, const RDG::TextureRef hzb,
                                               const glm::mat4& viewProjection, const glm::uvec3& extent )
    {
        // A tested frame culls every variety's camera list (AddFrame), so m_FrameCull is the frame's whole list,
        // the variety table's first rows; the table and the dispatch buffer are this graph's (AddFrame).
        if ( !m_FrameTested || m_FrameCull.empty() || !m_FrameTable || !m_Cull )
            return false;
        // No HZB of this frame (GrassCull.shader Hzb.z = 0): nothing to re-test against, so every candidate is
        // appended - drawn once more than an HZB would let through, never dropped.
        const bool            tests = hzb.IsValid();
        const RDG::TextureRef bound = tests ? hzb : m_FrameNeutral;
        GrassCullPush         post;
        post.HzbViewProjection = viewProjection;
        post.Hzb  = tests ? glm::uvec4( extent, kCullModePostOcclusion ) : glm::uvec4( 0u, 0u, 0u, kCullModePostOcclusion );
        post.Rows = glm::uvec4( static_cast<uint32_t>( m_FrameCull.size() ), 0u, 0u, 0u );
        GrassCullPush finalize = post;
        finalize.Hzb.w         = kCullModePostFinalize;
        graph.AddPass(
             tests ? "Grass: Post-Occlusion" : "Grass: Post-Occlusion (no HZB)", RDG::PassFlags::Compute,
             [this, bound]( RDG::PassBuilder& pass )
             {
                 BindCull( pass, bound, RDG::Access::StorageRead, RDG::Access::StorageRead );
                 pass.Read( m_FrameDispatch, RDG::Access::IndirectArgs );
                 pass.Read( m_CandidatesPool.Ref(),
                            RDG::Access::StorageRead ); // every variety's (CPU-GRASS-POOL-1)
                 // Every list's ids and records are ranges of one pool each (CPU-GRASS-POOL-5): the full
                 // list's counts are read and the camera list's written in the same buffer, one access.
                 if ( !m_FrameCull.empty() )
                 {
                     pass.Write( m_IdsPool.Ref(), RDG::Access::StorageWrite );
                     pass.Write( m_ArgsPool.Ref(), RDG::Access::StorageWrite );
                 }
             },
             [this, post, dispatchBuffer = m_FrameDispatch]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 // One thread per candidate of every row: the groups the Camera Cull Finalize laid out.
                 RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                 bindings.PushConstants( &post, sizeof( post ) );
                 return Renderer::DispatchComputeIndirect( bindings, *m_Cull, dispatchBuffer, kPostDispatchOffset );
             } );

        graph.AddPass(
             "Grass: Post-Occlusion Finalize", RDG::PassFlags::Compute,
             [this, bound]( RDG::PassBuilder& pass )
             {
                 BindCull( pass, bound, RDG::Access::StorageRead, RDG::Access::StorageRead );
                 if ( !m_FrameCull.empty() )
                 {
                     pass.Write( m_ArgsPool.Ref(), RDG::Access::StorageWrite ); // camera -> post records
                     pass.Write( m_FeedbackPool.Ref(), RDG::Access::StorageWrite );
                 }
             },
             [this, finalize]( RDG::PassContext& context ) -> Common::BoolResultStr
             { return DispatchOneGroup( context, *m_Cull, &finalize, sizeof( finalize ) ); } );
        if ( const Common::BoolResultStr gathered = AddDrawGather( graph, 1u ); !gathered )
            LOG_ERROR( "[Grass] the post-occlusion grass draws per variety this frame: {}", gathered.GetError() );
        return true;
    }

    Common::BoolResultStr GrassRenderer::AddDrawGather( RDG::Builder& graph, const uint32_t phase )
    {
        GpuDrawBinFrame& table   = m_DrawBins[phase];
        DrawBinBuffers&  buffers = m_DrawBinBuffers[phase];
        table.Open               = false;
        if ( phase == 0u )
            m_DrawBins[1].Open = false; // a frame without the post-occlusion gather draws its phase per variety
        if ( m_DrawCompact == nullptr || m_FrameCull.empty() )
            return BOOLSUCCESS;
        // THE TABLE FROM WHAT THE PHASE ADMITS (UE InstanceCulling): each section's slots of one list
        // (MeshRenderer::DrawBinSlots: a mesh's drawn submeshes x its levels, a blade's strip levels; summed per
        // variety where its sections are built) x the lists of the phase - what DrawBinsOf reserves by.
        uint64_t slots = 0u;
        for ( const VarietyGpu* gpu : m_FrameCull )
        {
            if ( phase == kGpuDrawBinCascadesPhase )
                slots += uint64_t{ gpu->BinCastSlots } * MeshRenderer::kDrawBinCascadeLists;
            else
                slots += uint64_t{ gpu->BinSlots } * MeshRenderer::kDrawBinCameraLists;
        }
        const uint64_t bounded = std::clamp<uint64_t>( slots, kDrawBinMinSlots, kDrawBinMaxEntries );
        uint32_t       need    = static_cast<uint32_t>( bounded );
        if ( table.Overflow )
            need = std::min( std::max( need, table.SlotCapacity * 2u ), kDrawBinMaxEntries );
        table.Overflow = false;
        if ( need > table.SlotCapacity || buffers.Commands == nullptr )
        {
            using Buffer     = ShaderResources::StorageBuffer;
            const auto bytes = [need]( const uint64_t each ) { return static_cast<uint32_t>( need * each ); };
            const uint32_t commandBytes = bytes( kGpuDrawBinCommandBytes );
            buffers.Commands = Buffer::CreateDeviceLocal( "GrassDrawBinCmds", commandBytes, 1 );
            buffers.Slots    = Buffer::CreateDeviceLocal( "GrassDrawBinSlots", bytes( kGpuDrawBinRowBytes ), 2 );
            buffers.Counts   = Buffer::CreateDeviceLocal( "GrassDrawBinCount", bytes( kGpuDrawBinCountBytes ), 3 );
            buffers.Entries  = Buffer::Create( "GrassDrawBinEntries", bytes( sizeof( GpuDrawCompactEntry ) ), 0 );
            buffers.Rows     = Buffer::Create( "GrassDrawBinRows", bytes( sizeof( GpuInstanceDrawRow ) ), 0 );
            if ( !buffers.Commands || !buffers.Slots || !buffers.Counts || !buffers.Entries || !buffers.Rows )
            {
                buffers            = DrawBinBuffers{};
                table.SlotCapacity = 0u;
                return Common::MakeError( std::string( "GrassRenderer: the bins' buffers were not made" ) );
            }
            table.SlotCapacity  = need;
            table.EntryCapacity = std::min( need, kDrawBinMaxEntries );
            table.BinCapacity   = need;
        }
        for ( const auto& [buffer, into] : { std::pair{ &buffers.Commands, &buffers.CommandsImport },
                                             std::pair{ &buffers.Slots, &buffers.SlotsImport },
                                             std::pair{ &buffers.Counts, &buffers.CountsImport },
                                             std::pair{ &buffers.Entries, &buffers.EntriesImport } } )
            if ( const Common::BoolResultStr imported = Renderer::ImportBuffer( *buffer, *into ); !imported )
                return imported;
        table.Refs.Commands = graph.RegisterExternal( buffers.CommandsImport, "GrassDrawBinCommands" );
        table.Refs.DrawRows = graph.RegisterExternal( buffers.SlotsImport, "GrassDrawBinSlots" );
        table.Refs.Counts   = graph.RegisterExternal( buffers.CountsImport, "GrassDrawBinCounts" );
        buffers.EntriesRef  = graph.RegisterExternal( buffers.EntriesImport, "GrassDrawBinEntries" );
        table.SlotsAddress  = BufferAddress( buffers.Slots );
        table.RowsAddress   = BufferAddress( buffers.Rows ); // this frame's copy, which the clear's run uploads
        if ( table.SlotsAddress == 0u || table.RowsAddress == 0u )
            return Common::MakeError( std::string( "GrassRenderer: the bins' buffers have no address" ) );
        table.Entries.clear();
        table.Rows.clear();
        table.Slots = 0u;
        table.Bins  = 0u;
        table.BinReads.Clear(); // the chunks keep their capacity (FPS-CPU2-GRASS)
        table.Verdicts.clear();
        table.VerdictOf.Clear();
        table.Reserved = 0u;
        table.Open = true;

        // The two runs (Compact.w): the clear of every bin's count, then the gather, its own pass so the graph
        // puts the barrier between the writes of the counts. The clear's run also uploads the entries and the
        // rows: by then every draw setup of the frame has filled the table (RDG::Builder::AddPass runs a setup at once).
        const auto declare = [this, phase]( RDG::PassBuilder& pass )
        {
            const GpuDrawBinFrame& bins = m_DrawBins[phase];
            const RDG::Access      out  = RDG::Access::StorageWrite;
            pass.Bindings( m_DrawCompactLayout.Get( m_DrawCompact->GetSpecification().Shader ),
                           Renderer::GetPipelineRouteFill( *m_DrawCompact ) )
                 .Storage( GrassDrawCompactShaderParameters::CompactTable, m_DrawBinBuffers[phase].EntriesRef,
                           RDG::Access::StorageRead )
                 .Storage( GrassDrawCompactShaderParameters::CompactCommands, bins.Refs.Commands, out )
                 .Storage( GrassDrawCompactShaderParameters::CompactDrawRows, bins.Refs.DrawRows, out )
                 .Storage( GrassDrawCompactShaderParameters::CompactCounts, bins.Refs.Counts, out )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( GrassDrawCompactPush ) ) );
            // The records the entries copy, by address: every list of the phase the varieties' culls wrote.
            for ( const VarietyGpu* gpu : m_FrameCull )
            {
                if ( phase == kGpuDrawBinCameraPhase )
                {
                    pass.Read( gpu->CameraRefs.Args, RDG::Access::StorageRead );
                    pass.Read( gpu->Refs.Args, RDG::Access::StorageRead );
                }
                else if ( phase == kGpuDrawBinCascadesPhase )
                {
                    // A cascade's own list (its records and their rest twins) or the full list it draws without
                    // one.
                    pass.Read( gpu->CascadeRefs.Args, RDG::Access::StorageRead );
                    pass.Read( gpu->Refs.Args, RDG::Access::StorageRead );
                }
                else
                    pass.Read( gpu->PostRefs.Args, RDG::Access::StorageRead );
            }
        };
        // The camera's and cascades' gathers close the camera cull's async segment (CullPassFlags); the
        // post-occlusion one follows its graphics-queue cull.
        RDG::PassFlags flags = RDG::PassFlags::Compute;
        if ( phase != kGpuDrawBinPostPhase )
            flags = CullPassFlags();
        std::string clearName  = "Grass: Draw Gather Clear";
        std::string gatherName = "Grass: Draw Gather";
        if ( phase == kGpuDrawBinPostPhase )
        {
            clearName += " Post-Occlusion";
            gatherName += " Post-Occlusion";
        }
        else if ( phase == kGpuDrawBinCascadesPhase )
        {
            clearName += " Cascades";
            gatherName += " Cascades";
        }
        graph.AddPass( clearName, flags, declare,
                       [this, phase]( RDG::PassContext& context ) -> Common::BoolResultStr
                       {
                           GpuDrawBinFrame& bins    = m_DrawBins[phase];
                           DrawBinBuffers&  storage = m_DrawBinBuffers[phase];
                           bins.Open                = false; // every draw setup of the frame is done
                           if ( bins.Entries.empty() )
                               return BOOLSUCCESS;
                           const size_t entryBytes = bins.Entries.size() * sizeof( GpuDrawCompactEntry );
                           const size_t rowBytes   = bins.Rows.size() * sizeof( GpuInstanceDrawRow );
                           if ( const auto wrote = storage.Entries->SetData( bins.Entries.data(), entryBytes );
                                !wrote.IsSuccess() )
                               return wrote;
                           if ( const auto wrote = storage.Rows->SetData( bins.Rows.data(), rowBytes );
                                !wrote.IsSuccess() )
                               return wrote;
                           const GrassDrawCompactPush clear{ glm::uvec4( 0u, 0u, bins.Bins, 0u ) };
                           RDG::PassBindings          bindings( context, context.GetBindingBlock( 0 ) );
                           bindings.PushConstants( &clear, sizeof( clear ) );
                           const uint32_t groups = ( bins.Bins + 63u ) / 64u;
                           return Renderer::DispatchCompute( bindings, *m_DrawCompact, groups, 1, 1 );
                       } );
        graph.AddPass( gatherName, flags, declare,
                       [this, phase]( RDG::PassContext& context ) -> Common::BoolResultStr
                       {
                           const GpuDrawBinFrame& bins = m_DrawBins[phase];
                           if ( bins.Entries.empty() )
                               return BOOLSUCCESS;
                           uint32_t longest = 0u;
                           for ( const GpuDrawCompactEntry& entry : bins.Entries )
                               longest = std::max( longest, entry.Count );
                           const uint32_t             entries = static_cast<uint32_t>( bins.Entries.size() );
                           const GrassDrawCompactPush gather{ glm::uvec4( entries, longest, bins.Bins, 1u ) };
                           RDG::PassBindings          bindings( context, context.GetBindingBlock( 0 ) );
                           bindings.PushConstants( &gather, sizeof( gather ) );
                           const uint32_t groups = ( longest + 63u ) / 64u;
                           return Renderer::DispatchCompute( bindings, *m_DrawCompact, groups, entries, 1 );
                       } );
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::System
