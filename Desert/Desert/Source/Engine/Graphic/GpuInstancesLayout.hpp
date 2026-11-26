#pragma once

// GPU-GENERATED INSTANCES, the std430 rows the shaders read (GpuInstances.hpp owns the buffers). Each row is
// declared once here; Generated/GpuInstances/GpuInstances.glslh is generated from it at build time
// (kGpuInstancesShaderInclude below): Common/GpuInstanceDrawRow.glslh and Programs/Grass/GrassDrawCompact.shader
// include it.

#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/vec4.hpp>

#include <cstdint>

namespace Desert::Graphic
{
    // ONE DRAW ROW (Common/GpuInstanceDrawRow.glslh): what the per-variety
    // path bound or pushed per section (MeshRendererGpuInstanced.cpp bindRecord's slots b17 / b18 / b13 and the
    // mesh visibility rows, addBuckets' MaterialIndex / Wind / PrimitiveFlags), by device address or by value.
    // Written by the producer when a word moves (a buffer re-created, the material row re-claimed, the wind
    // changed), per frame-in-flight copy, never per draw.
#define DESERT_GPU_INSTANCE_DRAW_ROW_FIELDS( X ) \
    X( uint64_t, Instances, "Instances", 0 ) /* mat4 per instance (InstanceTransforms) */ \
    X( uint64_t, Ids, "Ids", 0 ) /* the list's ids into Instances (GpuInstanceIds); 0: the slot is the row */ \
    X( uint64_t, BladeShape, "BladeShape", 0 ) /* the variety's GrassBladeShape; 0: not a blade */ \
    /* GrassMeshVisibilityGpu rows; 0: not drawn through the visibility buffer */ \
    X( uint64_t, MeshVisibility, "MeshVisibility", 0 ) \
    /* the scene material table's row (MeshRenderer::FindMaterialRow) */ \
    X( uint32_t, MaterialRow, "MaterialRow", 0 ) \
    X( uint32_t, PrimitiveFlags, "PrimitiveFlags", 0 ) \
    X( uint32_t, Reserved0, "Reserved0", 0 ) \
    X( uint32_t, Reserved1, "Reserved1", 0 ) \
    X( glm::vec4, WindA, "WindA", glm::vec4( 0.0f ) ) /* PackViewInstanceWind( Wind, PrevInstanceOffset ).A */ \
    X( glm::vec4, WindB, "WindB", glm::vec4( 0.0f ) ) /* .B */ \
    /* GRASS-SHADOW-BINS-1, a cascade caster's (the per-section path's light camera words that are the \
       section's, not the cascade's): xyz the origin the cascade thins the blades from (CasterWindOrigin, or a \
       still section's ThinOrigin), w the caster ring's inner distance (ShadowWindInnerOf; CasterWindRing.x). */ \
    X( glm::vec4, Caster, "Caster", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( GpuInstanceDrawRow, "GpuInstanceDrawRow", Std430, DESERT_GPU_INSTANCE_DRAW_ROW_FIELDS );
#undef DESERT_GPU_INSTANCE_DRAW_ROW_FIELDS

    // One gather entry (Programs/Grass/GrassDrawCompact.shader): Count records
    // from Records at Stride bytes apart (a strip's level records: kGpuInstanceArgsRecordBytes; a mesh's LOD
    // groups: a group's bytes), copied to slots FirstSlot .. FirstSlot + Count - 1 of bin Bin, and Count added to
    // the bin's drawn count. The CPU lays the entries of a bin out end to end in the per-variety path's draw
    // order, so a bin draws its records in that order (the picture stays the same); an empty record is an empty
    // draw of the GPU's front end.
#define DESERT_GPU_DRAW_COMPACT_ENTRY_FIELDS( X ) \
    X( uint64_t, Records, "Records", 0 ) /* device address of the run's first VkDrawIndexedIndirectCommand */ \
    X( uint64_t, DrawRow, "DrawRow", 0 ) /* device address of the GpuInstanceDrawRow its draws read */ \
    X( uint32_t, Count, "Count", 0 ) \
    X( uint32_t, Stride, "Stride", 0 ) \
    X( uint32_t, Bin, "Bin", 0 ) \
    X( uint32_t, FirstSlot, "FirstSlot", 0 )
    DESERT_SHADER_STRUCT( GpuDrawCompactEntry, "GpuDrawCompactEntry", Std430,
                          DESERT_GPU_DRAW_COMPACT_ENTRY_FIELDS );
#undef DESERT_GPU_DRAW_COMPACT_ENTRY_FIELDS

    inline constexpr const ShaderProtocols::ShaderStructDecl* kGpuInstancesStructs[] = {
        &kGpuInstanceDrawRowStruct,
        &kGpuDrawCompactEntryStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kGpuInstancesShaderInclude{
        .Path         = "Generated/GpuInstances/GpuInstances.glslh",
        .SourceHeader = "Desert/Desert/Source/Engine/Graphic/GpuInstancesLayout.hpp",
        .Structs      = kGpuInstancesStructs,
    };
} // namespace Desert::Graphic
