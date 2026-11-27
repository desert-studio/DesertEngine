#pragma once

// THE RAY-TRACING SCENE'S GPU RECORDS, declared once: a BLAS segment's geometry and material records (the
// ray-query walk's), a hit group's shader record (the pipeline's), and the emissive light list's segment, tree run
// and moved row. Tools/ShaderProtocolGen writes them as Generated/RayTracing/RayTracingRecords.glslh (Common/
// RayQueryVisibility.glslh, Common/RayTracingHitRecord.glslh) and Generated/RayTracing/EmissiveLights.glslh
// (Common/EmissiveLights.glslh, EmissiveLights.shader). Device addresses travel as uvec2 (lo, hi).

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Desert::Graphic::RayTracing
{
    // A record index or a byte offset that names nothing (no material record: opaque; no normal, no UV).
    inline constexpr uint32_t kNoMetadataRecord = ~0u;
    // A segment whose material has no emissive texture (its estimate is the colour x intensity alone).
    inline constexpr uint32_t kNoEmissiveTexture = 0xFFFFFFFFu;

    // One per BLAS segment (RayTracingSceneMetadata.hpp).
#define DESERT_RAY_TRACING_GEOMETRY_RECORD_FIELDS( X )                                                            \
    X( uint64_t, VertexAddress, "VertexAddress", 0 ) /* the buffer the BLAS read positions from */                \
    X( uint64_t, IndexAddress, "IndexAddress", 0 )   /* the index buffer */                                       \
    X( uint32_t, VertexStride, "VertexStride", 0u )  /* layout, as RayTracingGeometrySegment */                   \
    X( uint32_t, FirstVertex, "FirstVertex", 0u )                                                                 \
    X( uint32_t, FirstIndex, "FirstIndex", 0u ) /* in indices (IndexByteOffset / 4) */                            \
    /* RayTracingMaterialRecord row; none = opaque */                                                             \
    X( uint32_t, Material, "MaterialRecord", kNoMetadataRecord )                                                  \
    /* bytes into the vertex; none = the layout has no normal, no UV */                                           \
    X( uint32_t, NormalOffset, "NormalOffset", kNoMetadataRecord )                                                \
    X( uint32_t, UvOffset, "UvOffset", kNoMetadataRecord )                                                        \
    X( uint32_t, SubmeshIndex, "SubmeshIndex", 0u )                                                               \
    X( uint32_t, Pad, "Pad", 0u )
    DESERT_SHADER_STRUCT( RayTracingGeometryRecord, "RayTracingGeometryRecord", Std430,
                          DESERT_RAY_TRACING_GEOMETRY_RECORD_FIELDS );
#undef DESERT_RAY_TRACING_GEOMETRY_RECORD_FIELDS

    // What an alpha test needs, no more.
#define DESERT_RAY_TRACING_MATERIAL_RECORD_FIELDS( X )                                                            \
    X( uint32_t, OpacityTexture, "OpacityTexture", 0u ) /* BindlessTextureSlot of the opacity source */           \
    X( uint32_t, OpacitySampler, "OpacitySampler", 0u ) /* BindlessSamplerSlot */                                 \
    X( float, OpacityCutoff, "OpacityCutoff", 0.5f )                                                              \
    /* bit0 two-sided; bits1-2 channel (0 = A, 1..3 = R/G/B). bit3 = a LANDSCAPE VISIBILITY source                \
       (kMaterialFlagLandscapeVisibility): the texture is a tile's stacked weightmap, the UV the tile-local       \
       sample coordinate, bits4-6 the visibility layer, bit7 = two pages; the test is the raster's hole test.     \
       bit8 = a multi-channel signed distance source (kMaterialFlagSdfMedian). */                                 \
    X( uint32_t, Flags, "Flags", 0u )
    DESERT_SHADER_STRUCT( RayTracingMaterialRecord, "RayTracingMaterialRecord", Std430,
                          DESERT_RAY_TRACING_MATERIAL_RECORD_FIELDS,
                          bool operator==( const RayTracingMaterialRecord& ) const = default; );
#undef DESERT_RAY_TRACING_MATERIAL_RECORD_FIELDS

    // The record a hit stage reads through shaderRecordEXT (Common/RayTracingHitRecord.glslh DesertRayHitRecord).
    // MaterialRow is the row's device address on the GPU; in RayTracingHitGroupTable it is the row's BYTE OFFSET
    // into Rows, which the backend relocates when it uploads them.
#define DESERT_RAY_TRACING_HIT_RECORD_DATA_FIELDS( X )                                                            \
    X( uint64_t, VertexAddress, "VertexAddress", 0 ) /* the buffer the BLAS read positions from */                \
    X( uint64_t, IndexAddress, "IndexAddress", 0 )   /* its uint32 index buffer */                                \
    X( uint32_t, VertexStride, "VertexStride", 0u )                                                               \
    X( uint32_t, FirstVertex, "FirstVertex", 0u )                                                                 \
    X( uint32_t, FirstIndex, "FirstIndex", 0u )                    /* in indices */                               \
    X( uint32_t, NormalOffset, "NormalOffset", kNoMetadataRecord ) /* none: the layout has no normal */           \
    X( uint32_t, UvOffset, "UvOffset", kNoMetadataRecord )         /* none: the layout has no UV */               \
    X( uint32_t, Pad, "Pad", 0u )                                                                                 \
    /* the segment's MaterialParams row (the template's struct) */                                                \
    X( uint64_t, MaterialRow, "MaterialRow", 0 )
    DESERT_SHADER_STRUCT( RayTracingHitRecordData, "RayHitRecordData", Std430,
                          DESERT_RAY_TRACING_HIT_RECORD_DATA_FIELDS );
#undef DESERT_RAY_TRACING_HIT_RECORD_DATA_FIELDS

    // A row's world, row-major 3x4 (RayTracingPrimitive::World).
    using EmissiveObjectToWorldRows = std::array<glm::vec4, 3>;

    // One scene row's emissive segment (EmissiveLights.hpp).
#define DESERT_EMISSIVE_SEGMENT_GPU_FIELDS( X )                                                                   \
    X( EmissiveObjectToWorldRows, ObjectToWorld, "ObjectToWorld", {} )                                            \
    /* rgb: the material's emissive colour x intensity; w: its luminance */                                       \
    X( glm::vec4, Radiance, "Radiance", glm::vec4( 0.0f ) )                                                       \
    /* xy: UV offset, zw: UV tiling (KHR_texture_transform, Common/SurfaceInputs.glslh) */                        \
    X( glm::vec4, UvTransform, "UvTransform", glm::vec4( 0.0f, 0.0f, 1.0f, 1.0f ) )                               \
    /* the positions the BLAS was built from (mesh stream / skin-cache stream) */                                 \
    X( uint64_t, VertexAddress, "VertexAddress", 0 )                                                              \
    X( uint64_t, IndexAddress, "IndexAddress", 0 ) /* uint32 indices */                                           \
    /* the mesh's own vertex stream (Desert::Vertex): the UVs (they do not deform) */                             \
    X( uint64_t, AttributeAddress, "AttributeAddress", 0 )                                                        \
    X( uint32_t, VertexStride, "VertexStride", 0u )                                                               \
    /* bytes from a vertex's start to its position (RayTracingGeometrySegment) */                                 \
    X( uint32_t, PositionOffset, "PositionOffset", 0u )                                                           \
    X( uint32_t, FirstVertex, "FirstVertex", 0u )                                                                 \
    X( uint32_t, FirstIndex, "FirstIndex", 0u )       /* in indices */                                            \
    X( uint32_t, FirstTriangle, "FirstTriangle", 0u ) /* the segment's first row in its part's triangle list */   \
    X( uint32_t, TriangleCount, "TriangleCount", 0u )                                                             \
    X( uint32_t, GpuSceneRow, "GpuSceneRow",                                                                      \
       0u ) /* the row the segment is drawn on (a TLAS hit's custom index) */                                     \
    X( uint32_t, SubmeshIndex, "SubmeshIndex", 0u )       /* the submesh the segment is */                        \
    X( uint32_t, AttributeStride, "AttributeStride", 0u ) /* bytes between two attribute-stream vertices */       \
    X( uint32_t, UvOffset, "UvOffset", 0u )               /* bytes from an attribute vertex's start to its UV */  \
    /* the bindless table slot of the emissive map, its bindless sampler index */                                 \
    X( uint32_t, EmissiveTexture, "EmissiveTexture", kNoEmissiveTexture )                                         \
    X( uint32_t, EmissiveSampler, "EmissiveSampler", 0u )                                                         \
    X( float, UvRotation, "UvRotation", 0.0f ) /* radians (KHR_texture_transform) */                              \
    X( uint32_t, Reserved, "Reserved", 0u )
    DESERT_SHADER_STRUCT( EmissiveSegmentGpu, "EmissiveSegment", Std430, DESERT_EMISSIVE_SEGMENT_GPU_FIELDS );
#undef DESERT_EMISSIVE_SEGMENT_GPU_FIELDS

    // One run of tree nodes a Build or Reduce dispatch writes: its threads [ThreadPrefix, ThreadPrefix + Count)
    // write nodes [First, First + Count) of their level.
#define DESERT_EMISSIVE_NODE_RUN_GPU_FIELDS( X )                                                                  \
    X( uint32_t, First, "First", 0u )                                                                             \
    X( uint32_t, Count, "Count", 0u )                                                                             \
    X( uint32_t, ThreadPrefix, "ThreadPrefix", 0u )                                                               \
    X( uint32_t, Reserved, "Reserved", 0u )
    DESERT_SHADER_STRUCT( EmissiveNodeRunGpu, "EmissiveNodeRun", Std430, DESERT_EMISSIVE_NODE_RUN_GPU_FIELDS );
#undef DESERT_EMISSIVE_NODE_RUN_GPU_FIELDS

    // EmissiveLights.shader "Move": a kept static row whose transform moved, and its new one.
#define DESERT_EMISSIVE_MOVE_GPU_FIELDS( X )                                                                      \
    X( glm::uvec4, Row, "Row", glm::uvec4( 0u ) ) /* x: the static segment row */                                 \
    X( EmissiveObjectToWorldRows, ObjectToWorld, "ObjectToWorld", {} )
    DESERT_SHADER_STRUCT( EmissiveMoveGpu, "EmissiveMove", Std430, DESERT_EMISSIVE_MOVE_GPU_FIELDS );
#undef DESERT_EMISSIVE_MOVE_GPU_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kRayTracingRecordStructs[] = {
         &kRayTracingGeometryRecordStruct,
         &kRayTracingMaterialRecordStruct,
         &kRayTracingHitRecordDataStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kRayTracingRecordsShaderInclude{
         .Path         = "Generated/RayTracing/RayTracingRecords.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/RayTracing/RayTracingLayout.hpp",
         .Structs      = kRayTracingRecordStructs,
    };

    inline constexpr const ShaderProtocols::ShaderStructDecl* kEmissiveLightsStructs[] = {
         &kEmissiveSegmentGpuStruct,
         &kEmissiveNodeRunGpuStruct,
         &kEmissiveMoveGpuStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kEmissiveLightsShaderInclude{
         .Path         = "Generated/RayTracing/EmissiveLights.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/Systems/RayTracing/RayTracingLayout.hpp",
         .Structs      = kEmissiveLightsStructs,
    };
} // namespace Desert::Graphic::RayTracing
