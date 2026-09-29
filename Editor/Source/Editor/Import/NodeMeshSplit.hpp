#pragma once

// ONE STATIC MESH PER SOURCE NODE (UE: the FBX import option "Combine Meshes", OFF by default -
// UnFbx::FFbxImporter imports every mesh-bearing node as its own UStaticMesh, FbxMainImport.cpp /
// FbxStaticMeshImport.cpp ImportStaticMesh per node, with "Transform Vertex to Absolute" off so each asset's
// geometry sits around its own pivot). A Poly Haven grass file is 17 tufts on 17 nodes in a 5.6 m row: combined,
// it is one mesh and one thumbnail of dots; split, every tuft is an asset a foliage type can name.
//
// THE PATTERN, NOT THE LETTER. The importer bakes each node's world transform into the combined vertices
// (AssimpImporter ProcessScene) and records which node placed each submesh (ImportResult::SubmeshNodes); the
// split is a pure function over that result, so it is tested without assimp. Each node asset is a standalone
// `<source stem>_<node>.stmesh` beside the source (its stem is not the source's, so the loader reads it from
// disk - MeshDerivedData.hpp LoadMeshSourceAsset), with provenance naming the source file.
//
// IDENTITY. A node asset's GUID is derived from the source's mesh-relative id and the NODE NAME, never from the
// node's index: a re-import keeps every GUID, and so does an export that reorders the nodes.
//
// THE PIVOT. Bottom centre of the node's box (x, z centre; y minimum - Y is up after import), so a tuft
// placed on the ground stands on it (UE's per-node pivot plays the same role).

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Editor
{
    struct NodeMesh
    {
        std::string                          Node; // the node name, LOD suffix stripped (X_LOD0, X_LOD1 -> X)
        Assets::Serialization::MeshAssetData Mesh;
    };

    // The submeshes of @p combined grouped by the node that placed them (@p submeshNodes: one name per submesh),
    // in order of first appearance; each group rebased onto its own bottom-centre pivot. Refused, by name, when
    // the mesh is skinned, carries morph targets, or the node list does not match the submeshes one to one.
    [[nodiscard]] Common::ResultStr<std::vector<NodeMesh>>
    SplitStaticMeshByNode( const Assets::Serialization::MeshAssetData& combined,
                           std::span<const std::string>                submeshNodes );

    // What an import of @p source writes besides its combined mesh: the node meshes when the source's
    // "Combine Meshes" option is off (ReadImportRecordSettings) and it has more than one node; none otherwise
    // (Combine Meshes on, or a single node, which the combined mesh already is).
    [[nodiscard]] Common::ResultStr<std::vector<NodeMesh>>
    NodeMeshesOfImport( const Assets::Serialization::MeshAssetData& combined,
                        std::span<const std::string> submeshNodes, const std::filesystem::path& source );

    // Stable identity of @p node's asset imported from @p source: a function of the source's mesh-relative id
    // and the node name only.
    [[nodiscard]] Common::Content::AssetGuid NodeMeshGuid( const std::filesystem::path& source,
                                                           std::string_view             node );

    // `<source folder>/<source stem>_<node>.stmesh`, the node name made file-safe (StaticMeshAssetName).
    [[nodiscard]] std::filesystem::path NodeMeshAssetPath( const std::filesystem::path& source,
                                                           std::string_view             node );

    // Writes @p node as its static mesh asset beside @p source (overwriting the previous import of that node, as a
    // re-import does in UE) and returns the path written.
    [[nodiscard]] Common::ResultStr<std::filesystem::path>
    WriteNodeMeshAsset( const NodeMesh& node, std::span<const Assets::MeshMaterialSlot> named,
                        const std::filesystem::path& source );

    // THE IMPORT'S RECORD, ONE WRITER FOR EVERY KIND OF FILE (UE: every import - static or skeletal - leaves its
    // AssetImportData, which Reimport reads): the source's `.deimport` with @p settings and the box of
    // @p imported (source space, before the options are applied), no box when the file has no mesh (a skeleton
    // and its clips). The static writer below calls it first; a skinned import calls it from the import itself.
    // A source with a record is not new: the Import Options window is not offered for it again.
    [[nodiscard]] Common::BoolResultStr RecordImport( const std::filesystem::path&                source,
                                                      const Assets::Serialization::MeshAssetData* imported,
                                                      const Assets::SourceImportSettings&         settings );

    // THE STATIC MESH IMPORT'S WRITE, BOTH MODES (THM1j; UE: UFbxStaticMeshImportData::bCombineMeshes). The record
    // first (identity, box). Split (Combine Meshes off, more than one node): each node's mesh beside the source
    // and the node names in the record - and NO combined mesh, exactly as UE imports no combined asset then.
    // Combined: the one mesh (WriteImportedMeshAsset) and no node list. Returns the node meshes written (empty
    // when combined); the first failure otherwise, after every node was attempted. @p settings are the options
    // the import runs with (THM1l): written into the record first, and every mesh written reads them there.
    [[nodiscard]] Common::ResultStr<std::vector<std::pair<NodeMesh, std::filesystem::path>>>
    WriteStaticMeshImport( const Assets::Serialization::MeshAssetData& imported,
                           std::span<const std::string>                submeshNodes,
                           std::span<const Assets::MeshMaterialSlot> named, const std::filesystem::path& source,
                           const Assets::SourceImportSettings& settings );
} // namespace Desert::Editor
