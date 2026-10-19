#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// THE RAY-TRACING SCENE METADATA (UE FRayTracingSceneMetadata / the hit-group records of FRayTracingScene): what
// an inline ray query needs to know about the triangle it hit beyond the hardware's answer - the segment's vertex
// and index buffers (by device address) and, for a non-opaque segment, the material's opacity source. It is the
// GPU side of the ONE per-segment table: RayTracingGatherResult::SegmentRecords (RayTracingScene.hpp) is the CPU
// side, and geometry record i IS segment record i. A TLAS instance's SBT record offset is its primitive's
// FirstSegmentRecord, so a shader reads record[ SBT offset + geometry index ]; the instances of one ISM share
// their mesh's run, and so its rows. Shared by RT-SHADOWS (RayQueryVisibility.glslh) and GI-HWRT
// (GITraceRayQuery.glslh). Device-free: the builder is pure.
namespace Desert::Graphic::RayTracing
{
    // std430, 48 B, one per BLAS segment; Common/RayQueryVisibility.glslh mirrors it.
    struct RayTracingGeometryRecord
    {
        uint64_t VertexAddress = 0; // VkDeviceAddress of the buffer the BLAS read positions from
        uint64_t IndexAddress  = 0; // VkDeviceAddress of the index buffer
        uint32_t VertexStride  = 0; // layout, as RayTracingGeometrySegment
        uint32_t FirstVertex   = 0;
        uint32_t FirstIndex    = 0;                 // in indices (IndexByteOffset / 4)
        uint32_t Material      = kNoMetadataRecord; // RayTracingMaterialRecord row; none = opaque
        uint32_t NormalOffset  = kNoMetadataRecord; // bytes into the vertex; none = the layout has no normal
        uint32_t UvOffset      = kNoMetadataRecord; // bytes into the vertex; none = the layout has no UV
        uint32_t SubmeshIndex  = 0;
        uint32_t Pad           = 0;
    };
    static_assert( sizeof( RayTracingGeometryRecord ) == 48 );

    // std430, 16 B: what an alpha test needs, no more.
    struct RayTracingMaterialRecord
    {
        uint32_t OpacityTexture = 0; // BindlessTextureSlot of the opacity source
        uint32_t OpacitySampler = 0; // BindlessSamplerSlot
        float    OpacityCutoff  = 0.5f;
        // bit0 two-sided; bits1-2 channel (0 = A, 1..3 = R/G/B). bit3 = a LANDSCAPE VISIBILITY source
        // (kMaterialFlagLandscapeVisibility): the texture is a tile's stacked weightmap, the UV the tile-local
        // sample coordinate, bits4-6 the visibility layer, bit7 = two pages; the test is the raster's hole test.
        uint32_t Flags = 0;

        bool operator==( const RayTracingMaterialRecord& ) const = default;
    };
    static_assert( sizeof( RayTracingMaterialRecord ) == 16 );

    inline constexpr uint32_t kMaterialFlagTwoSided = 1u << 0;
    inline constexpr uint32_t kMaterialChannelShift = 1u;
    inline constexpr uint32_t kMaterialChannelAlpha = 0u;
    inline constexpr uint32_t kMaterialChannelRed   = 1u;

    // A landscape tile's holes as the alpha test of its masked segment (UE: the landscape material's any-hit
    // shader on its Visibility Mask, inline): Common/RayQueryVisibility.glslh reads the visibility layer's weight
    // from page layer / 4, channel layer % 4 of the stacked weightmap (Common/LandscapeWeights.glslh
    // LandscapeWeightmapPageUV) and blocks unless LandscapeIsHole - the raster's own threshold, so OpacityCutoff
    // is unused.
    inline constexpr uint32_t kMaterialFlagLandscapeVisibility = 1u << 3;
    inline constexpr uint32_t kMaterialLandscapeLayerShift     = 4u;
    inline constexpr uint32_t kMaterialFlagLandscapeTwoPages   = 1u << 7;

    // @p visibilityLayer: the tile's visibility layer (0..7, LandscapeWeightDraw::VisibilityLayer); @p pages: the
    // weightmap's page count (1 or 2). The ground is two-sided: a hole is open from below as from above.
    [[nodiscard]] constexpr RayTracingMaterialRecord LandscapeVisibilityRecord( const uint32_t weightmapSlot,
                                                                                const uint32_t samplerSlot,
                                                                                const uint32_t visibilityLayer,
                                                                                const uint32_t pages )
    {
        return { .OpacityTexture = weightmapSlot,
                 .OpacitySampler = samplerSlot,
                 .OpacityCutoff  = 0.0f,
                 .Flags          = kMaterialFlagTwoSided | kMaterialFlagLandscapeVisibility |
                          ( ( visibilityLayer & 7u ) << kMaterialLandscapeLayerShift ) |
                          ( pages > 1u ? kMaterialFlagLandscapeTwoPages : 0u ) };
    }

    struct RayTracingMetadata
    {
        std::vector<RayTracingGeometryRecord> Geometry;
        std::vector<RayTracingMaterialRecord> Materials;
    };

    // The alpha-test record of a non-opaque segment, from its segment record (the section's material instance and
    // surface template, or a landscape tile's segment); nullopt = no opacity source is known for that material.
    using RayTracingSegmentMaterialFn =
         std::function<std::optional<RayTracingMaterialRecord>( const RayTracingSegmentRecord& segment )>;

    struct RayTracingMetadataBuild
    {
        RayTracingMetadata Metadata;
        // Non-opaque segment records materialOf had no opacity source for: their row's Material is
        // kNoMetadataRecord, so a ray query confirms every candidate of theirs as a hit (RayQueryCandidateBlocks).
        // Counted, never silent: RayTracingSceneStats::SegmentsWithoutOpacity.
        uint32_t SegmentsWithoutOpacity = 0;
    };

    // PURE. One geometry record per segment record, in the same order (record i = @p segments[i]): every primitive
    // with a BLAS fills its run's rows from that BLAS's layout. The BLAS and the run must agree segment by segment
    // - the same count, submesh and Opaque - or the BLAS was built for other sections, and that is an error, never
    // a silent pass. A run shared by several primitives (an ISM) is filled once and must be shared with the same
    // BLAS. Rows of a run no primitive traces (its BLAS is missing) stay empty; no TLAS instance points at them.
    [[nodiscard]] inline Common::ResultStr<RayTracingMetadataBuild>
    BuildRayTracingMetadata( std::span<const RayTracingPrimitive>     primitives,
                             std::span<const RayTracingSegmentRecord> segments,
                             const RayTracingSegmentMaterialFn&       materialOf )
    {
        using Result = RayTracingMetadataBuild;
        // The SBT record offset is 24 bits in the TLAS row.
        if ( segments.size() > static_cast<std::size_t>( kMaxSegmentRecord ) + 1u )
            return Common::MakeFormattedError<Result>(
                 "RayTracingMetadata: {} segment records exceed the 24-bit record offset", segments.size() );
        Result build;
        build.Metadata.Geometry.resize( segments.size() );
        std::vector<const void*> filledBy( segments.size(), nullptr );
        for ( const RayTracingPrimitive& primitive : primitives )
        {
            if ( primitive.Geometry == nullptr || primitive.SegmentRecordCount == 0 )
                continue;
            if ( static_cast<std::size_t>( primitive.FirstSegmentRecord ) + primitive.SegmentRecordCount >
                 segments.size() )
                return Common::MakeFormattedError<Result>(
                     "RayTracingMetadata: GPU-scene row {} names segment records [{}, +{}) of {}",
                     primitive.GpuSceneRow, primitive.FirstSegmentRecord, primitive.SegmentRecordCount,
                     segments.size() );
            if ( const void* owner = filledBy[primitive.FirstSegmentRecord]; owner != nullptr )
            {
                if ( owner != primitive.Geometry )
                    return Common::MakeFormattedError<Result>(
                         "RayTracingMetadata: GPU-scene row {} shares segment record {} with a different BLAS",
                         primitive.GpuSceneRow, primitive.FirstSegmentRecord );
                continue;
            }
            const RayTracingGeometryLayout layout =
                 static_cast<const RayTracingGeometryResource*>( primitive.Geometry )->GetMetadataLayout();
            if ( layout.Segments.size() != primitive.SegmentRecordCount )
                return Common::MakeFormattedError<Result>(
                     "RayTracingMetadata: GPU-scene row {}: the BLAS has {} segments, its run {} records",
                     primitive.GpuSceneRow, layout.Segments.size(), primitive.SegmentRecordCount );
            for ( uint32_t i = 0; i < primitive.SegmentRecordCount; ++i )
            {
                const uint32_t                   index   = primitive.FirstSegmentRecord + i;
                const RayTracingGeometrySegment& segment = layout.Segments[i];
                const RayTracingSegmentRecord&   source  = segments[index];
                if ( segment.SubmeshIndex != source.SubmeshIndex || segment.Opaque != source.Opaque )
                    return Common::MakeFormattedError<Result>(
                         "RayTracingMetadata: GPU-scene row {} segment {}: the BLAS is submesh {} ({}), the "
                         "record submesh {} ({})",
                         primitive.GpuSceneRow, i, segment.SubmeshIndex, segment.Opaque ? "opaque" : "alpha-tested",
                         source.SubmeshIndex, source.Opaque ? "opaque" : "alpha-tested" );
                filledBy[index]                  = primitive.Geometry;
                RayTracingGeometryRecord& record = build.Metadata.Geometry[index];
                record.VertexAddress             = layout.VertexAddress;
                record.IndexAddress              = layout.IndexAddress;
                record.VertexStride              = layout.VertexStride;
                record.FirstVertex               = segment.FirstVertex;
                record.FirstIndex   = segment.IndexByteOffset / static_cast<uint32_t>( sizeof( uint32_t ) );
                record.NormalOffset = layout.NormalOffset;
                record.UvOffset     = layout.UvOffset;
                record.SubmeshIndex = segment.SubmeshIndex;
                if ( source.Opaque )
                    continue;
                const std::optional<RayTracingMaterialRecord> material = materialOf( source );
                if ( !material )
                {
                    ++build.SegmentsWithoutOpacity;
                    continue;
                }
                std::vector<RayTracingMaterialRecord>& materials = build.Metadata.Materials;
                const auto found = std::find( materials.begin(), materials.end(), *material );
                record.Material  = static_cast<uint32_t>( found - materials.begin() );
                if ( found == materials.end() )
                    materials.push_back( *material );
            }
        }
        return Common::MakeSuccess( std::move( build ) );
    }

    inline constexpr std::string_view kMetadataGeometryName  = "RayTracingScene.GeometryRecords";
    inline constexpr std::string_view kMetadataMaterialsName = "RayTracingScene.MaterialRecords";

    // This frame's metadata as graph buffers (StorageRead by a ray-query pass). Refs valid only for that graph.
    struct RayTracingMetadataFrame
    {
        RDG::BufferRef Geometry;
        RDG::BufferRef Materials;
    };

    // Uploads @p metadata into two graph buffers (at least one row each: a graph buffer cannot be empty, and a
    // scene with no masked segment reads no material row).
    template <typename Row>
    [[nodiscard]] std::vector<std::byte> EncodeMetadataRows( std::span<const Row> rows )
    {
        std::vector<std::byte> bytes( ( rows.empty() ? 1u : rows.size() ) * sizeof( Row ) );
        if ( !rows.empty() )
            std::memcpy( bytes.data(), rows.data(), rows.size() * sizeof( Row ) );
        return bytes;
    }

    [[nodiscard]] inline RayTracingMetadataFrame AddRayTracingMetadataUpload( RDG::Builder&             graph,
                                                                              const RayTracingMetadata& metadata )
    {
        RayTracingMetadataFrame      frame;
        const std::vector<std::byte> geometry =
             EncodeMetadataRows( std::span<const RayTracingGeometryRecord>( metadata.Geometry ) );
        const std::vector<std::byte> materials =
             EncodeMetadataRows( std::span<const RayTracingMaterialRecord>( metadata.Materials ) );
        frame.Geometry  = graph.CreateBuffer( RDG::BufferDesc{ geometry.size() }, kMetadataGeometryName );
        frame.Materials = graph.CreateBuffer( RDG::BufferDesc{ materials.size() }, kMetadataMaterialsName );
        graph.QueueBufferUpload( frame.Geometry, geometry );
        graph.QueueBufferUpload( frame.Materials, materials );
        return frame;
    }
} // namespace Desert::Graphic::RayTracing
