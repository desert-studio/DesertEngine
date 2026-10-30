#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshDisplacementBake.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>

#include <string>

namespace Desert::Editor
{
    // THE MESH BUILDER (UE FStaticMeshBuilder::Build), registered with Assets::SetMeshPlatformDataBuilder.
    // SRCE is stored as the file gave it; the import settings are applied HERE, so changing one re-derives
    // instead of re-importing: UniformScale and UpAxis are baked into the vertices (Geometry::TransformMesh,
    // the Bake Transform tool's mapping), a missing tangent layer is computed from UV 0, and LodPolicy::Generate
    // makes the LOD chain: authored source models k >= 1 fold into LOD k of LOD0's section of the same material
    // slot, and a section with no authored LODs is simplified. The output is the MeshBinary container
    // (EncodeMeshBinary) - the DDC value. Its header GUID is null: identity lives in the asset, and a derived
    // entry shared by two assets with the same source cannot name either of them. Refused, naming the reason: a
    // skinned asset (the skeleton's form is AF4f's decision), a triangle whose material ID has no slot, and
    // everything ToMeshAssetData refuses (a UV layer past 1; the colour layer and UV 1 are written as streams).
    // Bakes each submesh's LOD triangle sets (meshopt, UE's LOD reduction) into SubmeshData.LODs, static or
    // skinned alike - a LOD only selects a subset of the index buffer, the skin weights ride on the kept
    // vertices. Submeshes that already carry LODs (folded from authored source models) are kept. @p chain is the
    // mesh's MeshImportSettings::LodChain: each level's triangle share and the floor (Geometry::LODChainTargets).
    void BakeMeshLODs( Assets::Serialization::MeshAssetData& data, const Geometry::LODChainSettings& chain );

    Common::ResultStr<std::string> BuildMeshPlatformData( const Assets::MeshSourceAsset& asset );

    // THE DISPLACED MESH BUILDER, registered with Assets::SetDisplacedMeshBuilder (CONTRACT-DISPLACE §1g): every
    // section's height map decoded from its texture asset's platform data (TextureImporter::DecodePlatformMips), the
    // surface tessellated and displaced on the CPU exactly as the GPU pair does (Geometry::BuildDisplacedSurface)
    // at the input's level, bitwise-equal vertices welded (a cluster border is emitted by both clusters), then
    // with BakeLODs the LOD chain (BakeMeshLODs), encoded as the MeshBinary container.
    Common::ResultStr<std::string> BuildDisplacedMeshBytes( const Assets::DisplacedMeshBuildInput& input );
} // namespace Desert::Editor
