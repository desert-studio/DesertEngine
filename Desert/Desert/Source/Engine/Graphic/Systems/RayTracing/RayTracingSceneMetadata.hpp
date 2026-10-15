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
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

// THE RAY-TRACING SCENE METADATA (UE FRayTracingSceneMetadata / the hit-group records of FRayTracingScene): what
// an inline ray query needs to know about the triangle it hit beyond the hardware's answer - the segment's vertex
// and index buffers (by device address) and, for a masked segment, the material's opacity source. One geometry
// record per BLAS segment, laid out per instance at RayTracingInstance::GeometryRecordOffset, which the TLAS row
// carries as instanceShaderBindingTableRecordOffset: a shader reads record[ SBT offset + geometry index ].
// Instances with the same BLAS and the same material records (every instance of one ISM row) share one range.
// Shared by RT-SHADOWS (RayQueryVisibility.glslh) and GI-HWRT. Device-free: the builder is pure.
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
        uint32_t Flags          = 0; // bit0 two-sided; bits1-2 channel (0 = A, 1..3 = R/G/B)

        bool operator==( const RayTracingMaterialRecord& ) const = default;
    };
    static_assert( sizeof( RayTracingMaterialRecord ) == 16 );

    inline constexpr uint32_t kMaterialFlagTwoSided = 1u << 0;
    inline constexpr uint32_t kMaterialChannelShift = 1u;
    inline constexpr uint32_t kMaterialChannelAlpha = 0u;
    inline constexpr uint32_t kMaterialChannelRed   = 1u;

    struct RayTracingMetadata
    {
        std::vector<RayTracingGeometryRecord> Geometry;
        std::vector<RayTracingMaterialRecord> Materials;
    };

    // The alpha-test record of submesh @p submesh of @p primitive; nullopt = the submesh's material is not masked.
    using RayTracingSubmeshMaterialFn = std::function<std::optional<RayTracingMaterialRecord>(
         const RayTracingPrimitive& primitive, uint32_t submesh )>;

    // PURE. Fills every instance's GeometryRecordOffset and answers the rows, in TLAS order. A segment's Masked
    // flag (its BLAS geometry is not OPAQUE) and its material record must agree: a masked segment with no record
    // would be alpha-tested against nothing, an opaque segment with one would never be tested - both are a BLAS
    // built for other materials, and both are errors, never a silent pass.
    [[nodiscard]] inline Common::ResultStr<RayTracingMetadata> BuildRayTracingMetadata(
         std::span<RayTracingInstance> instances, std::span<const RayTracingPrimitive> primitives,
         const RayTracingSubmeshMaterialFn& materialOf )
    {
        RayTracingMetadata metadata;
        // (geometry, material rows) -> first record: instances that agree on both share one range.
        std::map<std::tuple<const void*, std::vector<uint32_t>>, uint32_t> ranges;
        for ( RayTracingInstance& instance : instances )
        {
            if ( instance.Primitive >= primitives.size() )
                return Common::MakeFormattedError<RayTracingMetadata>(
                     "RayTracingMetadata: instance names primitive {} of {}", instance.Primitive,
                     primitives.size() );
            const RayTracingPrimitive&                    primitive = primitives[instance.Primitive];
            if ( instance.Geometry == nullptr )
                return Common::MakeFormattedError<RayTracingMetadata>(
                     "RayTracingMetadata: the instance of GPU-scene row {} has no BLAS", primitive.GpuSceneRow );
            const RayTracingGeometryLayout layout =
                 static_cast<const RayTracingGeometryResource*>( instance.Geometry )->GetMetadataLayout();

            std::vector<uint32_t> materialRows;
            materialRows.reserve( layout.Segments.size() );
            for ( const RayTracingGeometrySegment& segment : layout.Segments )
            {
                const std::optional<RayTracingMaterialRecord> material =
                     materialOf( primitive, segment.SubmeshIndex );
                if ( material.has_value() != segment.Masked )
                    return Common::MakeFormattedError<RayTracingMetadata>(
                         "RayTracingMetadata: GPU-scene row {} submesh {}: the BLAS segment is {} but the "
                         "material "
                         "is {}",
                         primitive.GpuSceneRow, segment.SubmeshIndex, segment.Masked ? "masked" : "opaque",
                         material ? "masked" : "opaque" );
                if ( !material )
                {
                    materialRows.push_back( kNoMetadataRecord );
                    continue;
                }
                const auto found = std::find( metadata.Materials.begin(), metadata.Materials.end(), *material );
                materialRows.push_back( static_cast<uint32_t>( found - metadata.Materials.begin() ) );
                if ( found == metadata.Materials.end() )
                    metadata.Materials.push_back( *material );
            }

            const auto key = std::make_tuple( instance.Geometry, materialRows );
            if ( const auto shared = ranges.find( key ); shared != ranges.end() )
            {
                instance.GeometryRecordOffset = shared->second;
                continue;
            }
            const uint32_t offset         = static_cast<uint32_t>( metadata.Geometry.size() );
            instance.GeometryRecordOffset = offset;
            ranges.emplace( key, offset );
            for ( std::size_t i = 0; i < layout.Segments.size(); ++i )
            {
                const RayTracingGeometrySegment& segment = layout.Segments[i];
                RayTracingGeometryRecord&        record  = metadata.Geometry.emplace_back();
                record.VertexAddress                     = layout.VertexAddress;
                record.IndexAddress                      = layout.IndexAddress;
                record.VertexStride                      = layout.VertexStride;
                record.FirstVertex                       = segment.FirstVertex;
                record.FirstIndex   = segment.IndexByteOffset / static_cast<uint32_t>( sizeof( uint32_t ) );
                record.Material     = materialRows[i];
                record.NormalOffset = layout.NormalOffset;
                record.UvOffset     = layout.UvOffset;
                record.SubmeshIndex = segment.SubmeshIndex;
            }
        }
        // The SBT record offset is 24 bits in the TLAS row.
        if ( metadata.Geometry.size() > kMaxGpuSceneRow )
            return Common::MakeFormattedError<RayTracingMetadata>(
                 "RayTracingMetadata: {} geometry records exceed the 24-bit record offset",
                 metadata.Geometry.size() );
        return Common::MakeSuccess( std::move( metadata ) );
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
