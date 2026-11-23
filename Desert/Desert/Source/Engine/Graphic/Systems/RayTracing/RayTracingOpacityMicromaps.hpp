#pragma once

#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneMetadata.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

// RT-OMM-SER: WHICH SEGMENTS GET AN OPACITY MICROMAP (UE 5.4 r.RayTracing.OpacityMicromaps: baked once per masked
// section on the GPU, attached to its BLAS segment). A candidate is a non-opaque segment of a traced BLAS whose
// metadata row names a material record with a TEXEL alpha test - the only test the bake can classify exactly
// (OpacityMicromapBake.shader). A landscape visibility source or a signed-distance glyph is another test and is
// never a candidate: those segments keep reaching the ray-query alpha test for every candidate, as before. The
// owner of the BLAS (RayTracingGeometryVariants: the only cache whose BLAS carries non-opaque segments of a static
// mesh) decides what to bake; this is the pure part, checked without a device.
namespace Desert::Graphic::RayTracing
{
    struct OpacityMicromapCandidate
    {
        const RayTracingGeometryResource* Geometry      = nullptr; // the BLAS the segment belongs to
        uint32_t                          Segment       = 0;       // index into its RayTracingMeshInput::Segments
        uint32_t                          TriangleCount = 0;
        RayTracingGeometryRecord          Record;   // where the bake reads positions / UVs (metadata row)
        RayTracingMaterialRecord          Material; // the alpha test the bake classifies against
    };

    // One segment's bake for this frame's graph: the candidate and the buffers the bake writes (its output, by
    // device address).
    struct OpacityMicromapBake
    {
        OpacityMicromapCandidate         Source;
        RayTracingSegmentOpacityMicromap Output;
    };

    inline constexpr uint32_t kMaterialFlagsNotATexelTest =
         kMaterialFlagLandscapeVisibility | kMaterialFlagSdfMedian;

    // PURE. Every non-opaque segment of @p primitives' BLASes whose row in @p metadata has a texel alpha test,
    // once per (BLAS, segment) - an ISM run shared by many primitives is one candidate. Rows with no material
    // record (SegmentsWithoutOpacity) or with no UV stream are not candidates.
    [[nodiscard]] inline std::vector<OpacityMicromapCandidate>
    CollectOpacityMicromapCandidates( std::span<const RayTracingPrimitive> primitives,
                                      const RayTracingMetadata&            metadata )
    {
        std::vector<OpacityMicromapCandidate> candidates;
        // An ISM's instances share one run: visited once.
        std::vector<bool> runVisited( metadata.Geometry.size(), false );
        for ( const RayTracingPrimitive& primitive : primitives )
        {
            if ( primitive.Geometry == nullptr || primitive.SegmentRecordCount == 0 ||
                 primitive.FirstSegmentRecord >= runVisited.size() || runVisited[primitive.FirstSegmentRecord] )
                continue;
            runVisited[primitive.FirstSegmentRecord] = true;
            const auto* geometry = static_cast<const RayTracingGeometryResource*>( primitive.Geometry );
            const RayTracingGeometryLayout layout = geometry->GetMetadataLayout();
            if ( layout.Segments.size() != primitive.SegmentRecordCount )
                continue; // BuildRayTracingMetadata already refused such a run
            for ( uint32_t i = 0; i < primitive.SegmentRecordCount; ++i )
            {
                const RayTracingGeometrySegment& segment = layout.Segments[i];
                const std::size_t                row     = std::size_t{ primitive.FirstSegmentRecord } + i;
                if ( segment.Opaque || segment.PrimitiveCount == 0 || row >= metadata.Geometry.size() )
                    continue;
                const RayTracingGeometryRecord& record = metadata.Geometry[row];
                if ( record.Material == kNoMetadataRecord || record.Material >= metadata.Materials.size() ||
                     record.UvOffset == kNoMetadataRecord )
                    continue;
                const RayTracingMaterialRecord& material = metadata.Materials[record.Material];
                if ( ( material.Flags & kMaterialFlagsNotATexelTest ) != 0u )
                    continue;
                candidates.push_back( { geometry, i, segment.PrimitiveCount, record, material } );
            }
        }
        // Two runs over one BLAS (two primitives of one variant) name the same segments: one bake each.
        const auto byKey = []( const OpacityMicromapCandidate& a, const OpacityMicromapCandidate& b )
        {
            return a.Geometry != b.Geometry ? std::less<const void*>{}( a.Geometry, b.Geometry )
                                            : a.Segment < b.Segment;
        };
        std::stable_sort( candidates.begin(), candidates.end(), byKey );
        candidates.erase( std::unique( candidates.begin(), candidates.end(),
                                       []( const OpacityMicromapCandidate& a, const OpacityMicromapCandidate& b )
                                       { return a.Geometry == b.Geometry && a.Segment == b.Segment; } ),
                          candidates.end() );
        return candidates;
    }
} // namespace Desert::Graphic::RayTracing
