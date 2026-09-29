#pragma once

#include <Engine/Assets/AssetGuidRef.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>

#include <filesystem>
#include <optional>
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

    // True when @p source's current bytes already have an envelope cached under their DDC key (AF4h).
    // Bytes, not times: a `touch` or a fresh checkout does not re-import.
    bool ImportedMeshAssetIsFresh( const std::filesystem::path& source );

    // True when every material slot of @p source's cached envelope has its .demat on disk
    // (MaterialAdoption::MaterialAssetPath). The envelope being fresh says nothing about the materials: they are
    // editable content written beside it by the same import, and a deleted .demat left the mesh drawing the
    // default material for good, because a fresh envelope skipped the import that writes it (THM1a4). Each
    // missing material is logged by name and path.
    bool ImportedMaterialsPresent( const std::filesystem::path& source );

    // THE IMPORT'S "UP TO DATE" FOR A STATIC MESH SOURCE (ImportManager::Import skips the re-parse on true): the
    // envelope is cached for @p source's current bytes AND every material it names has its .demat. The one
    // place both halves are asked together, so the decision is testable without Assimp.
    bool ImportedMeshAssetIsCurrent( const std::filesystem::path& source );

    // THE GATE EVERY READER OF A STATIC MESH SOURCE NEEDS BEFORE TREATING IT AS "COOKED" (AF4h). @p cooked
    // existing on disk covers a hand-authored `.stmesh` (no import involved, so nothing else applies) and a
    // legacy beside-source file not yet overwritten by a re-import; @p source having a fresh DDC envelope
    // covers every import since AF4h, which never writes @p cooked at all. `exists(cooked)` alone — what
    // every reader used to ask — answers "not cooked" for a freshly imported mesh forever, degrading it to
    // the generic type icon (or, for MeshDnD::ResolveOrImport, failing the drop outright) with nothing in
    // the log to say why.
    bool StaticMeshCookAvailable( const std::filesystem::path& cooked, const std::filesystem::path& source );

    // What a re-import does to the file at CookPaths::MeshAsset( @p source ): removes it. Either a legacy file
    // from a build before AF4h (never read, removed silently) or an EDITED import (Assets::IsEditedImportedMesh,
    // P9b) - UE's re-import replaces the asset's source model, and so does this one: the edit is lost, with a
    // warning naming both files. Returns the edited file it removed; nullopt for none or a legacy file.
    std::optional<std::filesystem::path> RemoveBesideSourceFile( const std::filesystem::path& source );

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
    // THE PREVIEW MESH AN IMPORTED MATERIAL NAMES (UE: UMaterial::ThumbnailInfo / PreviewMesh): the static mesh
    // imported from @p source, by the GUID its import record states, located by @p source relative to the
    // working directory (the spelling the browser and ThumbnailSubject::ResolveMesh use). An error when the
    // record is missing - it is written by WriteImportedMeshAsset, so the mesh is written first.
    Common::ResultStr<Assets::AssetGuidRef> PreviewMeshRefFor( const std::filesystem::path& source );
} // namespace Desert::Editor
