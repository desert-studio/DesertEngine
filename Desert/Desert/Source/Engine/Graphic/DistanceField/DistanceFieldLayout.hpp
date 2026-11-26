#pragma once

// THE DISTANCE FIELD'S GPU RECORDS, declared once: the near field's froxel grid placement and its cull record, and
// the far field's ceiling (GlobalDistanceFieldCeiling.hpp). Tools/ShaderProtocolGen writes them as
// Generated/DistanceField/DistanceField.glslh, which Common/MeshSDFGrid.glslh and Common/GITracer.glslh include.

#include <Engine/Graphic/DistanceField/GlobalDistanceFieldCeiling.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Desert::Graphic::DistanceField
{
    // The froxel grid's placement (MeshDistanceFieldReads::NearFieldGridPlacement).
#define DESERT_MESH_SDF_GRID_PLACEMENT_FIELDS( X )                                                                \
    X( glm::mat4, WorldToClip, "worldToClip", glm::mat4( 1.0f ) ) /* the view's unjittered view-projection */     \
    /* cells across, cells down, depth slices, cells in all */                                                    \
    X( glm::uvec4, Cells, "cells", glm::uvec4( 0u ) )                                                             \
    /* log z scale (1/cm), log z offset, z scale, pixels per cell (DF_MESH_SDF_GRID_*) */                         \
    X( glm::vec4, ZDistribution, "zDistribution", glm::vec4( 0.0f ) )                                             \
    X( glm::vec4, Screen, "screen", glm::vec4( 0.0f ) ) /* the view's extent in pixels, 1 / that */
    DESERT_SHADER_STRUCT( MeshSDFGridPlacement, "MeshSDFGrid", Std430, DESERT_MESH_SDF_GRID_PLACEMENT_FIELDS );
#undef DESERT_MESH_SDF_GRID_PLACEMENT_FIELDS

    // THE CULL'S RECORD (GlobalDistanceField::AddNearFieldCullPasses): the grid, the way back from a cell to the
    // world, the near field's frustum, the object count.
    using MeshSDFCullPlanes = std::array<glm::vec4, 6>;
#define DESERT_MESH_SDF_CULL_PAYLOAD_FIELDS( X )                                                                  \
    X( MeshSDFGridPlacement, Grid, "grid", {} )                                                                   \
    X( glm::mat4, ClipToWorld, "clipToWorld", glm::mat4( 1.0f ) ) /* the inverse of grid.worldToClip */           \
    /* the projection's clip z and w as functions of view z: z = x * vz + y, w = z * vz + w */                    \
    X( glm::vec4, ClipDepth, "clipDepth", glm::vec4( 0.0f ) )                                                     \
    /* world, inward: dot( xyz, p ) + w >= 0 inside - the view's four sides, the camera's plane, the plane at     \
       DF_MESH_SDF_GRID_MAX_DISTANCE */                                                                           \
    X( MeshSDFCullPlanes, Planes, "planes", {} )                                                                  \
    /* x = objects in the object buffer, y = the uints the lists may fill (the grid buffer holds the headers,     \
       then that many), z = this cull's stamp (MeshSDFCullFeedback) */                                                       \
    X( glm::uvec4, Counts, "counts", glm::uvec4( 0u ) )
    DESERT_SHADER_STRUCT( MeshSDFCullPayload, "MeshSDFCullParams", Std430, DESERT_MESH_SDF_CULL_PAYLOAD_FIELDS );
#undef DESERT_MESH_SDF_CULL_PAYLOAD_FIELDS

    // A cell's header in the grid buffer: [cell * stride] = the first index of its list (in the same buffer),
    // [cell * stride + 1] = the list's length. The lists begin at cells.w * stride.
    inline constexpr uint32_t kMeshSDFGridHeaderStride = 2u;
    // What the cull hands back to the CPU (MeshSDFCullGridOffsets.shader writes it,
    // GlobalDistanceField::ReadNearFieldFeedback reads it frames later), 4 uints: [0] the list entries the frame
    // needed, [1] the entries it had room for (counts.y; needed > room = the frame's lists refused, every cell
    // empty), [2] the longest list, [3] the stamp of the cull that wrote it (counts.z).
    inline constexpr uint32_t kMeshSDFCullFeedbackUints = 4u;

    inline constexpr const ShaderProtocols::ShaderStructDecl* kDistanceFieldStructs[] = {
         &kGlobalDistanceFieldCeilingStruct,
         &kMeshSDFGridPlacementStruct,
         &kMeshSDFCullPayloadStruct,
    };
    inline constexpr ShaderProtocols::ShaderConstantDecl kDistanceFieldConstants[] = {
         { .Kind  = ShaderProtocols::ShaderConstantDecl::Type::Uint,
           .Name  = "MeshSDFGridHeaderStride",
           .Value = kMeshSDFGridHeaderStride },
         { .Kind  = ShaderProtocols::ShaderConstantDecl::Type::Uint,
           .Name  = "MeshSDFCullFeedbackUints",
           .Value = kMeshSDFCullFeedbackUints },
         { .Kind  = ShaderProtocols::ShaderConstantDecl::Type::Int,
           .Name  = "GdfCeilingCells",
           .Value = kGdfCeilingCells },
         { .Kind  = ShaderProtocols::ShaderConstantDecl::Type::Int,
           .Name  = "GdfCeilingMip",
           .Value = kGdfCeilingMip },
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kDistanceFieldShaderInclude{
         .Path         = "Generated/DistanceField/DistanceField.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/DistanceField/DistanceFieldLayout.hpp",
         .Structs      = kDistanceFieldStructs,
         .Constants    = kDistanceFieldConstants,
    };
} // namespace Desert::Graphic::DistanceField
