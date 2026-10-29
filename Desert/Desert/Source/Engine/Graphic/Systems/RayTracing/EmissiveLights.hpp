#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// THE SCENE'S EMISSIVE TRIANGLES AS LIGHTS (UE 5.8 Path Tracer emissive mesh light sampling; NVIDIA RTXDI: the
// emissive triangles of the scene in the one light list its samplers draw from). ONE list for every consumer -
// the path tracer's next event estimation and the surface cache's direct lighting under EITHER GI tracer
// (GI-EMISSIVE-LIGHTS), MegaLights next - built from the SCENE'S MESHES, not from a TLAS: the gather's rows and
// per-segment records (RayTracingGatherResult::Primitives / SegmentRecords - a scene gather when the frame
// traces no TLAS) and each mesh's own geometry. Every row's segment whose material emits becomes an emissive
// segment, every triangle of it an emissive triangle (world positions, area, power), and a power tree over the
// triangles picks one in proportion to its power (EmissiveLightsRenderer.hpp, Common/EmissiveLights.glslh). A
// frame that builds a TLAS adds the hit lookup (TLAS instance, geometry index) -> row over the same list.
//
// ONE SOURCE OF THE POSITIONS: a segment's triangles are read from the vertices the raster draws and the BLAS is
// built from. A static mesh's: its own vertex stream in the geometry pool (MeshGeometry); a deformed
// (skinned) primitive's BLAS reads the skin cache's current positions (RayTracingPrimitive::DeformedPositions,
// kDeformedPositionLayout), so its emissive segments read those - this frame's skinned pose, ordered after the
// skin pass by the graph. The list is therefore TWO parts under one root:
//   * STATIC: the segments whose positions do not move under an unchanged row - rebuilt only when their hash
//     (StaticHash) moves, kept across frames otherwise;
//   * DYNAMIC: the deformed primitives' segments - re-expanded every frame from that frame's skinned positions
//     (their bytes change with no row changing, so no hash could tell); an animated character re-expands its
//     own triangles, never the static list.
// The sampler picks between the two parts by their total powers (the common root), then descends that part's tree.
//
// Two sides, as the rest of the RT scene:
//   * this file, device-free but for the meshes' buffer addresses: the CPU list of emissive segments (transform,
//     the segment's vertex and index streams, its triangle range in its part, the material's emitted radiance)
//     and, on a frame with a TLAS, the lookup from a hit (TLAS instance, geometry index) to its segment, so a ray
//     that meets an emissive triangle names its row;
//   * EmissiveLightsRenderer.hpp: the graph passes that expand the segments into triangles on the GPU (the
//     vertices live in device buffers only) and reduce their powers into the trees.
//
// The material stays the source of the emitted radiance at a point: a sample on a triangle is lit by what the
// segment's hit group returns there (payload Emissive, a textured emission included), and the segment's
// Radiance here is the estimate the tree's powers are made of (the material's emissive colour x intensity).
// A segment the estimate calls dark is not in the list; a ray that meets it still sees its emission, with
// the whole weight (its light pdf is 0), so the estimate decides only how well the light is sampled, never
// what the image converges to.
namespace Desert::Graphic::RayTracing
{
    inline constexpr uint32_t kNoEmissiveSegment = 0xFFFFFFFFu;
    // A lookup slot with this bit names a row of the DYNAMIC segments (its low 31 bits), else a static row.
    inline constexpr uint32_t kEmissiveDynamicSegmentBit = 0x80000000u;

    // The surface templates' emission parameters (StandardSurface, StandardSurfaceMasked, FoliageSurface
    // `Properties`; their EvaluateSurface emits SurfaceEmission( texel, EmissiveColor, EmissiveIntensity )): what
    // the renderer's EmissiveRadianceFn reads from a segment's effective row (MaterialBinder::ReadRowParam). A
    // template without them has no estimate: its segments are not in the list, and their emission keeps the whole
    // weight.
    inline constexpr std::string_view kSurfaceEmissiveColorParam     = "EmissiveColor";
    inline constexpr std::string_view kSurfaceEmissiveIntensityParam = "EmissiveIntensity";

    // std430, 112 B: Common/EmissiveLights.glslh's EmissiveSegment. One scene row's emissive segment.
    struct EmissiveSegmentGpu
    {
        std::array<glm::vec4, 3> ObjectToWorld{}; // the row's world, row-major 3x4 (RayTracingPrimitive::World)
        glm::vec4 Radiance{ 0.0f };   // rgb: the material's emitted radiance estimate; w: its luminance
        uint64_t  VertexAddress  = 0; // the positions the BLAS was built from (mesh stream / skin-cache stream)
        uint64_t  IndexAddress   = 0; // uint32 indices
        uint32_t  VertexStride   = 0;
        uint32_t  PositionOffset = 0; // bytes from a vertex's start to its position (RayTracingGeometrySegment)
        uint32_t  FirstVertex    = 0;
        uint32_t  FirstIndex     = 0; // in indices
        uint32_t  FirstTriangle  = 0; // the segment's first row in its part's triangle list
        uint32_t  TriangleCount  = 0;
        uint32_t  GpuSceneRow    = 0; // the row the segment is drawn on (a TLAS hit's custom index)
        uint32_t  SubmeshIndex   = 0; // the submesh the segment is
    };
    static_assert( sizeof( EmissiveSegmentGpu ) == 112 );

    // std430, 48 B: Common/EmissiveLights.glslh's EmissiveTriangle, written by "EmissiveLights: Build".
    inline constexpr uint64_t kEmissiveTriangleBytes = 48;

    // The material's emitted radiance estimate of a segment (zero: the segment does not emit).
    using EmissiveRadianceFn = std::function<glm::vec3( const RayTracingSegmentRecord& )>;

    struct EmissiveTriangleList
    {
        // Each part's rows: ascending FirstTriangle, end to end, from 0 in that part.
        std::vector<EmissiveSegmentGpu> Segments;        // static
        std::vector<EmissiveSegmentGpu> DynamicSegments; // the deformed primitives', this frame's pose
        // The skin-cache streams the dynamic segments read (one per deformed primitive with an emissive segment),
        // as registered in the graph the primitives were gathered for: the dynamic Build pass reads them.
        std::vector<RDG::BufferRef> DynamicPositions;
        // Indexed by TLAS instance first (InstanceCount entries): the absolute index in Lookup of the instance's
        // per-geometry slots, or kNoEmissiveSegment for an instance with no emissive segment; then the slots,
        // each the row of that (instance, geometry index) - static, or dynamic with kEmissiveDynamicSegmentBit -
        // or kNoEmissiveSegment.
        std::vector<uint32_t> Lookup;
        uint32_t              InstanceCount        = 0;
        uint32_t              TriangleCount        = 0; // static
        uint32_t              DynamicTriangleCount = 0;
        // Emissive segments of deformed primitives left out this frame: their skin cache wrote no positions (no
        // ray-traced scene this frame). Counted, so the caller says it (AddFrameEmissiveLights).
        uint32_t UnplacedDeformedSegments = 0;
        // FNV-1a of what the static part is built from (Segments, Lookup): the renderer rebuilds it on a change.
        uint64_t StaticHash = 0;
        // StaticHash continued over DynamicSegments: what this frame's whole list is (the path tracer restarts on
        // a change).
        uint64_t Hash = 0;
    };

    [[nodiscard]] inline float EmissiveLuminance( const glm::vec3& c )
    {
        return glm::dot( c, glm::vec3( 0.2126f, 0.7152f, 0.0722f ) );
    }

    // PURE but for the meshes' device addresses (VertexBuffer / IndexBuffer::GetDeviceAddress). THE LIST IS BUILT
    // FROM THE SCENE'S MESHES, NOT FROM A TLAS: @p primitives / @p segments are a gather's rows and segment records
    // (GatherRayTracingPrimitives - the TLAS gather when this frame builds one, a SCENE gather otherwise; the same
    // rows, transforms, sections and materials either way), and each emissive record's triangles are read from its
    // mesh's own geometry (MeshGeometry: the pool span the raster draws and the BLAS is built from) over the
    // segment rule every BLAS uses (DescribeMeshGeometry with the row's masks: record FirstSegmentRecord + g is
    // segment g). A deformed primitive's records read its skin-cache positions (RayTracingPrimitive::
    // DeformedPositions) - this frame's pose.
    //
    // @p instances are this frame's TLAS rows in TLAS order (RayTracingInstanceList::Instances), EMPTY when the
    // frame builds no TLAS: they only make the hit lookup (a ray that meets an emissive triangle names its row by
    // TLAS instance + geometry index; instance i is the gather row CustomIndex, its record FirstSegmentRecord + g),
    // so they must come from the same gather as @p primitives.
    //
    // Errors, never a skipped row: a record past the table, an emissive mesh with no device address (a device
    // without bufferDeviceAddress - every device with hardware ray tracing has it), a mesh whose segments disagree
    // with its record run, an emitting TLAS segment the list holds no row of. Left out and COUNTED
    // (EmissiveTriangleList::UnplacedDeformedSegments): a deformed primitive's emissive segment on a frame whose
    // skin cache wrote no positions for it (no ray-traced scene this frame; the software tracer's scene holds no
    // deformed mesh either). The landscape grass rows (GpuInstanced: no CPU transform per instance) are not read.
    [[nodiscard]] inline Common::ResultStr<EmissiveTriangleList> BuildEmissiveTriangleList(
         std::span<const RayTracingPrimitive> primitives, std::span<const RayTracingSegmentRecord> segments,
         std::span<const RayTracingInstance> instances, const EmissiveRadianceFn& radianceOf )
    {
        using Result = EmissiveTriangleList;
        Result list;
        list.InstanceCount = static_cast<uint32_t>( instances.size() );

        // The estimate of each record once: the instances of one ISM share their mesh's run.
        std::vector<glm::vec3> radiance( segments.size(), glm::vec3( -1.0f ) );
        // (GPU-scene row, segment record) -> its row in its part (dynamic with kEmissiveDynamicSegmentBit).
        std::unordered_map<uint64_t, uint32_t> slotOf;
        const auto key = []( const uint32_t row, const uint32_t record )
        { return ( static_cast<uint64_t>( row ) << 32u ) | record; };

        uint64_t staticTriangles  = 0;
        uint64_t dynamicTriangles = 0;
        for ( std::size_t p = 0; p < primitives.size(); ++p )
        {
            const RayTracingPrimitive& primitive = primitives[p];
            if ( primitive.Kind == RayTracingPrimitiveKind::GpuInstanced || primitive.SegmentRecordCount == 0u )
                continue;
            if ( static_cast<std::size_t>( primitive.FirstSegmentRecord ) + primitive.SegmentRecordCount >
                 segments.size() )
                return Common::MakeFormattedError<Result>(
                     "EmissiveLights: primitive {} (row {}) names segment records {}..{} of {}", p,
                     primitive.GpuSceneRow, primitive.FirstSegmentRecord,
                     primitive.FirstSegmentRecord + primitive.SegmentRecordCount, segments.size() );
            bool emits = false;
            for ( uint32_t g = 0; g < primitive.SegmentRecordCount; ++g )
            {
                const std::size_t record = static_cast<std::size_t>( primitive.FirstSegmentRecord ) + g;
                if ( radiance[record].x < 0.0f )
                    radiance[record] = glm::max( radianceOf( segments[record] ), glm::vec3( 0.0f ) );
                emits = emits || EmissiveLuminance( radiance[record] ) > 0.0f;
            }
            if ( !emits )
                continue;

            const bool deformed = primitive.DeformedMesh != nullptr;
            // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): the gather stores these exact types
            const Mesh* mesh = deformed ? static_cast<const Mesh*>( static_cast<const SkinnedMesh*>( primitive.DeformedMesh ) )
                                        : static_cast<const Mesh*>( primitive.SourceMesh );
            // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
            if ( mesh == nullptr )
                return Common::MakeFormattedError<Result>( "EmissiveLights: primitive {} (row {}) emits and names "
                                                           "no mesh",
                                                           p, primitive.GpuSceneRow );
            if ( deformed && ( primitive.DeformedPositionsAddress == 0u || !primitive.DeformedPositions.IsValid() ) )
            {
                for ( uint32_t g = 0; g < primitive.SegmentRecordCount; ++g )
                    if ( EmissiveLuminance( radiance[primitive.FirstSegmentRecord + g] ) > 0.0f )
                        ++list.UnplacedDeformedSegments;
                continue;
            }
            const MeshGeometry& geometry  = mesh->GetGeometry();
            const uint64_t      indexBase = geometry.Indices ? geometry.Indices->GetDeviceAddress() : 0u;
            const uint64_t      vertexBase =
                 deformed ? primitive.DeformedPositionsAddress
                               : ( geometry.Vertices ? geometry.Vertices->GetDeviceAddress() : 0u );
            if ( indexBase == 0u || vertexBase == 0u )
                return Common::MakeFormattedError<Result>(
                     "EmissiveLights: the emissive mesh of row {} has no device address (not resident, or the "
                     "device has no bufferDeviceAddress)",
                     primitive.GpuSceneRow );
            // The mesh's own spans (indices stay mesh-relative, GeometryPool.hpp); the skin cache's stream starts
            // at the mesh's vertex 0.
            const uint64_t indexAddress  = indexBase + geometry.IndexByteOffset;
            const uint64_t vertexAddress = deformed ? vertexBase : vertexBase + geometry.VertexByteOffset;
            const std::vector<RayTracingGeometrySegment> described =
                 DescribeMeshGeometry( mesh->GetSubmeshes(), primitive.Submeshes,
                                       deformed ? kDeformedPositionLayout : kStaticMeshVertexLayout );
            if ( described.size() != primitive.SegmentRecordCount )
                return Common::MakeFormattedError<Result>(
                     "EmissiveLights: row {} has {} segment records and its mesh {} segments", primitive.GpuSceneRow,
                     primitive.SegmentRecordCount, described.size() );
            if ( deformed )
                list.DynamicPositions.push_back( primitive.DeformedPositions );

            for ( uint32_t g = 0; g < primitive.SegmentRecordCount; ++g )
            {
                const uint32_t                   record  = primitive.FirstSegmentRecord + g;
                const glm::vec3                  emitted = radiance[record];
                const RayTracingGeometrySegment& segment = described[g];
                if ( EmissiveLuminance( emitted ) <= 0.0f || segment.PrimitiveCount == 0u )
                    continue;
                std::vector<EmissiveSegmentGpu>& rows      = deformed ? list.DynamicSegments : list.Segments;
                uint64_t&                        triangles = deformed ? dynamicTriangles : staticTriangles;
                if ( rows.size() >= kEmissiveDynamicSegmentBit )
                    return Common::MakeFormattedError<Result>(
                         "EmissiveLights: {} emissive segments exceed the 31-bit segment index", rows.size() );
                const auto rowIndex = static_cast<uint32_t>( rows.size() );
                slotOf[key( primitive.GpuSceneRow, record )] =
                     deformed ? ( rowIndex | kEmissiveDynamicSegmentBit ) : rowIndex;

                EmissiveSegmentGpu& row = rows.emplace_back();
                // The row's world as a row-major 3x4 (glm is column-major: row r of column c is World[c][r]).
                for ( glm::length_t r = 0; r < 3; ++r )
                    row.ObjectToWorld[static_cast<std::size_t>( r )] =
                         glm::vec4( primitive.World[0][r], primitive.World[1][r], primitive.World[2][r],
                                    primitive.World[3][r] );
                row.Radiance       = glm::vec4( emitted, EmissiveLuminance( emitted ) );
                row.VertexAddress  = vertexAddress;
                row.IndexAddress   = indexAddress;
                row.VertexStride   = segment.VertexStride;
                row.PositionOffset = segment.PositionOffset;
                row.FirstVertex    = segment.FirstVertex;
                row.FirstIndex     = segment.IndexByteOffset / static_cast<uint32_t>( sizeof( uint32_t ) );
                row.FirstTriangle  = static_cast<uint32_t>( triangles );
                row.TriangleCount  = segment.PrimitiveCount;
                row.GpuSceneRow    = primitive.GpuSceneRow;
                row.SubmeshIndex   = segment.SubmeshIndex;
                triangles += segment.PrimitiveCount;
                // The triangle index and the trees' leaves are 32-bit on the GPU, and the two parts share one
                // index space (the dynamic triangles follow the static ones).
                if ( staticTriangles + dynamicTriangles > 0x7FFFFFFFull )
                    return Common::MakeFormattedError<Result>(
                         "EmissiveLights: {} emissive triangles exceed the 31-bit triangle index",
                         staticTriangles + dynamicTriangles );
            }
        }
        list.TriangleCount        = static_cast<uint32_t>( staticTriangles );
        list.DynamicTriangleCount = static_cast<uint32_t>( dynamicTriangles );

        // The hit lookup over this frame's TLAS (none without one): instance i's slots, one per geometry index g,
        // name the list row of (its GPU-scene row, its record FirstSegmentRecord + g).
        list.Lookup.assign( instances.size(), kNoEmissiveSegment );
        for ( std::size_t i = 0; i < instances.size(); ++i )
        {
            const RayTracingInstance& instance = instances[i];
            if ( instance.Geometry == nullptr )
                continue;
            const RayTracingGeometryLayout layout =
                 static_cast<const RayTracingGeometryResource*>( instance.Geometry )->GetMetadataLayout();
            for ( uint32_t g = 0; g < layout.Segments.size(); ++g )
            {
                const uint32_t record = instance.FirstSegmentRecord + g;
                if ( record >= segments.size() )
                    return Common::MakeFormattedError<Result>(
                         "EmissiveLights: TLAS instance {} (row {}) names segment record {} of {}", i,
                         instance.CustomIndex, record, segments.size() );
                const auto found = slotOf.find( key( instance.CustomIndex, record ) );
                if ( found == slotOf.end() )
                {
                    // Not emitting, or a grass row (no estimate read: radiance -1). A TLAS segment that emits
                    // while the list holds no row of it would be lit with no light pdf: refused.
                    if ( EmissiveLuminance( radiance[record] ) > 0.0f && layout.Segments[g].PrimitiveCount > 0u )
                        return Common::MakeFormattedError<Result>( "EmissiveLights: TLAS instance {} (row {}) "
                                                                   "segment {} emits and the list holds no row "
                                                                   "of it",
                                                                   i, instance.CustomIndex, g );
                    continue;
                }
                if ( list.Lookup[i] == kNoEmissiveSegment )
                {
                    list.Lookup[i] = static_cast<uint32_t>( list.Lookup.size() );
                    list.Lookup.resize( list.Lookup.size() + layout.Segments.size(), kNoEmissiveSegment );
                }
                list.Lookup[list.Lookup[i] + g] = found->second;
            }
        }

        uint64_t   hash  = 14695981039346656037ull;
        const auto bytes = [&hash]( const std::span<const std::byte> data )
        {
            for ( const std::byte b : data )
                hash = ( hash ^ static_cast<uint64_t>( b ) ) * 1099511628211ull;
        };
        bytes( std::as_bytes( std::span<const EmissiveSegmentGpu>( list.Segments ) ) );
        bytes( std::as_bytes( std::span<const uint32_t>( list.Lookup ) ) );
        list.StaticHash = hash;
        bytes( std::as_bytes( std::span<const EmissiveSegmentGpu>( list.DynamicSegments ) ) );
        list.Hash = hash;
        return Common::MakeSuccess( std::move( list ) );
    }
} // namespace Desert::Graphic::RayTracing
