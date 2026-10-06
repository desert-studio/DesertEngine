#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

// THE IMPORT RECORD (FIX8; UE: the persistent GUID and the AssetImportData a .uasset carries). Since AF4h an
// imported static mesh has no file of its own: its source envelope lives in the DDC, keyed by the source's
// bytes, and nothing is written at `<stem>.stmesh`. So the mesh's identity lives in a small TRACKED text file
// beside the source, `<name>.<ext>.deimport` (base.fbx -> base.fbx.deimport): its header states the mesh's
// GUID (Kind StaticMesh, `DIMP` 1). It is written once, at the first import, and a re-import never rewrites
// it, so every reference by GUID (a scene, a foliage type) survives an edit of the source. Moving or renaming
// the source moves the record with it (Editor AssetFileOps).
//
// This header is the path logic every layer shares — the content scan (a registry row for the mesh), the
// mesh asset (its GUID and handle), the DDC loader (the source beside the asset); reading and writing the
// record's body is Engine/Assets/Serialization/ImportRecord.
namespace Common::Content
{
    inline constexpr std::string_view kImportRecordSuffix = ".deimport";

    // Every raw format the editor's ImportManager recognises for a mesh (the same set GamePackager::
    // IsRawMeshSource keeps out of a package: the runtime never reads these directly).
    inline constexpr std::array<std::string_view, 6> kRawMeshSourceExtensions = { ".fbx", ".obj",   ".gltf",
                                                                                  ".glb", ".blend", ".dae" };

    // The raw source beside a mesh asset path, if one exists under a recognised extension - same stem, same
    // folder (CookPaths::MeshAsset's mapping, inverted).
    [[nodiscard]] std::optional<std::filesystem::path> MeshSourceBeside( const std::filesystem::path& assetPath );

    // base.fbx -> base.fbx.deimport.
    [[nodiscard]] std::filesystem::path ImportRecordPathFor( const std::filesystem::path& source );

    // True for a `<name>.<ext>.deimport` file.
    [[nodiscard]] bool IsImportRecord( const std::filesystem::path& file );

    // THE SOURCES WHOSE RECORDS LIE IN @p folder (base.fbx.deimport -> folder/base.fbx), in directory order: the
    // one walk every "which import wrote this file" question starts from (a node `.stmesh`, a `.skmesh`, a
    // `.skeleton`); the record's own body answers it. Empty for a folder that cannot be read.
    [[nodiscard]] std::vector<std::filesystem::path> SourcesRecordedIn( const std::filesystem::path& folder );

    // The static mesh asset path a record stands for: base.fbx.deimport -> base.stmesh.
    [[nodiscard]] std::filesystem::path MeshAssetOfImportRecord( const std::filesystem::path& record );

    // The record that stands for a static mesh asset WITH NO FILE OF ITS OWN (an import since AF4h): the
    // asset path does not exist, a raw source is beside it and that source's record exists. nullopt
    // otherwise - a hand-authored `.stmesh` on disk is its own identity.
    [[nodiscard]] std::optional<std::filesystem::path>
    ImportRecordStandingFor( const std::filesystem::path& assetPath );
} // namespace Common::Content
