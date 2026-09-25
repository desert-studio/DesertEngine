#pragma once

#include "RenderMeshData.hpp"

#include <Common/Core/ResultStr.hpp>
#include <Common/Content/AssetEnvelope.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>

#include <span>

namespace Desert::Geometry
{
    // THE ONE PLACE A .stmesh's ARRAYS MEET THE RENDER ARRAYS. Both modeling cores (EditMesh, EditMeshAsset.hpp,
    // and FDynamicMesh3, DynamicMeshAsset.hpp) go through their own ToRenderMesh / FromRenderMesh and then
    // through these two functions, so the file a core writes depends only on the render arrays it produced:
    // two cores that draw the same thing write the same bytes, and neither can drift in naming, slot binding
    // or polygroup order on its own.

    // Render arrays -> asset data. Submesh k takes slotMaterials[k] (null GUID past the end) and, when unnamed,
    // the name "Section<k>". polyGroups holds one group per render triangle, in render.Indices order.
    [[nodiscard]] Assets::Serialization::MeshAssetData
    MeshAssetDataFromRender( const RenderMeshData&                       render,
                             std::span<const Common::Content::AssetGuid> slotMaterials,
                             std::vector<int32_t>                        polyGroups );

    // Asset data -> render arrays, with no SubmeshMaterialIds (so the import gives submesh k MaterialID k).
    // Refused on a skinned asset, and on a polygroup list that is neither empty nor one entry per face.
    [[nodiscard]] Common::ResultStr<RenderMeshData>
    RenderFromMeshAssetData( const Assets::Serialization::MeshAssetData& data );
} // namespace Desert::Geometry
