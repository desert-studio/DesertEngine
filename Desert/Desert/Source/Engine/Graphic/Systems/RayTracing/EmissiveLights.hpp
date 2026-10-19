#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// THE SCENE'S EMISSIVE TRIANGLES AS LIGHTS (UE 5.8 Path Tracer emissive mesh light sampling; NVIDIA RTXDI: the
// emissive triangles of the scene in the one light list its samplers draw from). ONE list for every consumer -
// the path tracer's next event estimation now, MegaLights next - built from the same per-segment table the
// TLAS and the hit groups are (RayTracingGatherResult::SegmentRecords, RayTracingInstanceList::Instances): every
// TLAS instance's segment whose material emits becomes an emissive segment, every triangle of it an emissive
// triangle (world positions, area, power), and a power tree over the triangles picks one in proportion to its
// power (EmissiveLightsRenderer.hpp, Common/EmissiveLights.glslh).
//
// Two sides, as the rest of the RT scene:
//   * this file, PURE and device-free: the CPU list of emissive segments (transform, the segment's vertex and
//     index streams, its triangle range in the list, the material's emitted radiance) and the lookup from a hit
//     (TLAS instance, geometry index) to its segment, so a ray that meets an emissive triangle names its row;
//   * EmissiveLightsRenderer.hpp: the graph passes that expand the segments into triangles on the GPU (the
//     vertices live in device buffers only) and reduce their powers into the tree, rebuilt when the list's hash
//     moves and kept across frames otherwise.
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

    // The surface templates' emission parameters (StandardSurface, StandardSurfaceMasked, FoliageSurface
    // `Properties`; their EvaluateSurface emits SurfaceEmission( texel, EmissiveColor, EmissiveIntensity )): what
    // the renderer's EmissiveRadianceFn reads from a segment's effective row (MaterialBinder::ReadRowParam). A
    // template without them has no estimate: its segments are not in the list, and their emission keeps the whole
    // weight.
    inline constexpr std::string_view kSurfaceEmissiveColorParam     = "EmissiveColor";
    inline constexpr std::string_view kSurfaceEmissiveIntensityParam = "EmissiveIntensity";

    // std430, 112 B: Common/EmissiveLights.glslh's EmissiveSegment. One TLAS instance's emissive segment.
    struct EmissiveSegmentGpu
    {
        std::array<glm::vec4, 3> ObjectToWorld{}; // the instance's row-major 3x4 (RayTracingInstance::Transform)
        glm::vec4 Radiance{ 0.0f };   // rgb: the material's emitted radiance estimate; w: its luminance
        uint64_t  VertexAddress  = 0; // the attribute stream the BLAS read positions from (geometry record)
        uint64_t  IndexAddress   = 0; // uint32 indices
        uint32_t  VertexStride   = 0;
        uint32_t  PositionOffset = 0; // bytes from a vertex's start to its position (RayTracingGeometrySegment)
        uint32_t  FirstVertex    = 0;
        uint32_t  FirstIndex     = 0; // in indices
        uint32_t  FirstTriangle  = 0; // the segment's first row in the triangle list
        uint32_t  TriangleCount  = 0;
        uint32_t  Instance       = 0; // the TLAS instance (gl_InstanceID of a hit on it)
        uint32_t  GeometryIndex  = 0; // the segment's geometry index in the instance's BLAS
    };
    static_assert( sizeof( EmissiveSegmentGpu ) == 112 );

    // std430, 48 B: Common/EmissiveLights.glslh's EmissiveTriangle, written by "EmissiveLights: Build".
    inline constexpr uint64_t kEmissiveTriangleBytes = 48;

    // The material's emitted radiance estimate of a segment (zero: the segment does not emit).
    using EmissiveRadianceFn = std::function<glm::vec3( const RayTracingSegmentRecord& )>;

    struct EmissiveTriangleList
    {
        std::vector<EmissiveSegmentGpu> Segments; // ascending FirstTriangle, end to end
        // Indexed by TLAS instance first (InstanceCount entries): the absolute index in Lookup of the instance's
        // per-geometry slots, or kNoEmissiveSegment for an instance with no emissive segment; then the slots,
        // each the index in Segments of that (instance, geometry index) or kNoEmissiveSegment.
        std::vector<uint32_t> Lookup;
        uint32_t              InstanceCount = 0;
        uint32_t              TriangleCount = 0;
        // Emissive segments of a deformed primitive: its BLAS reads skinned positions the attribute stream does
        // not hold, so its triangles cannot be expanded from the geometry record. Not in the list; their emission
        // is met by the material rays alone (full weight). Counted, never silent.
        uint32_t SkippedDeformedSegments = 0;
        // FNV-1a of everything the GPU list is built from (Segments, Lookup): the renderer rebuilds on a change.
        uint64_t Hash = 0;
    };

    [[nodiscard]] inline float EmissiveLuminance( const glm::vec3& c )
    {
        return glm::dot( c, glm::vec3( 0.2126f, 0.7152f, 0.0722f ) );
    }

    // PURE. @p instances are the frame's TLAS rows in TLAS order (RayTracingInstanceList::Instances), @p
    // primitives the gathered primitives (a deformed one is known by its DeformedMesh), @p segments the segment
    // records the instances' FirstSegmentRecord index. Each instance's BLAS layout names its segments
    // (RayTracingGeometryResource::GetMetadataLayout); segment g of instance i is record FirstSegmentRecord + g.
    // A layout that disagrees with the record run (a record past the table) is an error, never a skipped row.
    [[nodiscard]] inline Common::ResultStr<EmissiveTriangleList> BuildEmissiveTriangleList(
         std::span<const RayTracingInstance> instances, std::span<const RayTracingPrimitive> primitives,
         std::span<const RayTracingSegmentRecord> segments, const EmissiveRadianceFn& radianceOf )
    {
        using Result = EmissiveTriangleList;
        Result list;
        list.InstanceCount = static_cast<uint32_t>( instances.size() );
        list.Lookup.assign( instances.size(), kNoEmissiveSegment );

        std::vector<bool> deformedRow;
        for ( const RayTracingPrimitive& primitive : primitives )
        {
            if ( primitive.DeformedMesh == nullptr )
                continue;
            if ( deformedRow.size() <= primitive.GpuSceneRow )
                deformedRow.resize( static_cast<std::size_t>( primitive.GpuSceneRow ) + 1u, false );
            deformedRow[primitive.GpuSceneRow] = true;
        }
        // The estimate of each record once: the instances of one ISM share their mesh's run.
        std::vector<glm::vec3> radiance( segments.size(), glm::vec3( -1.0f ) );

        uint64_t triangles = 0;
        for ( std::size_t i = 0; i < instances.size(); ++i )
        {
            const RayTracingInstance& instance = instances[i];
            if ( instance.Geometry == nullptr )
                continue;
            const RayTracingGeometryLayout layout =
                 static_cast<const RayTracingGeometryResource*>( instance.Geometry )->GetMetadataLayout();
            const bool deformed = instance.CustomIndex < deformedRow.size() && deformedRow[instance.CustomIndex];
            for ( uint32_t g = 0; g < layout.Segments.size(); ++g )
            {
                const std::size_t record = static_cast<std::size_t>( instance.FirstSegmentRecord ) + g;
                if ( record >= segments.size() )
                    return Common::MakeFormattedError<Result>(
                         "EmissiveLights: TLAS instance {} (row {}) names segment record {} of {}", i,
                         instance.CustomIndex, record, segments.size() );
                if ( radiance[record].x < 0.0f )
                    radiance[record] = glm::max( radianceOf( segments[record] ), glm::vec3( 0.0f ) );
                const glm::vec3 emitted = radiance[record];
                if ( EmissiveLuminance( emitted ) <= 0.0f )
                    continue;
                if ( deformed )
                {
                    ++list.SkippedDeformedSegments;
                    continue;
                }
                const RayTracingGeometrySegment& segment = layout.Segments[g];
                if ( segment.PrimitiveCount == 0u )
                    continue;
                if ( list.Lookup[i] == kNoEmissiveSegment )
                {
                    list.Lookup[i] = static_cast<uint32_t>( list.Lookup.size() );
                    list.Lookup.resize( list.Lookup.size() + layout.Segments.size(), kNoEmissiveSegment );
                }
                list.Lookup[list.Lookup[i] + g] = static_cast<uint32_t>( list.Segments.size() );

                EmissiveSegmentGpu& row = list.Segments.emplace_back();
                for ( std::size_t r = 0; r < 3u; ++r )
                    row.ObjectToWorld[r] =
                         glm::vec4( instance.Transform[r * 4u + 0u], instance.Transform[r * 4u + 1u],
                                    instance.Transform[r * 4u + 2u], instance.Transform[r * 4u + 3u] );
                row.Radiance       = glm::vec4( emitted, EmissiveLuminance( emitted ) );
                row.VertexAddress  = layout.VertexAddress;
                row.IndexAddress   = layout.IndexAddress;
                row.VertexStride   = segment.VertexStride;
                row.PositionOffset = segment.PositionOffset;
                row.FirstVertex    = segment.FirstVertex;
                row.FirstIndex     = segment.IndexByteOffset / static_cast<uint32_t>( sizeof( uint32_t ) );
                row.FirstTriangle  = static_cast<uint32_t>( triangles );
                row.TriangleCount  = segment.PrimitiveCount;
                row.Instance       = static_cast<uint32_t>( i );
                row.GeometryIndex  = g;
                triangles += segment.PrimitiveCount;
                // The triangle index and the tree's leaves are 32-bit on the GPU.
                if ( triangles > 0x7FFFFFFFull )
                    return Common::MakeFormattedError<Result>(
                         "EmissiveLights: {} emissive triangles exceed the 31-bit triangle index", triangles );
            }
        }
        list.TriangleCount = static_cast<uint32_t>( triangles );

        uint64_t   hash  = 14695981039346656037ull;
        const auto bytes = [&hash]( const std::span<const std::byte> data )
        {
            for ( const std::byte b : data )
                hash = ( hash ^ static_cast<uint64_t>( b ) ) * 1099511628211ull;
        };
        bytes( std::as_bytes( std::span<const EmissiveSegmentGpu>( list.Segments ) ) );
        bytes( std::as_bytes( std::span<const uint32_t>( list.Lookup ) ) );
        list.Hash = hash;
        return Common::MakeSuccess( std::move( list ) );
    }
} // namespace Desert::Graphic::RayTracing
