#pragma once

// THIS FRAME'S MESH DISTANCE FIELDS AS A TRACE READS THEM (UE FDistanceFieldObjectBufferParameters +
// FDistanceFieldAtlasParameters + FLumenMeshSDFGridParameters): the scene's placed fields - the asset records, the
// resident mips' indirection, the object records and the brick atlas the global distance field is composited from
// (GlobalDistanceField, DistanceFieldSceneData) - and the view's froxel grid of per-cell object lists a ray looks
// its near objects up in (Common/MeshSDFGrid.glslh; culled on the GPU,
// GlobalDistanceFieldView::AddNearFieldCullPasses; read by Common/GITraceDistanceField.glslh, the near-field leg of
// the software tracer). Produced by GlobalDistanceField::AddUpdatePasses and GlobalDistanceFieldView::AddFramePasses, carried by
// the frame (FrameTransients) to the GI tracer (GlobalIllumination::TracerFrame, DeclareTracerReads).

#include <Engine/Graphic/DistanceField/DistanceFieldLayout.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::DistanceField
{
    struct MeshDistanceFieldReads
    {
        RDG::BufferRef  Assets;      // DF_ASSET_STRIDE vec4 per asset ("DFAssetData")
        RDG::BufferRef  Indirection; // every resident mip's indirection, atlas brick slots
        RDG::BufferRef  Objects;     // DF_OBJECT_STRIDE vec4 per object
        RDG::TextureRef BrickAtlas;
        // The MESH objects, which lead Objects: the near field's (AddNearFieldCullPasses culls these only). The
        // landscape tiles placed after them (DF_OBJECT_FLAG_HEIGHTFIELD) are surfaces of the global field alone
        // (UE r.AOGlobalDistanceFieldRepresentHeightfields): the per-object near-field trace is for meshes.
        uint32_t NumMeshObjects = 0;
        // Every record of Objects, the mesh objects and the landscape tiles after them: what a reader that takes
        // the heightfields too walks (DistanceFieldShadowCull.shader, UE's shadow tile cull of mesh SDFs and
        // heightfields into the same lists).
        uint32_t NumObjects = 0;
        // The near field's froxel grid (AddNearFieldCullPasses): per cell MeshSDFGridHeaderStride uints (its
        // list's first index, its length), then the compacted lists; and its placement.
        RDG::BufferRef       NearFieldGrid;
        MeshSDFGridPlacement NearFieldGridPlacement;

        [[nodiscard]] bool IsValid() const
        {
            return Assets.IsValid() && Indirection.IsValid() && Objects.IsValid() && NearFieldGrid.IsValid() &&
                   BrickAtlas.IsValid();
        }
    };
} // namespace Desert::Graphic::DistanceField
