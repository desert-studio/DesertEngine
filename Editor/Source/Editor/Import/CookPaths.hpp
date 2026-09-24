#pragma once

#include <Common/Core/Constants.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

// Single source of truth for "source asset path -> deterministic cooked path". This logic used to be copied
// in ImportManager, MeshDnD, TextureImporter and FileExplorerPanel and DRIFTED apart (that drift caused the
// "textures outside Resources/Textures silently failed to cook" bug). All callers route through here now.
// These are PURE (no directory creation) — callers create_directories before writing.
namespace Desert::Editor::CookPaths
{
    // Source mesh -> Cooked/Meshes/<rel>.<ext> (ext = ".stmesh" / ".skmesh" / "_<anim>.anim" / ".skeleton").
    // Meshes under Resources/Assets/Meshes keep their layout (relative to that dir). Mesh sources ANYWHERE ELSE
    // under content (e.g. a character pack in Resources/Assets/Collections/<pack>/) map relative to Assets/
    // (then Resources/) instead — otherwise the relative path escapes Cooked/Meshes with "../" and the cooked
    // outputs land outside the tree the preloader scans, so the asset is silently never discovered.
    inline std::filesystem::path CookedMesh( const std::filesystem::path& source, const std::string& ext )
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        fs::path   rel       = fs::relative( source, Common::Constants::Path::MESH_PATH, ec );
        const bool underMesh = !rel.empty() && rel.begin()->string() != "..";
        if ( !underMesh )
        {
            fs::path relAssets = fs::relative( source, Common::Constants::Path::ASSETS_PATH, ec );
            if ( !relAssets.empty() && relAssets.begin()->string() != ".." )
                rel = relAssets;
            else
            {
                fs::path relRes = fs::relative( source, Common::Constants::Path::RESOURCE_PATH, ec );
                if ( !relRes.empty() && relRes.begin()->string() != ".." )
                    rel = relRes;
            }
        }

        fs::path result = Common::Constants::Path::MESH_PATH_COOKED / rel;
        result.replace_extension( ext );
        return result;
    }

    // A MESH'S IDENTITY, WITH ITS DIRECTORY IN IT: the cooked path relative to Cooked/Meshes, extension
    // dropped — "Props/base" for a source that cooks to Cooked/Meshes/Props/base.stmesh.
    //
    // The importer used to identify a source by `stem()` alone, i.e. by "base", with the directory thrown
    // away entirely. Two meshes with the same file name in different folders were then the SAME asset as
    // far as the importer was concerned: the same material ids (the key was `<stem>::<material>#<index>`)
    // and the same material output folder. And because the writer skips a .demat that already exists, the
    // second mesh did not overwrite the first — it silently adopted it. Nothing logged, nothing null; the
    // second model simply came in wearing the first one's surface. That is a collision BY CONSTRUCTION,
    // not by unlucky hashing, and no amount of care at the lookup can undo it, because both records are
    // Materials and a type check cannot tell two Materials apart.
    //
    // The repository already stands one file away from it: Assets/Meshes/base.fbx, base_basic_pbr.fbx and
    // base_basic_shaded.fbx each contain a material named "model" — all three keys are `<stem>::model#0`
    // and the ONLY thing separating them is that the three stems differ. A second base.fbx from any other
    // pack, in any other folder, merges with the first.
    //
    // Derived from the COOKED path rather than from the source path directly, so that "where is this
    // asset in the project" is answered in exactly one place. CookedMesh's ladder already decides what a
    // source outside Resources/Assets/Meshes means; asking it again here would be a second answer to a
    // question that already has one, and two answers that must agree is the defect this file was created
    // to end.
    inline std::filesystem::path MeshRelativeId( const std::filesystem::path& source )
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        fs::path rel = fs::relative( CookedMesh( source, "" ), Common::Constants::Path::MESH_PATH_COOKED, ec );
        rel.replace_extension();
        return rel;
    }

    // Where an imported mesh's materials live as editable content:
    // Resources/Assets/Materials/<meshRelativeId>/<materialName>.demat.
    //
    // Both the writer (ImportManager::SerializeMaterialAsset) and the reader that registers them after a
    // drag-drop (MeshDnD) call THIS — they used to spell `MATERIAL_PATH / stem` separately, which is two
    // places obliged to agree with nothing checking that they do.
    inline std::filesystem::path MaterialFolder( const std::filesystem::path& source )
    {
        return Common::Constants::Path::MATERIAL_PATH / MeshRelativeId( source );
    }

    // The string an imported material's stable id is hashed from. The material's own name and its index
    // in the source separate materials WITHIN one mesh; MeshRelativeId separates one mesh from another,
    // which the file stem could not.
    inline std::string MaterialKey( const std::filesystem::path& source, const std::string& materialName,
                                    const uint32_t index )
    {
        return MeshRelativeId( source ).generic_string() + "::" + materialName + "#" + std::to_string( index );
    }
} // namespace Desert::Editor::CookPaths
