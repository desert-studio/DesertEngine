#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <utility>

// THE IMPORT'S OUTPUT FOR A STATIC MESH (UE: UFbxFactory -> UStaticMesh with one FStaticMeshSourceModel per LOD,
// saved as part of the package). The importer's MeshAssetData is split into source models and written as a
// MeshSourceAsset envelope into the DDC (AF4h: Assets::kMeshSourceDeriver, keyed by the source file's own
// bytes - nothing is written at CookPaths::MeshAsset( source ) any more, so a build cannot leave a
// multi-megabyte file beside `base.fbx`). The render form is a second, separate derivation from THAT
// envelope (Assets::LoadMeshPlatformData -> the DDC, BuildMeshPlatformData on a miss). Nothing here writes
// under Cooked/ either: that root holds only the skinned outputs (AF4f moved them).
namespace Desert::Editor
{
    // "<base>_LOD<n>" (case-insensitive suffix, digits only) -> { base, n }; any other name is { name, 0 }.
    std::pair<std::string, int> ParseSourceModelLOD( const std::string& name );

    // The importer's static MeshAssetData as source models: submeshes named "<base>_LOD<k>" form model k, the
    // rest model 0, each welded back into one EditMesh (Geometry::FromMeshAssetData) whose material IDs index
    // one slot table shared by every model (first-use order, named from @p named by GUID, else by the
    // submesh). Refused, naming the file and the reason, where the source would lose something it holds: a
    // skinned mesh, morph targets (EditMesh has no morph layer), a gap in the LOD numbering, and every face
    // set FromMeshAssetData cannot weld one-to-one (degenerate, duplicate or non-manifold faces).
    Common::ResultStr<Assets::MeshSourceData>
    MeshSourceFromImport( const Assets::Serialization::MeshAssetData& imported,
                          std::span<const Assets::MeshMaterialSlot> named, const std::string& name );

    // True when @p source's current bytes already have an envelope cached under their DDC key (AF4h).
    // Bytes, not times: a `touch` or a fresh checkout does not re-import.
    bool ImportedMeshAssetIsFresh( const std::filesystem::path& source );

    enum class MeshAssetWrite
    {
        Written,
        Unchanged, // the DDC already holds an entry for this exact source content; nothing was built
    };

    // Builds the asset for @p source and Puts it into the DDC, keyed by @p source's own bytes (AF4h;
    // Assets::kMeshSourceDeriver) - never written beside @p source. A re-import of UNCHANGED bytes is a
    // cache hit (Unchanged); one of CHANGED bytes keys a different entry and mints a fresh envelope Guid -
    // there is no beside-source file left to read an old one from, and none is needed: scenes name a static
    // mesh by its PATH (AssetBase::AssetBase -> AssetHandle::FromCookedPath), not by this Guid.
    Common::ResultStr<MeshAssetWrite> WriteImportedMeshAsset( const Assets::Serialization::MeshAssetData& imported,
                                                              std::span<const Assets::MeshMaterialSlot>   named,
                                                              const std::filesystem::path&                source );
} // namespace Desert::Editor
