#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <utility>

// THE IMPORT'S OUTPUT FOR A STATIC MESH (UE: UFbxFactory -> UStaticMesh with one FStaticMeshSourceModel per LOD,
// saved beside its content). The importer's MeshAssetData is split into source models and written as the
// MeshSourceAsset envelope at CookPaths::MeshAsset( source ); the render form is derived from it on load
// (Assets::LoadMeshPlatformData -> the DDC, BuildMeshPlatformData on a miss). Nothing here writes under
// Cooked/: that root now holds only the skinned outputs (AF4f moves them).
namespace Desert::Editor
{
    // "<base>_LOD<n>" (case-insensitive suffix, digits only) -> { base, n }; any other name is { name, 0 }.
    std::pair<std::string, int> ParseSourceModelLOD( const std::string& name );

    struct ImportedMeshSource
    {
        Assets::MeshSourceData Source;
        // Summed over every LOD: what the weld did that was not a straight copy (Geometry::ImportedEditMesh).
        int DroppedDegenerate = 0;
        int DroppedDuplicate  = 0;
        int DetachedTriangles = 0;
    };

    // The importer's static MeshAssetData as source models: submeshes named "<base>_LOD<k>" form model k, the
    // rest model 0, each welded back into one EditMesh (Geometry::FromMeshAssetData) whose material IDs index
    // one slot table shared by every model (first-use order, named from @p named by GUID, else by the
    // submesh). Degenerate and duplicate faces are skipped and non-manifold ones detached, as UE's
    // MeshDescription conversion does, with the counts returned. Refused, naming the file and the reason,
    // where the source would lose something it holds: a skinned mesh, morph targets (EditMesh has no morph
    // layer), a gap in the LOD numbering, and a LOD with no face left after the weld.
    Common::ResultStr<ImportedMeshSource>
    MeshSourceFromImport( const Assets::Serialization::MeshAssetData& imported,
                          std::span<const Assets::MeshMaterialSlot> named, const std::string& name );

    // The ONE warning line an import with skipped or detached faces logs, naming @p name and the counts;
    // empty when every face crossed as it was.
    std::string SkippedFacesWarning( const ImportedMeshSource& source, const std::string& name );

    // True when the asset beside @p source exists, was imported from it, and its IMPT hash is the current
    // file's PakContentHash. Bytes, not times: a `touch` or a fresh checkout does not re-import.
    bool ImportedMeshAssetIsFresh( const std::filesystem::path& source );

    enum class MeshAssetWrite
    {
        Written,
        Unchanged, // the asset on disk already equals the import; the file is left alone
    };

    // Builds the asset for @p source and writes it at CookPaths::MeshAsset( source ). A RE-IMPORT KEEPS THE
    // ASSET'S GUID AND ITS IMPORT SETTINGS (UE: reimport updates the package in place): scenes name the mesh by
    // GUID, and the settings are the user's. An existing file that does not read as a mesh asset is refused,
    // not overwritten.
    Common::ResultStr<MeshAssetWrite> WriteImportedMeshAsset( const Assets::Serialization::MeshAssetData& imported,
                                                              std::span<const Assets::MeshMaterialSlot>   named,
                                                              const std::filesystem::path&                source );
} // namespace Desert::Editor
