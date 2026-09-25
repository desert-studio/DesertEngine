#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>

#include <string>

namespace Desert::Editor
{
    // Bakes each static submesh's LOD triangle sets (meshopt) into SubmeshData.LODs, so the load path skips the
    // simplification pass. Skinned meshes and submeshes that already carry LODs (authored ones folded in by
    // from authored source models) are left as they are.
    void BakeStaticMeshLODs( Assets::Serialization::MeshAssetData& data );

    // THE MESH BUILDER (UE FStaticMeshBuilder::Build), registered with Assets::SetMeshPlatformDataBuilder.
    // SRCE is stored as the file gave it; the import settings are applied HERE, so changing one re-derives
    // instead of re-importing: UniformScale and UpAxis are baked into the vertices (Geometry::TransformMesh,
    // the Bake Transform tool's mapping), a missing tangent layer is computed from UV 0, and LodPolicy::Generate
    // makes the LOD chain: authored source models k >= 1 fold into LOD k of LOD0's section of the same material
    // slot, and a section with no authored LODs is simplified. The output is the MeshBinary container
    // (EncodeMeshBinary) - the DDC value. Its header GUID is null: identity lives in the asset, and a derived
    // entry shared by two assets with the same source cannot name either of them. Refused, naming the reason: a
    // skinned asset (the skeleton's form is AF4f's decision), a triangle whose material ID has no slot, and
    // everything ToMeshAssetData refuses (colour layer, second UV layer).
    Common::ResultStr<std::string> BuildMeshPlatformData( const Assets::MeshSourceAsset& asset );
} // namespace Desert::Editor
