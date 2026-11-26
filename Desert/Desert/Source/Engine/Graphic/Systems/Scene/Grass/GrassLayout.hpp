#pragma once

// THE GRASS GPU RECORDS, declared once: Tools/ShaderProtocolGen writes their GLSL twins into
// Generated/Grass/Grass.glslh (read by Mesh/Surface/GrassBlade.glslh, Mesh/Surface/GrassMeshVisibility.glslh,
// Common/GrassVarietyRows.glslh, Common/GrassTileRecord.glslh and Programs/Grass/GrassCull.shader) and checks the
// std430 layouts against these declarations at compile time.

#include <Engine/Graphic/Materials/SceneResourcesLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Desert::Graphic::System
{
    // GPU-COST-21: the cascade views a variety keeps a list of its own for, at most (every shadowed directional
    // light's cascades; MeshRenderer::kCascadeViews). GrassGenerate.shader CascadePlanes and GrassCull.shader
    // kCascadeViews are sized by it.
    inline constexpr uint32_t kGrassCascadeLists = SceneResources::kMaxCascadeViews;
    // A blade's atlas cards the shape carries; the generator reads at most this many (GrassParamsGpu::CardWidths,
    // two vec4s).
    inline constexpr size_t kGrassBladeMaxCards = 8u;
    // The hull's classes and bands per card (Assets::kGrassCardHullClasses / kGrassCardHullBands;
    // GrassBlade.glslh).
    inline constexpr uint32_t kGrassBladeHullClasses = 4u;
    inline constexpr uint32_t kGrassBladeHullBands   = 8u;
    // GRASS-VRAM-BUDGET-1: a variety's tile pool grows by segments of this many pages (3 MB), each its own buffer.
    inline constexpr uint32_t kGrassTileSegmentPages = 1024u;
    // GRASS-VIS-MESH-1: a mesh grass section's submeshes and (level x submesh) ranges its raster-stream rows hold.
    inline constexpr uint32_t kGrassMeshVisibilityMaxSubmeshes = 8u;
    inline constexpr uint32_t kGrassMeshVisibilityMaxRanges    = 32u;

    using Vec4ArrayBladeCards     = std::array<glm::vec4, kGrassBladeMaxCards>;
    using Vec4Array2              = std::array<glm::vec4, 2>;
    using Vec4Array4              = std::array<glm::vec4, 4>;
    using Vec4ArrayBladeHullLines = std::array<glm::vec4, kGrassBladeMaxCards * kGrassBladeHullClasses>;
    using Vec4ArrayBladeHulls =
         std::array<glm::vec4, kGrassBladeMaxCards * kGrassBladeHullClasses * kGrassBladeHullBands / 2u>;
    using Uint32Array3            = std::array<uint32_t, 3>;
    using Vec4ArrayCascadeRings   = std::array<glm::vec4, kGrassCascadeLists / 2u>;
    using Vec4ArrayCascadePlanes  = std::array<glm::vec4, kGrassCascadeLists * 6u>;
    using Uvec4Array2             = std::array<glm::uvec4, 2>;
    using Uvec4ArrayMeshSubmeshes = std::array<glm::uvec4, kGrassMeshVisibilityMaxSubmeshes>;
    using Uvec4ArrayMeshRanges    = std::array<glm::uvec4, kGrassMeshVisibilityMaxRanges>;

    // A variety's blade shape as its cells read it (GLSL GrassBladeShapeData, taken by value by GrassBlade.glslh's
    // functions). The blade's look is its material's (a scanned blade atlas); the shape carries only where on that
    // atlas each of its cards is and how the strip stands, bends and sways.
#define DESERT_GRASS_BLADE_SHAPE_DATA_GPU_FIELDS( X )                                                             \
    X( glm::vec4, Size, "Size", glm::vec4( 0.0f ) ) /* Height cm, Stiffness, Curvature, card count */             \
    X( glm::vec4, Bend, "Bend", glm::vec4( 0.0f ) ) /* CurvatureSpread, BendSpread, TrampledLength */             \
    /* atlas rect: u0, v tip, u1, v root (texture space, v down the image) */                                     \
    X( Vec4ArrayBladeCards, Cards, "Cards", {} )                                                                  \
    /* GRASS-VIS: x = the variety's first visibility-buffer raster-stream row (instance i is row x + i), y = the  \
     */                                                                                                           \
    /* strip's level-0 segments, z = its bucket (= strip level) count, w = the rows it owns */                    \
    /* (GrassRenderer::PrepareVariety) */                                                                         \
    X( glm::uvec4, Visibility, "Visibility", glm::uvec4( 0u ) )                                                   \
    /* the first instance of bucket b < 8, [b / 4][b % 4] (GrassRenderer's bucket table) */                       \
    X( Uvec4Array2, BucketFirst, "BucketFirst", {} )                                                              \
    /* CPU-GRASS-POOL-5: x = the camera list's first slot in the shared ids pool (its CameraIds range), so the */ \
    /* raster's gl_InstanceIndex - x is the variety's own slot; y = 1 when the view's blade setup records exist   \
     */                                                                                                           \
    /* this frame (FPS-GRASS-RESOLVE-2, GrassRenderer::CommitBladeSetup; 0: none), read through the bound */      \
    /* GrassBladeSetupRecords block, never by address (FPS-GRASS-BLADE-SETUP-BIND), z = w = 0 */                  \
    X( glm::uvec4, Lists, "Lists", glm::uvec4( 0u ) )
    DESERT_SHADER_STRUCT( GrassBladeShapeDataGpu, "GrassBladeShapeData", Std430,
                          DESERT_GRASS_BLADE_SHAPE_DATA_GPU_FIELDS );
#undef DESERT_GRASS_BLADE_SHAPE_DATA_GPU_FIELDS

    // A variety's whole shape buffer (the raster's GrassBladeShape block, binding 13, or its draw row's
    // GrassDrawRowBlade reference; the ray-tracing compute's GrassBladeShape): the cells' shape, then the cards'
    // cooked hulls, read only where the raster's strip is placed (Mesh/Surface/GrassBladeHull.glslh).
#define DESERT_GRASS_BLADE_SHAPE_GPU_FIELDS( X )                                                                  \
    X( GrassBladeShapeDataGpu, Blade, "Blade", {} )                                                               \
    /* FPS-GRASS-HULL: x, y = the opacity mask's level-0 texels, z = 1 when the raster's strip spans only its */  \
    /* card's hull (GrassQuality::BladeCardHull and a cooked hull), w = 0 */                                      \
    X( glm::vec4, HullTexels, "BladeHullTexels", glm::vec4( 0.0f ) )                                              \
    /* GrassBladeDraw::CardHulls: 8 cards x 4 classes x 8 bands, (lo, hi) pairs (GrassBlade.glslh) */             \
    X( Vec4ArrayBladeHulls, Hull, "BladeHull", {} )                                                               \
    /* FPS-GRASS-RASTER-2: per card x class (k x 4 + c) the one-segment strip's trapezoid (lo, hi at the root, */ \
    /* lo, hi at the tip) holding every band of Hull (GrassBladeHullLine); lo > hi: nothing passes */             \
    X( Vec4ArrayBladeHullLines, HullLine, "BladeHullLine", {} )
    DESERT_SHADER_STRUCT( GrassBladeShapeGpu, "GrassBladeShapeRecord", Std430,
                          DESERT_GRASS_BLADE_SHAPE_GPU_FIELDS );
#undef DESERT_GRASS_BLADE_SHAPE_GPU_FIELDS

    // GRASS-VIS-MESH-1: a mesh grass SECTION's raster-stream rows (GLSL GrassMeshVisibilityData; GrassRenderer::
    // PrepareVariety writes it every frame: the mesh's device addresses are read when used). Slot i of the
    // camera's list owns rows [x + i * y, x + (i + 1) * y) of Visibility; the draw of level L, submesh s fills
    // them from Ranges[L * w + s].z on, one row per 128 of its triangles.
#define DESERT_GRASS_MESH_VISIBILITY_GPU_FIELDS( X )                                                              \
    /* first row, rows per instance, level (bucket) count, submesh count */                                       \
    X( glm::uvec4, Visibility, "Visibility", glm::uvec4( 0u ) )                                                   \
    /* x = the slots the section's list holds (its capacity), y = the camera list's first slot in the ids pool,   \
     */                                                                                                           \
    /* z = the setup records' first row in the bound setup buffer, w = 1: records this frame */                   \
    /* (FPS-GRASS-MESH-SETUP-5) */                                                                                \
    X( glm::uvec4, Slots, "Slots", glm::uvec4( 0u ) )                                                             \
    /* the first slot of bucket (= level) b < 8, [b / 4][b % 4] */                                                \
    X( Uvec4Array2, BucketFirst, "BucketFirst", {} )                                                              \
    /* [0] vertex address lo, hi, stream address lo, hi; [1] index address lo, hi (uint32 indices), vertex */     \
    /* stride, stream stride (bytes): the mesh's own spans (MeshGeometry byte offsets added) */                   \
    X( Uvec4Array2, Geometry, "Geometry", {} )                                                                    \
    /* VertexOffset, VertexCount, drawn by this section, 0 */                                                     \
    X( Uvec4ArrayMeshSubmeshes, Submeshes, "Submeshes", {} )                                                      \
    /* per L x submeshes + s: first index, triangles, row offset, 0 */                                            \
    X( Uvec4ArrayMeshRanges, Ranges, "Ranges", {} )
    DESERT_SHADER_STRUCT( GrassMeshVisibilityGpu, "GrassMeshVisibilityData", Std430,
                          DESERT_GRASS_MESH_VISIBILITY_GPU_FIELDS );
#undef DESERT_GRASS_MESH_VISIBILITY_GPU_FIELDS

    // GRASS-CULL-2 (UE GPUScene / InstanceCulling: one table describes every primitive's instances, and one
    // dispatch culls them all): one row of THE VARIETY TABLE GrassCull reads (GLSL GrassVarietyRow), written once
    // per frame by GrassRenderer::WriteVarietyTable. A row names the variety's buffers by device address
    // (StorageBuffer::GetDeviceAddress: the copies this frame imports) and carries what the cull reads of its
    // params, so a pass over every variety binds the table once.
#define DESERT_GRASS_VARIETY_ROW_GPU_FIELDS( X )                                                                  \
    X( glm::uvec2, Instances, "Instances", {} ) /* mat4 per law slot, LawCapacity of them: the law's rows */      \
    /* GpuInstances.hpp: header (bucket count, capacity, record count, drawn), the draw records, then per bucket  \
     */                                                                                                           \
    /* count, first, capacity, half screen size */                                                                \
    X( glm::uvec2, Args, "Args", {} )                                                                             \
    X( glm::uvec2, CameraIds, "CameraIds",                                                                        \
       {} ) /* the camera's list: per slot the row of Instances (GPU-COST-22) */                                  \
    X( glm::uvec2, CameraArgs, "CameraArgs", {} ) /* Args' layout, then four phase words per bucket */            \
    /* per slot two uvec4: the hidden instance's sphere, its row of Instances */                                  \
    X( glm::uvec2, Candidates, "Candidates", {} )                                                                 \
    X( glm::uvec2, PostArgs, "PostArgs", {} ) /* Args' layout: phase 2's draws */                                 \
    /* this frame in flight's copy: per bucket the generator's count, the written mark, the full list's, the */   \
    /* camera's phase-1 and phase 2's drawn counts, one per cascade view */                                       \
    X( glm::uvec2, Feedback, "Feedback", {} )                                                                     \
    /* per cascade list one copy of Args' layout (GrassGenerate.shader CascadeArgs) */                            \
    X( glm::uvec2, CascadeArgs, "CascadeArgs", {} )                                                               \
    /* x = bucket count, y = the bucket table's first word in Args, z = flags (kRowBlade), w = the cascade */     \
    /* views with a list (VarietyGpu::CascadeMask) */                                                             \
    X( glm::uvec4, Info, "Info", {} )                                                                             \
    /* VarietyGpu::CullBounds: x = bounds radius (local), y = bounds centre height (local), z = the cull pad */   \
    /* (cm: a mesh's sway past its bounds), w = 0 */                                                              \
    X( glm::vec4, Bounds, "Bounds", {} )                                                                          \
    /* GRASS-CULL-UNIFY-1, the cascades' lists GrassCull culls every frame: CascadeIds' address, x = one list's   \
     */                                                                                                           \
    /* words (VarietyGpu::CascadeArgWords), y = its first rest count, and list k's ring at [k / 2] .xy / .zw */   \
    /* (VarietyGpu::CascadeRings: x = inner, y = wind distance, cm; 0: no mover) */                               \
    X( glm::uvec2, CascadeIds, "CascadeIds", {} )                                                                 \
    X( glm::uvec2, CascadeWords, "CascadeWords", {} )                                                             \
    X( Vec4ArrayCascadeRings, CascadeRings, "CascadeRings", {} )                                                  \
    /* GRASS-VIEW-LAW-1, what the view law reads and writes (Common/GrassVarietyRows.glslh, the chunk */          \
    /* contract): the build's chunks (VarietyGpu::Chunks, its header holds the room), the variety's params, the   \
     */                                                                                                           \
    /* field, the tile pool's segment table and the full list's ids */                                            \
    X( glm::uvec2, Chunks, "Chunks", {} )                                                                         \
    X( glm::uvec2, Params, "Params", {} )                                                                         \
    X( glm::uvec2, Field, "Field", {} )                                                                           \
    X( glm::uvec2, TilePool, "TilePool", {} )                                                                     \
    X( glm::uvec2, FullIds, "FullIds", {} )                                                                       \
    /* GRASS-CULL-COST-3, THE LAW'S SLOTS (GrassCull.shader LawChunk): the law generation */                      \
    /* (VarietyGpu::LawGeneration, never 0: a chunk the build wrote carries 0) and the slots of Instances */      \
    X( uint32_t, LawGeneration, "LawGeneration", 0u )                                                             \
    X( uint32_t, LawCapacity, "LawCapacity", 0u )                                                                 \
    /* CPU-GRASS-POOL-5: the ranges' first row of Instances and first slot of each id list in their pools */      \
    /* (GrassCull.shader adds them to the ids it writes and to the records' firstInstance) */                     \
    X( uint32_t, InstancesBase, "InstancesBase", 0u )                                                             \
    X( uint32_t, FullIdsBase, "FullIdsBase", 0u )                                                                 \
    X( uint32_t, CameraIdsBase, "CameraIdsBase", 0u )                                                             \
    X( uint32_t, CascadeIdsBase, "CascadeIdsBase", 0u )                                                           \
    /* CPU-GRASS-POOL-7: the mark FinalizeFull writes into Feedback (VarietyGpu::FeedbackMark) */                 \
    X( uint32_t, FeedbackMark, "FeedbackMark", 0u )                                                               \
    /* std430 places LodEyes at the next 16 bytes (256); glm::vec4 aligns to 4 in C++, so the gap is spelt out */ \
    X( Uint32Array3, Pad, "Pad", {} )                                                                             \
    /* LOD-DITHER-GRASS-SIDE: the law's two LOD eyes (VarietyGpu::LodEyes; GrassCull.shader GrassViewLaw), xyz */ \
    /* = the view's two temporal LOD samples snapped to the field eye's tile grid */                              \
    X( Vec4Array2, LodEyes, "LodEyes", ( Vec4Array2{ glm::vec4( 0.0f ), glm::vec4( 0.0f ) } ) )
    DESERT_SHADER_STRUCT( GrassVarietyRowGpu, "GrassVarietyRow", Std430, DESERT_GRASS_VARIETY_ROW_GPU_FIELDS );
#undef DESERT_GRASS_VARIETY_ROW_GPU_FIELDS

    // The variety table's header (GrassCull.shader's Varieties block: these members, then GrassVarietyRow
    // VarietyRows[]).
#define DESERT_GRASS_VARIETY_TABLE_GPU_FIELDS( X )                                                                \
    /* the camera's side planes (ViewVolumes::Camera): left / right / bottom / top, xyz.n + w >= 0 inside */      \
    X( Vec4Array4, CameraPlanes, "VarietyCameraPlanes", {} )                                                      \
    /* GRASS-CULL-UNIFY-1: list k's caster box at 6k..6k+5 (ViewVolumes::CascadePlanes, */                        \
    /* MeshRenderer::CascadeCasterBoxSet) */                                                                      \
    X( Vec4ArrayCascadePlanes, CascadePlanes, "VarietyCascadePlanes", {} )                                        \
    /* the shadow wind rule's origin: xyz = MeshRenderer::CasterWindOrigin of the first list's view */            \
    X( glm::vec4, CasterWindOrigin, "VarietyCasterWindOrigin", {} )                                               \
    /* DEFORM-GRASS-3: the deformation field's window (ViewVolumes::TrampleWindow): xy world X, Z min, zw max */  \
    /* (cm); min > max = nothing pressed. GrassCull's CullInstance tests a blade rooted inside it by its root */  \
    /* sphere of laid-flat reach */                                                                               \
    X( glm::vec4, TrampleWindow, "VarietyTrampleWindow", {} )
    DESERT_SHADER_STRUCT( GrassVarietyTableGpu, "GrassVarietyTable", Std430,
                          DESERT_GRASS_VARIETY_TABLE_GPU_FIELDS );
#undef DESERT_GRASS_VARIETY_TABLE_GPU_FIELDS

    // A variety's params (GLSL GrassParams), read by the tile build (GrassGenerate.shader) and the view law
    // (GrassCull.shader GrassViewLaw) through the variety row's Params address.
#define DESERT_GRASS_PARAMS_GPU_FIELDS( X )                                                                       \
    /* GRASS-CULL-UNIFY-1: xyz = the field eye (cm: the camera snapped to the tile grid, GrassRenderer */         \
    /* FieldEye), w = EndCullDistance through the lens - no view volume, the same for every view */               \
    X( glm::vec4, Camera, "Camera", {} )                                                                          \
    /* x, y = Start/EndCullDistance through the lens (DrawDistanceScale), z = cell (cm), w = max jitter (cells)   \
     */                                                                                                           \
    X( glm::vec4, Cull, "Cull", {} )                                                                              \
    /* x/y = AllowedDensityRange, z = MaxScaleWeightAttenuation, w = bounds radius (local) */                     \
    X( glm::vec4, Range, "Range", {} )                                                                            \
    /* x/y = ScaleX range, z = scaling (0 uniform, 1 free, 2 lock XY), w = flags */                               \
    X( glm::vec4, ScaleX, "ScaleX", {} )                                                                          \
    X( glm::vec4, ScaleY, "ScaleY", {} ) /* x/y = ScaleY range, z = bounds centre height (local) */               \
    /* x/y = ScaleZ range (up), w = the floor of the lens share (1 / DrawDistanceScale, at most 1; LensShare) */  \
    X( glm::vec4, ScaleZ, "ScaleZ", {} )                                                                          \
    X( glm::uvec4, Info, "Info", {} ) /* x = layer, y = capacity, z = submesh count, w = seed */                  \
    /* x = bucket count, y = the bucket table's first word in Args, z = a blade's card count, w = a blade's */    \
    /* finest strip segment count when the LOD of the curve is on (Lod.x / y; 0: off) */                          \
    X( glm::uvec4, Buckets, "Buckets", {} )                                                                       \
    /* x = LodDistance through the lens (0: one bucket), y = ClumpSize cm, z = Clumping, w = ClumpPull */         \
    X( glm::vec4, Blade, "Blade", {} )                                                                            \
    /* x = Scatter (cells), y = Patchiness, z = PatchSize cm, w = the screen-width floor's distance (cm, 0: none) \
     */                                                                                                           \
    X( glm::vec4, Spread, "Spread", {} )                                                                          \
    /* x = Tilt (radians), y = FacingJitter (radians), z = the world's SnowDepth (cm), w = the screen-width */    \
    /* floor's width per unit of distance (MinScreenWidth / pixels per unit; 0: none) */                          \
    X( glm::vec4, Lean, "Lean", {} )                                                                              \
    /* A mesh's per-instance level by screen size (Geometry::BoundsScreenRadiusSquared): x = the lens's screen */ \
    /* multiple x the mesh's local half-diagonal, y = |Projection[2][3]| (0: orthographic) x the Foliage */       \
    /* quality's MeshLodDistanceScale squared (FPS-GRASS-MESH-RASTER, UE foliage.LODDistanceScale). A blade's */  \
    /* LOD of the curve (FPS-GRASS-RASTER-COST): x = Height x PixelsPerUnit, y = the Foliage quality's */         \
    /* BladeSegmentPixels (0, 0: off; GrassFieldShare.glslh GrassBladeCurveLevel). The far LOD of a blade */      \
    /* (GPU-FRAME-4) or a mesh (GPU-FRAME-6): z = the distance at which its bounding radius at scale 1 stands */  \
    /* at the Foliage quality's LodScreenPixels (0: none; GrassFieldShare.glslh GrassBladeLodAt, */               \
    /* GrassMeshFarLod). w, a blade: the distance its coarsest level's instances draw from as the small bucket */ \
    /* (BladeStrip's BladeSmall; 0: none); a mesh: 0 */                                                           \
    X( glm::vec4, Lod, "Lod", {} )                                                                                \
    /* The variety's world patches: x = Coverage (1: none), y = Size cm, z = Softness, w = the noise seed's bits  \
     */                                                                                                           \
    /* (shared by a Group) */                                                                                     \
    X( glm::vec4, Patches, "Patches", {} )                                                                        \
    /* A blade's cards' widths, cm at scale 1 (card i at [i / 4][i % 4], Buckets.z of them): the card an */       \
    /* instance draws (Mesh/Surface/GrassBlade.glslh BladeCard) is lifted to the screen-width floor */            \
    X( Vec4Array2, CardWidths, "CardWidths", {} )
    DESERT_SHADER_STRUCT( GrassParamsGpu, "GrassParams", Std430, DESERT_GRASS_PARAMS_GPU_FIELDS );
#undef DESERT_GRASS_PARAMS_GPU_FIELDS

    // FPS-GRASS-SETUP-FUSE: per row of GrassCull's pass (the variety table's row r) what its camera list's setup
    // records need (GLSL GrassCullSetupRow): a blade variety's shape (the GrassBladeShapeData its cells read,
    // without the hull) or a mesh variety's setup section's rows (in Shape.Visibility / BucketFirst / Lists), the
    // raster draw row's view wind (PackViewInstanceWind) and what its camera slots' records are.
#define DESERT_GRASS_BLADE_SETUP_ROW_GPU_FIELDS( X )                                                              \
    X( GrassBladeShapeDataGpu, Shape, "Shape", {} )                                                               \
    X( glm::vec4, WindA, "WindA", glm::vec4( 0.0f ) )                                                             \
    X( glm::vec4, WindB, "WindB", glm::vec4( 0.0f ) )                                                             \
    /* x = kCullSetupNone / kCullSetupBlade / kCullSetupMesh, yzw = 0 */                                          \
    X( glm::uvec4, Kind, "Kind", glm::uvec4( 0u ) )
    DESERT_SHADER_STRUCT( GrassBladeSetupRowGpu, "GrassCullSetupRow", Std430,
                          DESERT_GRASS_BLADE_SETUP_ROW_GPU_FIELDS );
#undef DESERT_GRASS_BLADE_SETUP_ROW_GPU_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kGrassStructs[] = {
         &kGrassBladeShapeDataGpuStruct, &kGrassBladeShapeGpuStruct,   &kGrassMeshVisibilityGpuStruct,
         &kGrassVarietyRowGpuStruct,     &kGrassVarietyTableGpuStruct, &kGrassParamsGpuStruct,
         &kGrassBladeSetupRowGpuStruct,
    };
    inline constexpr ShaderProtocols::ShaderConstantDecl kGrassConstants[] = {
         { ShaderProtocols::ShaderConstantDecl::Type::Uint, "GRASS_MESH_VISIBILITY_MAX_SUBMESHES",
           kGrassMeshVisibilityMaxSubmeshes },
         { ShaderProtocols::ShaderConstantDecl::Type::Uint, "GRASS_MESH_VISIBILITY_MAX_RANGES",
           kGrassMeshVisibilityMaxRanges },
         { ShaderProtocols::ShaderConstantDecl::Type::Uint, "GRASS_BLADE_HULL_CLASSES", kGrassBladeHullClasses },
         { ShaderProtocols::ShaderConstantDecl::Type::Uint, "GRASS_BLADE_HULL_BANDS", kGrassBladeHullBands },
         { ShaderProtocols::ShaderConstantDecl::Type::Uint, "GRASS_TILE_SEGMENT_PAGES", kGrassTileSegmentPages },
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kGrassShaderInclude{
         .Path         = "Generated/Grass/Grass.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Grass/GrassLayout.hpp",
         .Structs      = kGrassStructs,
         .Constants    = kGrassConstants,
    };
} // namespace Desert::Graphic::System
