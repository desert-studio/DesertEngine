#pragma once

// Ported from UE 5.8 Engine/Plugins/Runtime/MeshModelingToolset/Source/ModelingComponents/Private/
// ModelingToolTargetUtil.cpp:453-500 (CommitDynamicMeshUpdate -> UStaticMesh source model), adapted: the
// target asset is our MeshSourceAsset envelope; the edit replaces LOD0's EditMeshSer and nothing else of the
// asset's identity, and an imported mesh's file lands at its asset path beside the raw source (P9b).
//
// WHERE THE EDITED GEOMETRY OF AN IMPORTED MESH LIVES. UE writes a tool's result into the UStaticMesh; the
// .fbx is only read again by a re-import, which overwrites the edit. Our imported mesh has no file of its own
// (AF4h: its envelope is derived into the DDC from the .fbx bytes), and the DDC is a cache - an edit kept
// there would die with a cache purge and never reach version control. So the edit is written as a real
// `<stem>.stmesh` at the asset path, stating the GUID of the source's import record (FIX8): the registry row,
// MeshAsset's handle and every GUID reference in a scene or foliage type stay the same, and the loader reads
// that file in the envelope's place (Assets::IsEditedImportedMesh). A re-import deletes it, with a warning.
//
// The mesh a tool edits is the RENDER form lifted back (ModelingToolTarget: import scale and up axis already
// applied), so the written asset's import settings are the identity - building it again must not apply
// them twice. Its IMPT provenance (the source file and its hash) is kept: it is still that file's asset.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp> // FDynamicMesh3

#include <filesystem>
#include <optional>

namespace Desert::Editor
{
    // @p current with its LOD0 replaced by @p edited (authored LODs dropped: they were made for the old
    // LOD0) and the build settings reset to the identity. The slot table is re-indexed the way the builder
    // numbered the render sections the tool lifted: section j is the j-th distinct material ID of the old
    // LOD0, ascending. Refused, by name, when the loader could not derive render data from @p edited.
    [[nodiscard]] Common::ResultStr<Assets::MeshSourceAsset>
    EditedMeshSourceAsset( const Assets::MeshSourceAsset& current, const Geometry::DynamicMesh3& edited );

    // Commits @p edited into the static mesh asset at @p assetFile (the path a MeshHandle resolves to):
    // reads the asset as the loader does, builds EditedMeshSourceAsset and writes it at @p assetFile, same GUID.
    [[nodiscard]] Common::BoolResultStr WriteEditedMeshAsset( const std::filesystem::path&  assetFile,
                                                              const Geometry::DynamicMesh3& edited );
} // namespace Desert::Editor
