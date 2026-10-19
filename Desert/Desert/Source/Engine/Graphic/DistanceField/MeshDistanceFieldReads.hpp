#pragma once

// THIS FRAME'S MESH DISTANCE FIELDS AS A TRACE READS THEM (UE FDistanceFieldObjectBufferParameters +
// FDistanceFieldAtlasParameters + FLumenMeshSDFGridParameters): the scene's placed fields - the asset records, the
// resident mips' indirection, the object records and the brick atlas the global distance field is composited from
// (GlobalDistanceField, DistanceFieldSceneData) - and the view's froxel grid of per-cell object lists a ray looks
// its near objects up in (Common/MeshSDFGrid.glslh; culled on the GPU,
// GlobalDistanceField::AddNearFieldCullPasses; read by Common/GITraceDistanceField.glslh, the near-field leg of
// the software tracer). Produced by GlobalDistanceField::AddUpdatePasses and AddNearFieldCullPasses, carried by
// the frame (FrameTransients) to the GI tracer (GlobalIllumination::TracerFrame, DeclareTracerReads).

#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::DistanceField
{
    // The froxel grid's placement - std430 `struct MeshSDFGrid` in Common/MeshSDFGrid.glslh.
    struct MeshSDFGridPlacement
    {
        glm::mat4  WorldToClip{ 1.0f };   // the view's unjittered view-projection
        glm::uvec4 Cells{ 0u };           // cells across, cells down, depth slices, cells in all
        glm::vec4  ZDistribution{ 0.0f }; // log z scale (1/cm), log z offset, z scale, pixels per cell
        glm::vec4  Screen{ 0.0f };        // the view's extent in pixels, 1 / that
    };
    static_assert( sizeof( MeshSDFGridPlacement ) == 112 );

    struct MeshDistanceFieldReads
    {
        RDG::BufferRef  Assets;      // DF_ASSET_STRIDE vec4 per asset ("DFAssetData")
        RDG::BufferRef  Indirection; // every resident mip's indirection, atlas brick slots
        RDG::BufferRef  Objects;     // DF_OBJECT_STRIDE vec4 per object
        RDG::TextureRef BrickAtlas;
        uint32_t        NumObjects = 0; // objects in Objects
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
