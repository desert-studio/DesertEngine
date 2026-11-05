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
        // bit8 = a multi-channel signed distance source (kMaterialFlagSdfMedian).
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

    // A MULTI-CHANNEL SIGNED DISTANCE source (3D text, TextRayTracing.hpp TextRayHit): the opacity is the median
    // of the texel's R, G, B (Common/SdfText.glslh SdfTextMedian - the raster's own coverage, 0.5 on the outline)
    // against OpacityCutoff; the channel bits are unused.
    inline constexpr uint32_t kMaterialFlagSdfMedian = 1u << 8;

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

    // Fills the rows of @p primitive's run (it has a BLAS and a run inside @p segments) in @p metadata from its
    // BLAS's layout - BuildRayTracingMetadata's step per run: the BLAS and the run must agree segment by segment.
    // The rows are written over, never reset: a caller patching a run resets it first. @p withoutOpacity counts
    // the run's non-opaque rows materialOf had no opacity source for.
    [[nodiscard]] inline Common::BoolResultStr
    FillRayTracingMetadataRun( RayTracingMetadata& metadata, const RayTracingPrimitive& primitive,
                               std::span<const RayTracingSegmentRecord> segments,
                               const RayTracingSegmentMaterialFn& materialOf, uint32_t& withoutOpacity )
    {
        const RayTracingGeometryLayout layout =
             static_cast<const RayTracingGeometryResource*>( primitive.Geometry )->GetMetadataLayout();
        if ( layout.Segments.size() != primitive.SegmentRecordCount )
            return Common::MakeFormattedError<bool>(
                 "RayTracingMetadata: GPU-scene row {}: the BLAS has {} segments, its run {} records",
                 primitive.GpuSceneRow, layout.Segments.size(), primitive.SegmentRecordCount );
        for ( uint32_t i = 0; i < primitive.SegmentRecordCount; ++i )
        {
            const uint32_t                   index   = primitive.FirstSegmentRecord + i;
            const RayTracingGeometrySegment& segment = layout.Segments[i];
            const RayTracingSegmentRecord&   source  = segments[index];
            if ( segment.SubmeshIndex != source.SubmeshIndex || segment.Opaque != source.Opaque )
                return Common::MakeFormattedError<bool>(
                     "RayTracingMetadata: GPU-scene row {} segment {}: the BLAS is submesh {} ({}), the "
                     "record submesh {} ({})",
                     primitive.GpuSceneRow, i, segment.SubmeshIndex, segment.Opaque ? "opaque" : "alpha-tested",
                     source.SubmeshIndex, source.Opaque ? "opaque" : "alpha-tested" );
            RayTracingGeometryRecord& record = metadata.Geometry[index];
            record.VertexAddress             = layout.VertexAddress;
            record.IndexAddress              = layout.IndexAddress;
            record.VertexStride              = layout.VertexStride;
            record.FirstVertex               = segment.FirstVertex;
            record.FirstIndex =
                 segment.IndexByteOffset / static_cast<uint32_t>( sizeof( uint32_t ) );
            record.NormalOffset              = layout.NormalOffset;
            record.UvOffset                  = layout.UvOffset;
            record.SubmeshIndex              = segment.SubmeshIndex;
            if ( source.Opaque )
                continue;
            const std::optional<RayTracingMaterialRecord> material = materialOf( source );
            if ( !material )
            {
                ++withoutOpacity;
                continue;
            }
            std::vector<RayTracingMaterialRecord>& materials = metadata.Materials;
            const auto found = std::find( materials.begin(), materials.end(), *material );
            record.Material  = static_cast<uint32_t>( found - materials.begin() );
            if ( found == materials.end() )
                materials.push_back( *material );
        }
        return Common::MakeSuccess( true );
    }

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
            if ( const Common::BoolResultStr filled = FillRayTracingMetadataRun(
                      build.Metadata, primitive, segments, materialOf, build.SegmentsWithoutOpacity );
                 !filled )
                return Common::MakeError<Result>( filled.GetError() );
            std::fill_n( filledBy.begin() + primitive.FirstSegmentRecord, primitive.SegmentRecordCount,
                         primitive.Geometry );
        }
        return Common::MakeSuccess( std::move( build ) );
    }

    // PURE. BuildRayTracingMetadata again for the @p patched primitives only (indices into @p primitives), into
    // the @p build made from the same list (a kept list: every primitive traces the BLAS it traced, over the same
    // segment records and opacity sources; only a layout's buffers may have moved - a per-frame BLAS, a landscape
    // tile built again). Each patched run is reset and filled anew from its BLAS; @p dirty gets every row whose
    // bytes changed; a material record not yet in Materials is appended (Materials only grows until the next
    // build); SegmentsWithoutOpacity follows the refilled rows. Errors as BuildRayTracingMetadata's, and @p build
    // is then partly patched: the caller builds it again.
    [[nodiscard]] inline Common::BoolResultStr
    PatchRayTracingMetadata( RayTracingMetadataBuild& build, std::span<const RayTracingPrimitive> primitives,
                             std::span<const RayTracingSegmentRecord> segments, std::span<const uint32_t> patched,
                             const RayTracingSegmentMaterialFn& materialOf, std::vector<uint32_t>& dirty )
    {
        std::vector<RayTracingGeometryRecord>& rows = build.Metadata.Geometry;
        if ( rows.size() != segments.size() )
            return Common::MakeFormattedError<bool>(
                 "RayTracingMetadata: a patch over {} segment records of a build made for {}", segments.size(),
                 rows.size() );
        std::vector<RayTracingGeometryRecord> before;
        for ( const uint32_t p : patched )
        {
            if ( p >= primitives.size() )
                return Common::MakeFormattedError<bool>( "RayTracingMetadata: patched primitive {} of {}", p,
                                                         primitives.size() );
            const RayTracingPrimitive& primitive = primitives[p];
            if ( primitive.Geometry == nullptr || primitive.SegmentRecordCount == 0 )
                continue;
            const std::size_t first = primitive.FirstSegmentRecord;
            const std::size_t end   = first + primitive.SegmentRecordCount;
            if ( end > rows.size() )
                return Common::MakeFormattedError<bool>(
                     "RayTracingMetadata: GPU-scene row {} names segment records [{}, +{}) of {}",
                     primitive.GpuSceneRow, first, primitive.SegmentRecordCount, rows.size() );
            // The run's rows as the build left them, and its share of SegmentsWithoutOpacity: a non-opaque row
            // with no material row is one (an untraced run is never patched: a primitive gaining its BLAS
            // relists).
            uint32_t withoutBefore = 0;
            for ( std::size_t i = first; i < end; ++i )
                if ( !segments[i].Opaque && rows[i].Material == kNoMetadataRecord )
                    ++withoutBefore;
            before.assign( rows.begin() + static_cast<std::ptrdiff_t>( first ),
                           rows.begin() + static_cast<std::ptrdiff_t>( end ) );
            std::fill( rows.begin() + static_cast<std::ptrdiff_t>( first ),
                       rows.begin() + static_cast<std::ptrdiff_t>( end ), RayTracingGeometryRecord{} );
            uint32_t withoutAfter = 0;
            if ( const Common::BoolResultStr filled =
                      FillRayTracingMetadataRun( build.Metadata, primitive, segments, materialOf, withoutAfter );
                 !filled )
                return filled;
            if ( build.SegmentsWithoutOpacity < withoutBefore )
                return Common::MakeFormattedError<bool>(
                     "RayTracingMetadata: GPU-scene row {} held {} rows without opacity, the build counted {}",
                     primitive.GpuSceneRow, withoutBefore, build.SegmentsWithoutOpacity );
            build.SegmentsWithoutOpacity = build.SegmentsWithoutOpacity - withoutBefore + withoutAfter;
            for ( std::size_t i = first; i < end; ++i )
                if ( std::memcmp( &rows[i], &before[i - first], sizeof( RayTracingGeometryRecord ) ) != 0 )
                    dirty.push_back( static_cast<uint32_t>( i ) );
        }
        return Common::MakeSuccess( true );
    }

    inline constexpr std::string_view kMetadataGeometryName  = "RayTracingScene.GeometryRecords";
    inline constexpr std::string_view kMetadataMaterialsName = "RayTracingScene.MaterialRecords";

    // This frame's metadata as graph buffers (StorageRead by a ray-query pass): the view's persistent rows
    // (AddRayTracingMetadataUpload, RayTracingSceneRenderer.hpp). Refs valid only for that graph.
    struct RayTracingMetadataFrame
    {
        RDG::BufferRef Geometry;
        RDG::BufferRef Materials;
    };
} // namespace Desert::Graphic::RayTracing
