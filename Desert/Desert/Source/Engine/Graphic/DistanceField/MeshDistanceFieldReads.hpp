#pragma once

// THIS FRAME'S MESH DISTANCE FIELDS AS A TRACE READS THEM (UE FDistanceFieldObjectBufferParameters +
// FDistanceFieldAtlasParameters + FLumenMeshSDFGridParameters): the scene's placed fields - the asset records, the
// resident mips' indirection, the object records and the brick atlas the global distance field is composited from
// (GlobalDistanceField, DistanceFieldSceneData) - and the camera-centred grid of per-cell object lists a ray looks
// its near objects up in (Common/GITraceDistanceField.glslh, the near-field leg of the software tracer).
// Produced by GlobalDistanceField::AddUpdatePasses, carried by the frame (FrameTransients) to the GI tracer
// (GlobalIllumination::TracerFrame, DeclareTracerReads).

#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::DistanceField
{
    struct MeshDistanceFieldReads
    {
        RDG::BufferRef  Assets;      // DF_ASSET_STRIDE vec4 per asset ("DFAssetData")
        RDG::BufferRef  Indirection; // every resident mip's indirection, atlas brick slots
        RDG::BufferRef  Objects;     // DF_OBJECT_STRIDE vec4 per object
        RDG::BufferRef  ObjectGrid;  // per cell: 0 or the offset of its count-prefixed object list
        RDG::TextureRef BrickAtlas;
        // xyz = the grid's minimum corner (cm), w = its cell (cm); DF_MESH_SDF_GRID_RESOLUTION cells per side.
        glm::vec4 ObjectGridPlacement{ 0.0f };

        [[nodiscard]] bool IsValid() const
        {
            return Assets.IsValid() && Indirection.IsValid() && Objects.IsValid() && ObjectGrid.IsValid() &&
                   BrickAtlas.IsValid();
        }
    };
} // namespace Desert::Graphic::DistanceField
