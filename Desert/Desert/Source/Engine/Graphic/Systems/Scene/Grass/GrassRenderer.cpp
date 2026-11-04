// Ported from UE 5.8 Engine/Source/Runtime/Landscape/Private/LandscapeGrass.cpp:2032-2080 (FGrassBuilderBase: the
// grid size from GrassDensity) and :2378-2520 (FAsyncGrassBuilder::Build, its grid branch), adapted: the instances
// are built on the GPU once per world tile of a world-aligned grid around the view and kept while the tile is
// within the cull radius (GRASS-TILE-1, Programs/Grass/GrassGenerate.shader; UE builds them on worker threads per
// landscape component into HISM tiles), culled per view into the drawn lists, and drawn by indirect draws through
// the mesh renderer's instanced cells (GpuInstances.hpp) instead of a HISM component.
#include <Engine/Graphic/Systems/Scene/Grass/GrassRenderer.hpp>

#include <Common/Core/Profiler.hpp>
#include <Engine/Core/Device.hpp>
#include <Engine/Core/EngineContext.hpp>
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

        // A tile's key: its world tile index, x in the high half.
        uint64_t TileKey( const int32_t x, const int32_t z )
        {
            return ( static_cast<uint64_t>( static_cast<uint32_t>( x ) ) << 32u ) | static_cast<uint32_t>( z );
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
        // kFeedbackLawWord; LawScales is sized from it).
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
            // GRASS-CULL-COST-3, THE LAW'S SLOTS (GrassCull.shader LawChunk): LawScales' address, the law generation
            // (VarietyGpu::LawGeneration, never 0: a chunk the build wrote carries 0) and LawScales' slots.
            glm::uvec2 LawScales;
            uint32_t   LawGeneration = 0u;
            uint32_t   LawCapacity   = 0u;
            glm::uvec2 Pad{ 0u };
        };
        static_assert(
             sizeof( GrassVarietyRowGpu ) == 240,
             "Common/GrassVarietyRows.glslh GrassVarietyRow: 8 uvec2, uvec4 Info at 64, vec4 Bounds at 80, "
             "uvec2 CascadeIds at 96, uvec2 CascadeWords at 104, vec4 CascadeRings[4] at 112, uvec2 Chunks at 176, "
             "Params at 184, Field at 192, TilePool at 200, FullIds at 208, LawScales at 216, uint LawGeneration at "
             "224, uint LawCapacity at 228, std430's 8 bytes of padding at 232" );
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
        // GrassVarietyRows.glslh kGrassChunkHeader / kGrassChunkWords (the chunk contract): Chunks' header (x = the
        // chunks, one per tile slot and cluster, y = the law's slots handed out, z = the room, w = their law
        // generation; GrassCull writes y and w) and the words of one chunk.
        constexpr uint32_t kChunkHeaderWords = 4u;
        // GRASS-CULL-COST-3: words 10..25 are the camera cull's law words (the generation, the kept box, the codes,
        // the first slot of LawScales).
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
            // GrassBladeLodAt, GrassMeshFarLod).
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
        // The bins' table holds every blade level of the frame's varieties this many times over: the lists of a
        // phase (the deferred early-Z pair and its G-buffer halves, the visibility raster, the forward pair) x the
        // sections of a variety. A frame that passes it grows the table twice as large (GpuDrawBinFrame::Overflow).
        inline constexpr uint32_t kDrawBinSlotsPerLevel = 32u;
        inline constexpr uint32_t kDrawBinMinSlots      = 1024u;
        inline constexpr uint32_t kDrawBinMaxEntries    = 65535u; // the gather's dispatch y (a group row per entry)

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
        };
        static_assert( sizeof( GrassGeneratePush ) == 32 );

        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).

        // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
        struct GrassCullShaderParameters
        {
            DESERT_SHADER_PARAMETER( StorageBuffer, Varieties );
            DESERT_SHADER_PARAMETER( StorageBuffer, CullDispatch );
            DESERT_SHADER_PARAMETER( Sampler, PointClampSampler );
            DESERT_SHADER_PARAMETER( Texture, GrassHzbTexture );
        };

        // GrassCull's push block, std430 (Programs/Grass/GrassCull.shader PushConstants).
        struct GrassCullPush
        {
            glm::mat4  HzbViewProjection{ 1.0f }; // unjittered, what the bound HZB was built through
            glm::uvec4 Hzb{ 0u };  // xy = the HZB's mip-0 size, z = its mip count (0: nothing tested), w = kCullMode*
            glm::uvec4 Rows{ 0u }; // x = the variety table's rows the pass covers (its first)
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
        strip = std::make_shared<StaticMesh>( vertices, indices, submeshes, std::vector<MeshVertexStreams>{} );
        if ( const auto uploaded = strip->Invalidate(); !uploaded.IsSuccess() )
        {
            LOG_ERROR( "[Grass] the {}-segment blade strip was not uploaded: {}", segments, uploaded.GetError() );
            strip.reset();
            return nullptr;
        }
        return strip.get();
    }

    GrassRenderer::VarietyGpu* GrassRenderer::PrepareVariety( const GrassVarietyDraw& variety, Desert::Mesh& mesh,
                                                              const ViewFrame& frame, const ViewVolumes& volumes,
                                                              const MeshRenderer& meshes,
                                                              const uint64_t      fieldRevision )
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
        VarietyGpu&  gpu    = m_Varieties[variety.Key];
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
            for ( uint32_t b = 1; perspective && b < buckets; ++b )
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
        // What the GPU counted for each bucket FramesInFlight frames ago (GrassCull FinalizeFull), when this frame's copy
        // has been written since the feedback buffer was made for this bucket count.
        std::vector<uint32_t> counted( buckets + kFeedbackTailWords, 0u );
        bool                  fedBack = false;
        gpu.Drawn                     = std::nullopt;
        gpu.DrawnCascades             = {};
        if ( gpu.Feedback && gpu.Buckets == buckets )
        {
            if ( const auto read = gpu.Feedback->ReadBack( counted.data(), ( buckets + kFeedbackTailWords ) * 4u );
                 !read.IsSuccess() )
            {
                LOG_ERROR( "[Grass] the generated counts were not read back: {}", read.GetError() );
            }
            else
            {
                fedBack = counted[buckets] != 0u;
                if ( fedBack )
                {
                    gpu.Drawn = glm::uvec3( counted[buckets + 1u], counted[buckets + 2u], counted[buckets + 3u] );
                    for ( uint32_t v = 0; v < kGrassCascadeLists; ++v )
                        gpu.DrawnCascades[v] = counted[buckets + 4u + v];
                }
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
        constexpr uint32_t    kShrinkFrames = 60;
        std::vector<uint32_t> capacities( buckets );
        uint32_t              capacity = 0;
        // The first bucket whose read-back count exceeds the capacity it was drawn with (GrassGenerate dropped the
        // rest): the frames since drew a truncated field (InstancesSettling).
        std::optional<uint32_t> overflowed;
        for ( uint32_t b = 0; b < buckets; ++b )
        {
            // A blade's level is a ring of distance; a mesh level's screen size reaches anywhere in the disc (a
            // large instance keeps its detail farther than a small one), so it is bounded by the whole disc.
            const bool ring  = variety.Blade && buckets > 1u;
            double     inner = ring ? static_cast<double>( b ) * lodDistance : 0.0;
            // The coarsest level also holds the far LOD's instances (GrassBladeLodAt), from where the smallest
            // blade's radius stands at its screen size.
            if ( ring && b + 1u == buckets && farLodDistance > 0.0f )
            {
                const float minScale = std::min( { p.ScaleX.x, p.ScaleY.x, p.ScaleZ.x } );
                inner = std::min( inner, static_cast<double>( farLodDistance ) * std::max( minScale, 0.0f ) );
            }
            const double outer = !ring || b + 1u == buckets ? static_cast<double>( radius )
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
        // row at half. A new LawScales is a new law generation (LawKey: its address), so every chunk is lawed into it;
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
            if ( !gpu.LawScales || lawNext != gpu.LawCapacity )
            {
                const uint64_t lawBytes = static_cast<uint64_t>( lawNext ) * 3u * sizeof( float );
                gpu.LawScales.reset();
                gpu.LawCapacity = 0u;
                if ( lawBytes <= UINT32_MAX )
                    gpu.LawScales = ShaderResources::StorageBuffer::CreateDeviceLocal(
                         "GrassLawScales", static_cast<uint32_t>( lawBytes ), 2 );
                if ( !gpu.LawScales )
                {
                    LOG_ERROR( "[Grass] variety {:016x}: the view law's slots ({} bytes, at most a storage buffer's "
                               "4 GB) were not made; they are made anew next frame",
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
                          "LawScales' room; the field drew without the rest until it grows",
                          variety.Key, variety.Layer, lawUsed );
            gpu.Truncated   = true;
            gpu.SettleReads = settleReads;
        }
        else if ( gpu.SettleReads > 0u && --gpu.SettleReads == 0u )
            gpu.Truncated = false;
        m_Settling = m_Settling || gpu.SettleReads > 0u;

        if ( gpu.Mesh != &mesh || gpu.Capacity != capacity || gpu.Buckets != buckets ||
             gpu.BucketCapacities != capacities || !gpu.Args || gpu.List != volumes.List ||
             gpu.CascadeMask != volumes.CascadeMask )
        {
            gpu.List             = volumes.List;
            gpu.Mesh             = &mesh;
            gpu.Capacity         = capacity;
            gpu.Buckets          = buckets;
            gpu.BucketCapacities = capacities;
            if ( gpu.Index == 0 )
                gpu.Index = ++m_NextIndex;
            // GRASS-VIEW-LAW-1: the id lists below are written and read by the GPU alone, so they live in
            // device-local memory (StorageBuffer::CreateDeviceLocal), not in the host-visible device heap, and so
            // do the rows they name (Instances, REGRESS-CACHE-1: three regions of the lists' capacity, below).
            const auto submeshCount = static_cast<uint32_t>( submeshes.size() );
            const auto tableWord    = static_cast<uint32_t>( GpuInstanceArgsOffset( records ) / 4u );
            const auto argBytes     = ( tableWord + buckets * kBucketWords ) * 4u;
            gpu.Args                = ShaderResources::StorageBuffer::Create( "GrassArgs", argBytes, 3, true );
            // The records' static half: record L x submeshCount + si draws submesh si at level L (a submesh with a
            // shorter chain its coarsest level, one without a chain its base range); a blade's record k x
            // submeshCount + s draws strip level max(s, k) for bucket s (set k, the shadow LOD). instanceCount and
            // firstInstance are GrassCull's (FinalizeFull, FinalizeCamera). The bucket table: each bucket's first instance, capacity and - a
            // mesh level - half its screen size.
            std::vector<uint32_t> args( argBytes / 4u, 0u );
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
            if ( const auto wrote = gpu.Args->SetData( args.data(), argBytes ); !wrote.IsSuccess() )
            {
                LOG_ERROR( "[Grass] the argument records were not written: {}", wrote.GetError() );
                return nullptr;
            }
            // The camera's list (GRASS-VIS): the same records and bucket table, its own counts; CameraArgs adds
            // the phase words, and only an HZB-tested list has candidates and phase 2's records (PlanesAndHzb).
            // What this frame's list does not use is a one-word stand-in (the passes bind it, Hzb.w / Mode.y keep
            // it idle).
            {
                gpu.CameraIds = ShaderResources::StorageBuffer::CreateDeviceLocal(
                     "GrassCameraIds", std::max( capacity, 1u ) * kGrassIdBytes, 5 );
                // The full list (GrassCull.shader CullInstance): the records the views keep, in Args' buckets.
                gpu.FullIds = ShaderResources::StorageBuffer::CreateDeviceLocal(
                     "GrassFullIds", std::max( capacity, 1u ) * kGrassIdBytes, 9 );
                // REGRESS-CACHE-1, THE ROWS (UE: the culled instances' compacted buffer, sized by what the views
                // keep, never by every built instance): the view law's mat4 of each KEPT instance, in three
                // regions of the lists' layout - the full list's (row = its slot in FullIds), the camera's own and
                // its HZB candidates' (for an instance the full list holds no slot for this frame: its cascades
                // held, or its bucket full). Every list holds rows, not pool records (GrassCull.shader
                // CullInstance). One row per pool record had spent 64 B on every resident record (~1.7 GB on
                // FIELD_Barn, past the VRAM budget).
                constexpr uint32_t kGrassRowRegions = 3u; // the full list's, the camera's own, its candidates'
                const uint64_t     rowBytes =
                     static_cast<uint64_t>( kGrassRowRegions ) * std::max( capacity, 1u ) * sizeof( glm::mat4 );
                gpu.Instances.reset();
                if ( rowBytes <= UINT32_MAX )
                    gpu.Instances = ShaderResources::StorageBuffer::CreateDeviceLocal(
                         "GrassInstances", static_cast<uint32_t>( rowBytes ), 2 );
                if ( !gpu.CameraIds || !gpu.FullIds || !gpu.Instances )
                {
                    // No Args: this block runs again next frame, as a pool that was not made is (UpdateTiles).
                    LOG_ERROR( "[Grass] variety {:016x}: its id lists or rows ({} bytes, at most a storage buffer's "
                               "4 GB) were not made; they are made anew next frame",
                               variety.Key, rowBytes );
                    gpu.Args.reset();
                    return nullptr;
                }
                std::vector<uint32_t> cameraArgs = args;
                cameraArgs.resize( args.size() + static_cast<size_t>( buckets ) * kPhaseWords, 0u );
                const auto cameraBytes = static_cast<uint32_t>( cameraArgs.size() * 4u );
                gpu.CameraArgs = ShaderResources::StorageBuffer::Create( "GrassCameraArgs", cameraBytes, 6, true );
                if ( const auto wrote = gpu.CameraArgs->SetData( cameraArgs.data(), cameraBytes ); !wrote.IsSuccess() )
                {
                    LOG_ERROR( "[Grass] the camera list's records were not written: {}", wrote.GetError() );
                    return nullptr;
                }
            }
            if ( volumes.List == CameraList::PlanesAndHzb )
            {
                gpu.Candidates = ShaderResources::StorageBuffer::CreateDeviceLocal(
                     "GrassCandidates", std::max( capacity, 1u ) * kCandidateBytes, 7 );
                gpu.PostArgs = ShaderResources::StorageBuffer::Create( "GrassPostArgs", argBytes, 3, true );
                if ( const auto wrote = gpu.PostArgs->SetData( args.data(), argBytes ); !wrote.IsSuccess() )
                {
                    LOG_ERROR( "[Grass] the camera list's phase-2 records were not written: {}", wrote.GetError() );
                    return nullptr;
                }
            }
            else
            {
                gpu.Candidates = ShaderResources::StorageBuffer::CreateDeviceLocal( "GrassCandidates", 16u, 7 );
                gpu.PostArgs   = ShaderResources::StorageBuffer::Create( "GrassPostArgs", 16u, 3, true );
            }
            // The cascades' lists (GPU-COST-21): per cascade view this frame draws a copy of the records and the
            // bucket table, list k's bucket firsts moved by k x capacity into CascadeIds; then its rest twin
            // (GRASS-SHADOW-1): the records once more, for the rest-pose casters at each bucket's end, and one
            // rest count per bucket (GrassGenerate.shader appends, GrassCull.shader FinalizeCascades).
            const auto recordsWord = static_cast<uint32_t>( GpuInstanceArgsOffset( 0u ) / 4u );
            gpu.CascadeMask        = volumes.CascadeMask;
            gpu.CascadeArgWords    = static_cast<uint32_t>( args.size() ) + ( tableWord - recordsWord ) + buckets;
            gpu.CascadeRestShift   = argBytes - recordsWord * 4u;
            if ( const auto lists = static_cast<uint32_t>( std::popcount( volumes.CascadeMask ) ); lists > 0u )
            {
                gpu.CascadeIds = ShaderResources::StorageBuffer::CreateDeviceLocal(
                     "GrassCascadeIds", std::max( lists * capacity, 1u ) * kGrassIdBytes, 8 );
                std::vector<uint32_t> cascadeArgs;
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
                const auto cascadeBytes = static_cast<uint32_t>( cascadeArgs.size() * 4u );
                gpu.CascadeArgs =
                     ShaderResources::StorageBuffer::Create( "GrassCascadeArgs", cascadeBytes, 9, true );
                if ( const auto wrote = gpu.CascadeArgs->SetData( cascadeArgs.data(), cascadeBytes );
                     !wrote.IsSuccess() )
                {
                    LOG_ERROR( "[Grass] the cascade lists' records were not written: {}", wrote.GetError() );
                    return nullptr;
                }
            }
            else
            {
                gpu.CascadeIds  = ShaderResources::StorageBuffer::CreateDeviceLocal( "GrassCascadeIds", 16u, 8 );
                gpu.CascadeArgs = ShaderResources::StorageBuffer::Create( "GrassCascadeArgs", 16u, 9, true );
            }
            gpu.Params = ShaderResources::StorageBuffer::Create( "GrassParams", sizeof( GrassParamsGpu ), 1 );
            if ( const uint32_t feedbackBytes = ( buckets + kFeedbackTailWords ) * 4u;
                 !gpu.Feedback || gpu.Feedback->GetSize() != feedbackBytes )
                gpu.Feedback = ShaderResources::StorageBuffer::Create( "GrassFeedback", feedbackBytes, 1 );
            gpu.Shape.reset();
            if ( variety.Blade )
                gpu.Shape =
                     ShaderResources::StorageBuffer::Create( "GrassBladeShape", sizeof( GrassBladeShapeGpu ), 13 );
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
            for ( uint32_t bucket = 0, first = 0; bucket < std::min( buckets, kGrassBladeVisibilityMaxBuckets );
                  first += capacities[bucket], ++bucket )
                gpu.ShapeData.BucketFirst[bucket / 4u][bucket % 4u] = first;
            if ( const auto wrote = gpu.Shape->SetData( &gpu.ShapeData, sizeof( gpu.ShapeData ) );
                 !wrote.IsSuccess() )
            {
                LOG_ERROR( "[Grass] the blade shape was not written: {}", wrote.GetError() );
                return nullptr;
            }
        }

        // GRASS-VIS-MESH-1: a mesh variety's sections each take their rows of this frame's raster stream
        // (GrassMeshVisibilityGpu, Mesh/Surface/GrassMeshVisibility.glslh; UE 5 Nanite's visible clusters): slot i
        // owns RowsPerInstance rows, a level's submesh one row per 128 of its triangles. A section whose table
        // cannot be stated (rows past kGrassVisibilityMaxRows, more submeshes or ranges than the table holds, a
        // mesh without device addresses) has none and stays in the masked prepass and the EQUAL G-buffer
        // (MeshRenderer admits only a tabled section), said once.
        if ( !variety.Blade )
        {
            gpu.MeshVisibility.resize( variety.Sections.size() );
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
                std::shared_ptr<ShaderResources::StorageBuffer>& table  = gpu.MeshVisibility[s];
                const uint64_t                                   hidden = variety.Sections[s].HiddenSubmeshes;
                GrassMeshVisibilityGpu                           rows{};
                uint32_t                                         perInstance = 0;
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
                    table.reset();
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
                rows.Slots = glm::uvec4( capacity, 0u, 0u, 0u );
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
                if ( !table )
                    table = ShaderResources::StorageBuffer::Create( "GrassMeshVisibility",
                                                                    sizeof( GrassMeshVisibilityGpu ), 13 );
                if ( const auto wrote = table->SetData( &rows, sizeof( rows ) ); !wrote.IsSuccess() )
                {
                    LOG_ERROR( "[Grass] a mesh section's visibility rows were not written: {}", wrote.GetError() );
                    table.reset();
                    continue;
                }
                m_VisibilityRows += need;
            }
        }

        GrassParamsGpu params{};
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
        params.Lean.z = std::max( m_SceneRenderer->GetWorldParameterScalar( "SnowDepth" ), 0.0f );
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
            params.Lod = glm::vec4( 0.0f, 0.0f, farLodDistance, 0.0f );
        if ( const auto wrote = gpu.Params->SetData( &params, sizeof( params ) ); !wrote.IsSuccess() )
        {
            LOG_ERROR( "[Grass] the variety's parameters were not written: {}", wrote.GetError() );
            return nullptr;
        }

        // GRASS-TILE-1, THE VARIETY'S TILES AROUND THE EYE (UpdateTiles). The build reads the placement's params
        // alone - the grid and its jitter, the layer and its seed, the density range and the scales, a blade's
        // clump, scatter, patchiness and lean, the patches - and the field (its revision); the share its tiles
        // keep reads the thinning's distances. The view's planes, position and cascades are the tile cull's.
        // RENDER-JOB-CPU-2: its 40 words (nine vec4, the layer, the seed, the field's revision) in one allocation,
        // not the ten regrowths of 40 push_backs, per variety per frame.
        constexpr std::size_t kPlacementWords = 9u * 4u + 4u;
        std::vector<uint32_t> placement;
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
        std::vector<uint32_t> thin{ std::bit_cast<uint32_t>( params.Cull.x ),
                                    std::bit_cast<uint32_t>( params.Cull.y ),
                                    std::bit_cast<uint32_t>( params.Spread.w ) };
        // How far an instance can lie outside its cell: its jitter (Cull.w cells), a blade's scatter and clump
        // pull.
        const auto stray = static_cast<float>( static_cast<double>( params.Cull.w * cell ) + moved );
        if ( !UpdateTiles( gpu, std::move( placement ), std::move( thin ), glm::vec2( eye.x, eye.z ), cell, radius,
                           stray ) )
            return nullptr;

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
        for ( const void* buffer :
              { static_cast<const void*>( gpu.Instances.get() ), static_cast<const void*>( gpu.Args.get() ) } )
        {
            const auto address = reinterpret_cast<std::uintptr_t>( buffer ); // NOLINT: identity, not access
            gpu.FrameKey.push_back( static_cast<uint32_t>( address ) );
            gpu.FrameKey.push_back( static_cast<uint32_t>( static_cast<uint64_t>( address ) >> 32u ) );
        }
        // GRASS-CULL-COST-3: what the view law reads (VarietyGpu::LawKey) - the params (the field eye among them), the
        // pool's identity, Args (a mesh level's half screen size) and LawScales; AddFrame adds the field's revision. A
        // tile built or freed is no new law generation: the build writes its chunks unlawed (law word 0).
        gpu.LawKey.assign( gpu.FrameKey.begin(),
                           gpu.FrameKey.begin() +
                                static_cast<std::ptrdiff_t>( sizeof( GrassParamsGpu ) / sizeof( uint32_t ) ) );
        gpu.LawKey.push_back( gpu.PoolGeneration );
        // LawScales' room too: a remade LawScales may land on the freed one's address, and every remake changes it.
        gpu.LawKey.push_back( gpu.LawCapacity );
        for ( const void* buffer :
              { static_cast<const void*>( gpu.Args.get() ), static_cast<const void*>( gpu.LawScales.get() ) } )
        {
            const auto address = reinterpret_cast<std::uintptr_t>( buffer ); // NOLINT: identity, not access
            gpu.LawKey.push_back( static_cast<uint32_t>( address ) );
            gpu.LawKey.push_back( static_cast<uint32_t>( static_cast<uint64_t>( address ) >> 32u ) );
        }
        return &gpu;
    }

    uint64_t GrassRenderer::TilePoolBytes( const VarietyGpu* const except ) const
    {
        uint64_t bytes = 0u;
        for ( const auto& entry : m_Varieties )
            if ( &entry.second != except )
                bytes += static_cast<uint64_t>( entry.second.TileSegments.size() ) * kGrassTileSegmentBytes;
        return bytes;
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
        bool ranOut = false;
        if ( gpu.TileFeedback )
        {
            std::array<uint32_t, kGrassTileFeedbackWords> read{};
            if ( const auto got = gpu.TileFeedback->ReadBack( read.data(), sizeof( read ) ); !got.IsSuccess() )
            {
                LOG_ERROR( "[Grass] the tile pool's feedback was not read back: {}", got.GetError() );
            }
            else if ( read != std::array<uint32_t, kGrassTileFeedbackWords>{} )
            {
                ranOut = read[0] == gpu.PoolGeneration && read[1] != 0u;
                const std::array<uint32_t, kGrassTileFeedbackWords> cleared{};
                if ( const auto wrote = gpu.TileFeedback->SetData( cleared.data(), sizeof( cleared ) );
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
            gpu.Tiles.clear();
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
            // GRASS-TILE-3 / GRASS-VIEW-LAW-1: the segments and the pool's words live in video memory, device-local
            // storage buffers the GPU alone writes (StorageBuffer::CreateDeviceLocal): kept across graphs and
            // frames, read by address where a pass covers many varieties. The old ones are released once no frame
            // in flight holds them.
            gpu.TileSegments.clear();
            gpu.TilePool.reset();
            gpu.TileWords.reset();
            gpu.Chunks.reset();
            const uint64_t wordBytes =
                 ( 1u + static_cast<uint64_t>( room ) + static_cast<uint64_t>( slots ) * kGrassTileHeaderWords ) * 4u;
            if ( wordBytes > UINT32_MAX )
            {
                LOG_ERROR( "[Grass] a variety's tile pool words ({} bytes) exceed a storage buffer's 4 GB; no tile is "
                           "built",
                           wordBytes );
                return false;
            }
            gpu.TilePool  = ShaderResources::StorageBuffer::Create(
                 "GrassTileSegments",
                 static_cast<uint32_t>( room / kGrassTileSegmentPages * sizeof( uint64_t ) ), 14, true );
            gpu.TileWords = ShaderResources::StorageBuffer::CreateDeviceLocal(
                 "GrassTileWords", static_cast<uint32_t>( wordBytes ), 15 );
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
            std::vector<uint32_t> chunks( kChunkHeaderWords + static_cast<size_t>( chunkCount ) * kChunkWords, 0u );
            gpu.ChunkRoom         = static_cast<uint32_t>( chunkCount );
            chunks[0]             = gpu.ChunkRoom;
            chunks[2]             = gpu.ChunkRoom;
            const auto chunkBytes = static_cast<uint32_t>( chunks.size() * 4u );
            gpu.Chunks            = ShaderResources::StorageBuffer::Create( "GrassChunks", chunkBytes, 8, true );
            if ( gpu.Chunks )
            {
                if ( const auto wrote = gpu.Chunks->SetData( chunks.data(), chunkBytes ); !wrote.IsSuccess() )
                {
                    LOG_ERROR( "[Grass] the variety's chunks were not written: {}", wrote.GetError() );
                    gpu.Chunks.reset();
                }
            }
            // The table holds at most every held tile freed or culled, and a frame's builds freed and built.
            gpu.TileTable = ShaderResources::StorageBuffer::Create(
                 "GrassTileTable", ( 1u + 2u * ( slots + kGrassTileBuildsPerFrame ) ) * sizeof( glm::uvec4 ), 12 );
            if ( !gpu.TileFeedback )
                gpu.TileFeedback = ShaderResources::StorageBuffer::Create( "GrassTileFeedback",
                                                                           kGrassTileFeedbackWords * 4u, 13 );
        }
        if ( !gpu.TileTable || !gpu.TileFeedback || !gpu.TilePool || !gpu.TileWords || !gpu.Chunks )
        {
            LOG_ERROR( "[Grass] a variety's tile table, feedback, pool, pool words or chunks were not made; its "
                       "pool is made anew next frame" );
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
            std::shared_ptr<ShaderResources::StorageBuffer> segment = ShaderResources::StorageBuffer::CreateDeviceLocal(
                 "GrassTilePool", static_cast<uint32_t>( kGrassTileSegmentBytes ), 14 );
            if ( !segment )
            {
                LOG_ERROR( "[Grass] a variety's tile pool segment was not made; its pool is made anew next frame" );
                gpu.TilePoolMade    = false;
                gpu.TileWalkSettled = false;
                return false;
            }
            const Common::ResultStr<uint64_t> address = segment->GetDeviceAddress();
            if ( !address )
            {
                LOG_ERROR( "[Grass] a tile pool segment has no device address; its pool is made anew next frame: {}",
                           address.GetError() );
                gpu.TilePoolMade    = false;
                gpu.TileWalkSettled = false;
                return false;
            }
            const uint64_t value = address.GetValue();
            if ( const auto wrote = gpu.TilePool->SetData( &value, sizeof( value ),
                                                           static_cast<uint32_t>( s * sizeof( uint64_t ) ) );
                 !wrote.IsSuccess() )
            {
                LOG_ERROR( "[Grass] the tile pool's segment table was not written; its pool is made anew next frame: "
                           "{}",
                           wrote.GetError() );
                gpu.TilePoolMade    = false;
                gpu.TileWalkSettled = false;
                return false;
            }
            gpu.TileSegments.push_back( std::move( segment ) );
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
            for ( auto& [key, tile] : gpu.Tiles )
                tile.Stale = true;
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
        gpu.TileEntries.assign( 1u, glm::uvec4( gpu.PoolGeneration, 0u, 0u, 0u ) );
        for ( auto it = gpu.Tiles.begin(); it != gpu.Tiles.end(); )
        {
            const auto x = static_cast<int32_t>( static_cast<uint32_t>( it->first >> 32u ) );
            const auto z = static_cast<int32_t>( static_cast<uint32_t>( it->first ) );
            if ( nearestOf( x, z ) <= radius + side )
            {
                ++it;
                continue;
            }
            gpu.TileEntries.push_back( glm::uvec4( it->second.Slot, 0u, 0u, 0u ) );
            gpu.FreeSlots.push_back( it->second.Slot );
            it = gpu.Tiles.erase( it );
        }

        // THE BUILDS (UE grass.MaxCreatePerFrame, nearest first): a tile within the radius that is missing, stale,
        // nearer than it was built for (its far draws were not kept), or past twice what it was built for (it
        // holds several times the instances it needs). A tile is built for builtFor( its nearest ).
        struct Wanted
        {
            float   Nearest = 0.0f;
            int32_t X = 0, Z = 0;
        };
        std::vector<Wanted> wanted;
        for ( int32_t z = atZ - reach; z <= atZ + reach; ++z )
            for ( int32_t x = atX - reach; x <= atX + reach; ++x )
            {
                const float nearest = nearestOf( x, z );
                if ( nearest > radius )
                    continue;
                const auto found = gpu.Tiles.find( TileKey( x, z ) );
                if ( found == gpu.Tiles.end() || found->second.Stale || nearest < found->second.BuiltNear ||
                     nearest > 2.0f * ( found->second.BuiltNear + side ) )
                    wanted.push_back( { nearest, x, z } );
            }
        std::sort( wanted.begin(), wanted.end(),
                   []( const Wanted& a, const Wanted& b ) { return a.Nearest < b.Nearest; } );
        // Tiles that draw short of what the view keeps (missing, stale, approached) until built:
        // InstancesSettling.
        bool                    pending = false;
        std::vector<glm::uvec4> builds;
        for ( const Wanted& want : wanted )
        {
            auto       found = gpu.Tiles.find( TileKey( want.X, want.Z ) );
            const bool drawsShort =
                 found == gpu.Tiles.end() || found->second.Stale || want.Nearest < found->second.BuiltNear;
            if ( builds.size() == kGrassTileBuildsPerFrame ||
                 ( found == gpu.Tiles.end() && gpu.FreeSlots.empty() ) )
            {
                pending = pending || drawsShort;
                continue;
            }
            if ( found == gpu.Tiles.end() )
            {
                found =
                     gpu.Tiles
                          .emplace( TileKey( want.X, want.Z ), VarietyGpu::ResidentTile{ gpu.FreeSlots.back() } )
                          .first;
                gpu.FreeSlots.pop_back();
            }
            else
                gpu.TileEntries.push_back( glm::uvec4( found->second.Slot, 0u, 0u, 0u ) );
            found->second.BuiltNear = builtFor( want.Nearest );
            found->second.Stale     = false;
            builds.push_back( glm::uvec4( found->second.Slot, static_cast<uint32_t>( want.X ),
                                          static_cast<uint32_t>( want.Z ),
                                          std::bit_cast<uint32_t>( found->second.BuiltNear ) ) );
        }
        gpu.TileFrees  = static_cast<uint32_t>( gpu.TileEntries.size() - 1u );
        gpu.TileBuilds = static_cast<uint32_t>( builds.size() );
        gpu.TileEntries.insert( gpu.TileEntries.end(), builds.begin(), builds.end() );
        // The tiles culled, nearest first (GRASS-TILE-2): the cull's groups run in about that order, so the full
        // list - and the camera's, which keeps its order - holds the field front to back, and the depth prepass
        // and the GBuffer reject the far blades behind the near ones early. GRASS-TILE-4: not the band past the
        // radius the residents keep - none of a tile there lies within it (an instance's distance from the eye is
        // at least nearestOf), and the cull runs a group per cluster of every tile it is given.
        std::vector<std::pair<float, uint32_t>> culled;
        culled.reserve( gpu.Tiles.size() );
        for ( const auto& [key, tile] : gpu.Tiles )
        {
            const float nearest = nearestOf( static_cast<int32_t>( static_cast<uint32_t>( key >> 32u ) ),
                                             static_cast<int32_t>( static_cast<uint32_t>( key ) ) );
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

    void GrassRenderer::AddFrame( RDG::Builder& graph, const ViewFrame& frame, MeshRenderer& meshes,
                                  const bool rayTracing, const GrassOcclusion& occlusion )
    {
        // TURN-CPU-SETUP-1: the frame's grass by stage, one profile row each (inside SceneRenderer's "Setup:
        // Grass").
        DESERT_PROFILE_PHASES( grassPhases );
        DESERT_PROFILE_PHASE( grassPhases, "Grass: View volumes" );
        const bool testsHzb = occlusion.Hzb.IsValid();
        // The two volumes (GRASS-VIS-6): the camera's side planes, and the same planes swept along every light
        // whose cascades this frame fitted (UpdateCascades ran before this) for the full list the cascades and
        // the TLAS draw. While they differ the camera keeps a list of its own even with no HZB to test.
        ViewVolumes volumes;
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
        m_FrameCull.clear();
        m_FrameTileFree.clear();
        m_FrameTileBuild.clear();
        m_FrameTilePoolCap.reset();
        m_Settling       = false;
        m_FrameTested    = testsHzb;
        m_FrameNeutral   = occlusion.Neutral;
        m_FrameTable     = false;
        m_VisibilityRows = 0;
        m_BladeRayTracing.BeginFrame( rayTracing );
        if ( m_Queue.empty() || !m_Generate || !m_Cull )
            return;

        DESERT_PROFILE_PHASE( grassPhases, "Grass: Prepare varieties" );
        for ( const GrassFieldDraw& field : m_Queue )
        {
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

            for ( const GrassVarietyDraw& variety : field.Varieties )
            {
                Desert::Mesh* mesh = variety.Blade ? BladeStrip( variety.Blade->Segments ) : variety.Mesh;
                if ( mesh == nullptr || variety.Sections.empty() || !mesh->GetGeometry().IsResident() ||
                     mesh->GetGeometry().IndexCount == 0 )
                    continue;
                // RENDER-CPU-COST-1: a row of its own per step of a variety (the phase had none: 0.70 ms self on
                // the turn, scn-1104-073309); each row sums every variety's step.
                DESERT_PROFILE_PHASES( varietyPhases );
                DESERT_PROFILE_PHASE( varietyPhases, "Grass variety: Prepare" );
                VarietyGpu* gpu = PrepareVariety( variety, *mesh, frame, volumes, meshes,
                                                  static_cast<uint64_t>( fieldGpu.Revision ) );
                if ( gpu == nullptr )
                    continue;
                DESERT_PROFILE_PHASE( varietyPhases, "Grass variety: Import + register buffers" );
                Common::BoolResultStr imported = Renderer::ImportBuffer( gpu->Instances, gpu->InstancesImport );
                if ( imported )
                    imported = Renderer::ImportBuffer( gpu->Args, gpu->ArgsImport );
                if ( imported )
                    imported = Renderer::ImportBuffer( gpu->Params, gpu->ParamsImport );
                if ( imported )
                    imported = Renderer::ImportBuffer( gpu->Feedback, gpu->FeedbackImport );
                for ( const auto& [buffer, into] : { std::pair{ &gpu->CameraIds, &gpu->CameraIdsImport },
                                                     std::pair{ &gpu->CameraArgs, &gpu->CameraArgsImport },
                                                     std::pair{ &gpu->Candidates, &gpu->CandidatesImport },
                                                     std::pair{ &gpu->Chunks, &gpu->ChunksImport },
                                                     std::pair{ &gpu->LawScales, &gpu->LawScalesImport },
                                                     std::pair{ &gpu->PostArgs, &gpu->PostArgsImport },
                                                     std::pair{ &gpu->CascadeIds, &gpu->CascadeIdsImport },
                                                     std::pair{ &gpu->CascadeArgs, &gpu->CascadeArgsImport },
                                                     std::pair{ &gpu->TilePool, &gpu->TilePoolImport },
                                                     std::pair{ &gpu->FullIds, &gpu->FullIdsImport } } )
                    if ( imported )
                        imported = Renderer::ImportBuffer( *buffer, *into );
                // RENDER-JOB-CPU-2: the tile table, the pool's words and the tile feedback are read and written by
                // the tile passes alone (bindGenerate), which run only for a variety that frees or builds tiles
                // this frame - they are imported there (below), not on a frame that only turns the camera.
                // GRASS-VRAM-BUDGET-1: the pool's segments, which the passes read and write by address.
                gpu->TileSegmentImports.resize( gpu->TileSegments.size() );
                for ( std::size_t s = 0; s < gpu->TileSegments.size() && imported; ++s )
                    imported = Renderer::ImportBuffer( gpu->TileSegments[s], gpu->TileSegmentImports[s] );
                if ( !imported )
                {
                    LOG_ERROR( "[Grass] a variety's buffers are not in the frame graph: {}", imported.GetError() );
                    continue;
                }
                gpu->Refs.Instances = graph.RegisterExternal( gpu->InstancesImport, "GrassInstances" );
                gpu->Refs.Args      = graph.RegisterExternal( gpu->ArgsImport, "GrassArgs" );
                gpu->Refs.ArgsAddress = BufferAddress( gpu->Args ); // a compacted bin's gather reads them
                gpu->ParamsRef      = graph.RegisterExternal( gpu->ParamsImport, "GrassParams" );
                gpu->FeedbackRef    = graph.RegisterExternal( gpu->FeedbackImport, "GrassFeedback" );
                // GRASS-VIEW-LAW-1: the full list reads Instances' rows through FullIds too (RT, list-less cascades).
                gpu->Refs.Ids = graph.RegisterExternal( gpu->FullIdsImport, "GrassFullIds" );
                // GPU-COST-22: every list reads the one Instances through its ids (GpuInstanceRefs::Ids).
                gpu->CameraRefs.Instances  = gpu->Refs.Instances;
                gpu->CameraRefs.Ids        = graph.RegisterExternal( gpu->CameraIdsImport, "GrassCameraIds" );
                gpu->CameraRefs.Args      = graph.RegisterExternal( gpu->CameraArgsImport, "GrassCameraArgs" );
                gpu->CameraRefs.ArgsAddress = BufferAddress( gpu->CameraArgs );
                gpu->CandidatesRef        = graph.RegisterExternal( gpu->CandidatesImport, "GrassCandidates" );
                gpu->ChunksRef            = graph.RegisterExternal( gpu->ChunksImport, "GrassChunks" );
                gpu->LawScalesRef         = graph.RegisterExternal( gpu->LawScalesImport, "GrassLawScales" );
                gpu->PostRefs.Instances    = gpu->Refs.Instances;
                gpu->PostRefs.Ids          = gpu->CameraRefs.Ids;
                gpu->PostRefs.Args        = graph.RegisterExternal( gpu->PostArgsImport, "GrassPostArgs" );
                gpu->PostRefs.ArgsAddress = BufferAddress( gpu->PostArgs );
                gpu->CascadeRefs.Instances = gpu->Refs.Instances;
                gpu->CascadeRefs.Ids       = graph.RegisterExternal( gpu->CascadeIdsImport, "GrassCascadeIds" );
                gpu->CascadeRefs.Args = graph.RegisterExternal( gpu->CascadeArgsImport, "GrassCascadeArgs" );
                gpu->FieldRef              = fieldGpu.Ref;
                gpu->FieldBuffer           = fieldGpu.Buffer;
                gpu->TilePoolRef           = graph.RegisterExternal( gpu->TilePoolImport, "GrassTileSegments" );
                gpu->TileSegmentRefs.clear();
                for ( RDG::ExternalBuffer& segment : gpu->TileSegmentImports )
                    gpu->TileSegmentRefs.push_back( graph.RegisterExternal( segment, "GrassTilePool" ) );
                // Last frame's handles name another graph's resources: invalid until the tile passes import them.
                gpu->TileWordsRef    = {};
                gpu->TileTableRef    = {};
                gpu->TileFeedbackRef = {};
                DESERT_PROFILE_PHASE( varietyPhases, "Grass variety: Keys + tile table" );
                m_Frame.push_back( gpu );
                if ( gpu->Drawn )
                    DrawCounter::RecordGrass( gpu->Drawn->x, gpu->Drawn->y, gpu->Drawn->z, gpu->DrawnCascades );
                const auto revision = static_cast<uint64_t>( fieldGpu.Revision );
                gpu->FrameKey.push_back( static_cast<uint32_t>( revision ) );
                gpu->FrameKey.push_back( static_cast<uint32_t>( revision >> 32u ) );
                if ( gpu->FrameKey != gpu->GenerateKey )
                {
                    gpu->GenerateKey       = gpu->FrameKey;
                    ++gpu->Generation;
                }
                gpu->LawKey.push_back( static_cast<uint32_t>( revision ) );
                gpu->LawKey.push_back( static_cast<uint32_t>( revision >> 32u ) );
                if ( gpu->LawKey != gpu->LawedKey )
                {
                    gpu->LawedKey = gpu->LawKey;
                    ++gpu->LawGeneration;
                }
                // GRASS-VIEW-LAW-1: nothing is generated per view - the law is GrassCull's, every frame, over the
                // build's chunks. The tile passes run on a frame that frees or builds tiles and read the table written
                // then (a per-frame-in-flight copy); a fresh pool's Init pass writes its words first.
                const bool grown = gpu->TileGrowFrom != gpu->TilePages; // GRASS-VRAM-BUDGET-1
                bool       tiles = gpu->PoolFresh || grown || gpu->TileFrees != 0u || gpu->TileBuilds != 0u;
                if ( tiles )
                {
                    const auto bytes = static_cast<uint32_t>( gpu->TileEntries.size() * sizeof( glm::uvec4 ) );
                    if ( const auto wrote = gpu->TileTable->SetData( gpu->TileEntries.data(), bytes );
                         !wrote.IsSuccess() )
                    {
                        // No tile pass runs for it this frame; a new pool next frame builds its tiles again.
                        LOG_ERROR( "[Grass] the variety's tile table was not written: {}", wrote.GetError() );
                        gpu->TilePoolMade = false;
                        tiles             = false;
                    }
                    else
                    {
                        // The tile passes' own buffers, imported after the table's write (the copy it wrote).
                        Common::BoolResultStr tileImported =
                             Renderer::ImportBuffer( gpu->TileTable, gpu->TileTableImport );
                        if ( tileImported )
                            tileImported = Renderer::ImportBuffer( gpu->TileWords, gpu->TileWordsImport );
                        if ( tileImported )
                            tileImported = Renderer::ImportBuffer( gpu->TileFeedback, gpu->TileFeedbackImport );
                        if ( !tileImported )
                        {
                            // As an unwritten table: no tile pass this frame, a new pool next frame.
                            LOG_ERROR( "[Grass] the variety's tile buffers are not in the frame graph: {}",
                                       tileImported.GetError() );
                            gpu->TilePoolMade = false;
                            tiles             = false;
                        }
                        else
                        {
                            gpu->TileWordsRef = graph.RegisterExternal( gpu->TileWordsImport, "GrassTileWords" );
                            gpu->TileTableRef = graph.RegisterExternal( gpu->TileTableImport, "GrassTileTable" );
                            gpu->TileFeedbackRef =
                                 graph.RegisterExternal( gpu->TileFeedbackImport, "GrassTileFeedback" );
                            if ( gpu->PoolFresh )
                                gpu->TilePoolMade = true;
                        }
                    }
                }
                if ( tiles && ( gpu->PoolFresh || grown || gpu->TileFrees != 0u ) )
                    m_FrameTileFree.push_back( gpu );
                if ( tiles && gpu->TileBuilds != 0u )
                    m_FrameTileBuild.push_back( gpu );
                // The camera's list depends on last frame's depth when it is HZB-tested, so it is culled every
                // such frame; by the camera's planes alone it follows the generation's inputs (the planes are
                // among them), so it is culled when the variety generates.
                // Every frame: the camera's planes and the cascades' boxes are this frame's (GRASS-CULL-UNIFY-1).
                m_FrameCull.push_back( gpu );
                DESERT_PROFILE_PHASE( varietyPhases, "Grass variety: Texture demand" );
                NoteVarietyTextureDemand( variety, *mesh, *field.Field, frame,
                                          ScaledPlacement( variety.Placement, m_Quality ).EndCullDistance );

                DESERT_PROFILE_PHASE( varietyPhases, "Grass variety: Sections" );
                // RENDER-CPU-COST-2: the rows are built into scratch kept across varieties and frames (no allocation
                // once grown) and handed to the mesh renderer by move, so no buffer handle is copied twice.
                thread_local std::vector<MeshRenderer::GpuInstancedRenderData> t_Sections;
                std::vector<MeshRenderer::GpuInstancedRenderData>&             sections = t_Sections;
                sections.clear();
                sections.reserve( variety.Sections.size() );
                // GRASS-VIS-MESH-1: a mesh section's rows (null: the prepass pair this frame) and the mesh its
                // visibility resolve's full-screen triangle draws (a blade strip: its first three indices are 0,
                // 1, 2). Without a resident strip no mesh section leaves the pair: the resolve would draw the
                // grass mesh's own first triangle, not the screen.
                Desert::Mesh* resolveMesh = variety.Blade ? nullptr : BladeStrip( 1u );
                if ( resolveMesh != nullptr && !resolveMesh->GetGeometry().IsResident() )
                    resolveMesh = nullptr;
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
                           .Instances        = gpu->Instances,
                           .CameraIds        = gpu->CameraIds,
                           .BladeShape       = gpu->Shape,
                           .MeshVisibility   = meshRows,
                           .ResolveMesh      = resolveMesh,
                           .Refs             = &gpu->Refs,
                           .CameraRefs       = &gpu->CameraRefs,
                           .PostRefs         = testsHzb ? &gpu->PostRefs : nullptr,
                           .CascadeIds       = gpu->CascadeIds,
                           .FullIds          = gpu->FullIds,
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
                    sections.back().DrawBins = { &m_DrawBins[0], &m_DrawBins[1] }; // RENDER-JOB-CPU-4
                }
                // A Blade variety's levels in the view's TLAS: one shared BLAS per level, built from the shape
                // (GrassBladeRayTracing.hpp). A level a Masked section draws runs the alpha test, as MasksOf rules
                // for a mesh (RayTracingGather.cpp).
                if ( variety.Blade && m_BladeRayTracing.IsRequired() )
                {
                    uint64_t nonOpaque = 0;
                    for ( const MeshRenderer::GpuInstancedRenderData& section : sections )
                        if ( MeshRenderer::ShadingOf( section ).Masked )
                            nonOpaque |= ~section.HiddenSubmeshes;
                    const auto blas =
                         m_BladeRayTracing.PrepareVariety( graph, variety.Key, *mesh, gpu->ShapeData, nonOpaque );
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
        }
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
            if ( const Common::BoolResultStr table = WriteVarietyTable( graph, volumes ); !table )
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
                 .Storage( GrassGenerateShaderParameters::Params, gpu.ParamsRef, RDG::Access::StorageRead )
                 .Storage( GrassGenerateShaderParameters::TilePool, gpu.TilePoolRef, RDG::Access::StorageRead )
                 .Storage( GrassGenerateShaderParameters::TileWords, gpu.TileWordsRef, pool )
                 .Storage( GrassGenerateShaderParameters::Tiles, gpu.TileTableRef, RDG::Access::StorageRead )
                 .Storage( GrassGenerateShaderParameters::TileFeedback, gpu.TileFeedbackRef, pool )
                 .Storage( GrassGenerateShaderParameters::Chunks, gpu.ChunksRef, pool )
                 .PushConstantBytes( static_cast<uint32_t>( sizeof( GrassGeneratePush ) ) );
            // GRASS-VRAM-BUDGET-1: the builds write the records through the segment table.
            for ( const RDG::BufferRef& segment : gpu.TileSegmentRefs )
                pass.Write( segment, RDG::Access::StorageWrite );
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
                    return std::pair{
                         GrassGeneratePush{ glm::uvec4( kGrassTileModeInit, 0u, gpu.TileSlots, gpu.TilePages ),
                                            glm::uvec4( gpu.TileRoom, 0u, 0u, 0u ) },
                         glm::uvec3( ( threads + 63u ) / 64u, 1u, 1u ) };
                }
                // GRASS-VRAM-BUDGET-1: the Free pass also pushes a grown pool's new pages on the stack.
                const uint32_t          grown = gpu.TilePages - gpu.TileGrowFrom;
                const GrassGeneratePush push{ glm::uvec4( kGrassTileModeFree, 1u, gpu.TileFrees, gpu.TilePages ),
                                              glm::uvec4( gpu.TileRoom, gpu.TileGrowFrom, grown, 0u ) };
                const uint32_t          threads = std::max( gpu.TileFrees, grown );
                return std::pair{ push, glm::uvec3( ( threads + 63u ) / 64u, 1u, 1u ) };
            };
            graph.AddPass(
                 "Grass: Generate Free", RDG::PassFlags::Compute,
                 [this, bindGenerate]( RDG::PassBuilder& pass )
                 {
                     for ( const VarietyGpu* gpu : m_FrameTileFree )
                         bindGenerate( pass, *gpu );
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
                    const GrassGeneratePush push{
                         glm::uvec4( tileMode, 1u + gpu.TileFrees, gpu.TileBuilds, gpu.TilePages ),
                         glm::uvec4( gpu.TileRoom, 0u, 0u, 0u ) };
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
                     },
                     [this, dispatchTiles, buildShape]( RDG::PassContext& context ) -> Common::BoolResultStr
                     { return dispatchTiles( context, m_FrameTileBuild, buildShape ); } );
            }
        }

        // THE CAMERA CULL (GRASS-VIS-24; UE: the instance cull of every frame over instances built once), one
        // dispatch over every culled variety (GrassCull.shader). GRASS-CULL-HIER-1, two levels in one pass (UE
        // InstanceCulling / Nanite: a node's bound is culled by its group of lanes, which then cull its elements):
        // the rows' groups from their chunks, then one subgroup per chunk - the cascades' boxes, the camera's planes
        // and last frame's HZB against the chunk's box, then a lane per instance of a chunk a view met into the
        // camera's list and the cascades' - then the records, phase words and phase 2's groups. Its cost follows the
        // chunks and the instances the views can take, not the field.
        if ( m_FrameTable && cullRows != 0u )
        {
            RDG::BufferDesc dispatchDesc;
            dispatchDesc.Bytes = ( kCullDispatchHeader + cullRows ) * sizeof( glm::uvec4 );
            m_FrameDispatch    = graph.CreateBuffer( dispatchDesc, "GrassCullDispatch" );
            const RDG::TextureRef hzb = testsHzb ? occlusion.Hzb : occlusion.Neutral;
            GrassCullPush         layOut;
            layOut.HzbViewProjection = occlusion.PrevViewProjection;
            // Hzb.z = 0: the camera cull tests the camera's planes alone.
            layOut.Hzb  = testsHzb ? glm::uvec4( occlusion.Extent, kCullModeDispatch )
                                   : glm::uvec4( 0u, 0u, 0u, kCullModeDispatch );
            layOut.Rows = glm::uvec4( cullRows, 0u, 0u, 0u );
            GrassCullPush cull     = layOut;
            cull.Hzb.w             = kCullModeCameraCull;
            GrassCullPush finalize = layOut;
            finalize.Hzb.w         = kCullModeCameraFinalize;
            graph.AddPass(
                 "Grass: Cull Dispatch", RDG::PassFlags::Compute,
                 [this, hzb]( RDG::PassBuilder& pass )
                 {
                     BindCull( pass, hzb, RDG::Access::StorageWrite );
                     for ( const VarietyGpu* gpu : m_FrameCull )
                     {
                         // The law's slots restarted (Chunks' header).
                         pass.Write( gpu->ChunksRef, RDG::Access::StorageWrite );
                         pass.Write( gpu->Refs.Args, RDG::Access::StorageWrite ); // the full list's counts reset
                     }
                 },
                 [this, layOut]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return DispatchOneGroup( context, *m_Cull, &layOut, sizeof( layOut ) ); } );

            graph.AddPass(
                 "Grass: Camera Cull", RDG::PassFlags::Compute,
                 [this, hzb]( RDG::PassBuilder& pass )
                 {
                     BindCull( pass, hzb, RDG::Access::StorageRead );
                     pass.Read( m_FrameDispatch, RDG::Access::IndirectArgs );
                     for ( const VarietyGpu* gpu : m_FrameCull )
                     {
                         pass.Write( gpu->ChunksRef, RDG::Access::StorageWrite );    // the law words (LawChunk)
                         pass.Write( gpu->LawScalesRef, RDG::Access::StorageWrite ); // the law's slots
                         pass.Read( gpu->TilePoolRef, RDG::Access::StorageRead );
                         for ( const RDG::BufferRef& segment : gpu->TileSegmentRefs )
                             pass.Read( segment, RDG::Access::StorageRead ); // the view law's records
                         pass.Read( gpu->ParamsRef, RDG::Access::StorageRead );
                         pass.Read( gpu->FieldRef, RDG::Access::StorageRead );
                         pass.Write( gpu->Refs.Instances, RDG::Access::StorageWrite ); // the lists' rows
                         pass.Write( gpu->Refs.Ids, RDG::Access::StorageWrite );       // the full list
                         pass.Write( gpu->Refs.Args, RDG::Access::StorageWrite );
                         pass.Write( gpu->CameraRefs.Ids, RDG::Access::StorageWrite );
                         pass.Write( gpu->CameraRefs.Args, RDG::Access::StorageWrite );
                         pass.Write( gpu->CandidatesRef, RDG::Access::StorageWrite );
                         pass.Write( gpu->CascadeRefs.Ids, RDG::Access::StorageWrite );
                         pass.Write( gpu->CascadeRefs.Args, RDG::Access::StorageWrite );
                     }
                 },
                 [this, cull, dispatchBuffer = m_FrameDispatch]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     // One subgroup per chunk of every row: the groups the Cull Dispatch laid out.
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &cull, sizeof( cull ) );
                     return Renderer::DispatchComputeIndirect( bindings, *m_Cull, dispatchBuffer, 0u );
                 } );

            graph.AddPass(
                 "Grass: Camera Cull Finalize", RDG::PassFlags::Compute,
                 [this, hzb]( RDG::PassBuilder& pass )
                 {
                     BindCull( pass, hzb, RDG::Access::StorageWrite );
                     for ( const VarietyGpu* gpu : m_FrameCull )
                     {
                         pass.Write( gpu->CameraRefs.Args, RDG::Access::StorageWrite );
                         pass.Write( gpu->Refs.Args, RDG::Access::StorageWrite ); // the full list's records
                         pass.Write( gpu->FeedbackRef, RDG::Access::StorageWrite );
                         pass.Write( gpu->CascadeRefs.Args, RDG::Access::StorageWrite );
                         pass.Read( gpu->ChunksRef, RDG::Access::StorageRead ); // the law's slots handed out
                     }
                 },
                 [this, finalize]( RDG::PassContext& context ) -> Common::BoolResultStr
                 { return DispatchOneGroup( context, *m_Cull, &finalize, sizeof( finalize ) ); } );
            // RENDER-JOB-CPU-4: the first phase's compacted bins, gathered from the records just finalized.
            if ( const Common::BoolResultStr gathered = AddDrawGather( graph, 0u ); !gathered )
                LOG_ERROR( "[Grass] the grass draws per variety this frame: {}", gathered.GetError() );
        }

        // The blade levels whose BLAS this frame builds: their vertices, before the view's TLAS block requests the
        // builds (SceneRenderer).
        DESERT_PROFILE_PHASE( grassPhases, "Grass: Blade BLAS pass" );
        if ( const Common::BoolResultStr blades = m_BladeRayTracing.AddPass( graph ); !blades )
            LOG_ERROR( "[Grass] the blade grass is not in the view's TLAS; rays miss it this frame: {}",
                       blades.GetError() );
    }

    Common::BoolResultStr GrassRenderer::WriteVarietyTable( RDG::Builder& graph, const ViewVolumes& volumes )
    {
        GrassVarietyTableGpu header{};
        std::copy( std::begin( volumes.Camera ), std::end( volumes.Camera ), std::begin( header.CameraPlanes ) );
        std::copy( std::begin( volumes.CascadePlanes ), std::end( volumes.CascadePlanes ),
                   std::begin( header.CascadePlanes ) );
        header.CasterWindOrigin = glm::vec4( glm::vec3( volumes.CascadeWind[0] ), 0.0f );
        std::vector<uint8_t> image( sizeof( GrassVarietyTableGpu ) );
        std::memcpy( image.data(), &header, sizeof( header ) );
        for ( const std::vector<VarietyGpu*>* list : { &m_FrameCull } )
        {
            for ( VarietyGpu* gpu : *list )
            {
                GrassVarietyRowGpu row{};
                for ( const auto& [into, buffer] :
                      { std::pair{ &row.Instances, gpu->Instances.get() }, std::pair{ &row.Args, gpu->Args.get() },
                        std::pair{ &row.CameraIds, gpu->CameraIds.get() },
                        std::pair{ &row.CameraArgs, gpu->CameraArgs.get() },
                        std::pair{ &row.Candidates, gpu->Candidates.get() },
                        std::pair{ &row.PostArgs, gpu->PostArgs.get() },
                        std::pair{ &row.Feedback, gpu->Feedback.get() },
                        std::pair{ &row.CascadeArgs, gpu->CascadeArgs.get() },
                        std::pair{ &row.CascadeIds, gpu->CascadeIds.get() },
                        std::pair{ &row.Chunks, gpu->Chunks.get() },
                        std::pair{ &row.Params, gpu->Params.get() },
                        std::pair{ &row.Field, gpu->FieldBuffer.get() },
                        std::pair{ &row.TilePool, gpu->TilePool.get() },
                        std::pair{ &row.FullIds, gpu->FullIds.get() },
                        std::pair{ &row.LawScales, gpu->LawScales.get() } } )
                {
                    if ( buffer == nullptr )
                        return Common::MakeError( "a variety's buffer was not made" );
                    const Common::ResultStr<uint64_t> address = buffer->GetDeviceAddress();
                    if ( !address )
                        return Common::MakeFormattedError( "a variety's buffer has no device address: {}",
                                                           address.GetError() );
                    const uint64_t value = address.GetValue();
                    *into = glm::uvec2( static_cast<uint32_t>( value ), static_cast<uint32_t>( value >> 32u ) );
                }
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
                    std::vector<uint32_t> key;
                    const auto            append = [&key]( const auto& value )
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
                    // GRASS-CULL-COST-3: and the view law's generation - a frame whose law ran out of LawScales' slots
                    // culled the lists without those chunks (kGrassLawNoSlot), so the grown LawScales (a new law
                    // generation) culls them anew instead of holding the short lists.
                    append( row.LawGeneration );
                    if ( key != gpu->CascadeKey )
                    {
                        gpu->CascadeKey     = std::move( key );
                        gpu->CascadesCulled = false;
                    }
                    if ( gpu->CascadesCulled )
                        row.Info.z |= kRowCascadesHeld;
                    gpu->CascadesCulled = true;
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

    void GrassRenderer::BindCull( RDG::PassBuilder& pass, const RDG::TextureRef hzb, const RDG::Access dispatch )
    {
        pass.Bindings( m_CullLayout.Get( m_Cull->GetSpecification().Shader ), Renderer::GetPipelineRouteFill( *m_Cull ) )
             .Storage( GrassCullShaderParameters::Varieties, m_TableRef, RDG::Access::StorageRead )
             .Storage( GrassCullShaderParameters::CullDispatch, m_FrameDispatch, dispatch )
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
                 BindCull( pass, bound, RDG::Access::StorageRead );
                 pass.Read( m_FrameDispatch, RDG::Access::IndirectArgs );
                 for ( const VarietyGpu* gpu : m_FrameCull )
                 {
                     pass.Read( gpu->Refs.Args, RDG::Access::StorageRead );
                     pass.Read( gpu->CandidatesRef, RDG::Access::StorageRead );
                     pass.Write( gpu->CameraRefs.Ids, RDG::Access::StorageWrite );
                     pass.Write( gpu->CameraRefs.Args, RDG::Access::StorageWrite );
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
                 BindCull( pass, bound, RDG::Access::StorageRead );
                 for ( const VarietyGpu* gpu : m_FrameCull )
                 {
                     pass.Read( gpu->CameraRefs.Args, RDG::Access::StorageRead );
                     pass.Write( gpu->PostRefs.Args, RDG::Access::StorageWrite );
                     pass.Write( gpu->FeedbackRef, RDG::Access::StorageWrite );
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
        uint32_t levels = 0u;
        for ( const VarietyGpu* gpu : m_FrameCull )
            levels += std::max<uint32_t>( gpu->Buckets, 1u );
        uint32_t need = std::max( kDrawBinMinSlots, levels * kDrawBinSlotsPerLevel );
        if ( table.Overflow )
            need = std::max( need, table.SlotCapacity * 2u );
        table.Overflow = false;
        if ( need > table.SlotCapacity || buffers.Commands == nullptr )
        {
            using Buffer     = ShaderResources::StorageBuffer;
            const auto bytes = [need]( const uint64_t each ) { return static_cast<uint32_t>( need * each ); };
            buffers.Commands = Buffer::CreateDeviceLocal( "GrassDrawBinCmds", bytes( kGpuDrawBinCommandBytes ), 1 );
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
        table.BinReads.clear();
        table.BinReads.reserve( table.BinCapacity );
        table.Open = true;

        // The two runs (Compact.w): the clear of every bin's count, then the gather, its own pass so the graph
        // puts the barrier between the writes of the counts. The clear's run also uploads the entries and the rows: by
        // then every draw setup of the frame has filled the table (RDG::Builder::AddPass runs a setup at once).
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
                if ( phase == 0u )
                {
                    pass.Read( gpu->CameraRefs.Args, RDG::Access::StorageRead );
                    pass.Read( gpu->Refs.Args, RDG::Access::StorageRead );
                }
                else
                    pass.Read( gpu->PostRefs.Args, RDG::Access::StorageRead );
            }
        };
        const std::string clearName =
             phase == 0u ? "Grass: Draw Gather Clear" : "Grass: Draw Gather Clear Post-Occlusion";
        const std::string gatherName = phase == 0u ? "Grass: Draw Gather" : "Grass: Draw Gather Post-Occlusion";
        graph.AddPass( clearName, RDG::PassFlags::Compute, declare,
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
        graph.AddPass( gatherName, RDG::PassFlags::Compute, declare,
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
