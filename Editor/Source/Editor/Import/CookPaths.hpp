#pragma once

#include <Common/Core/Constants.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

// Single source of truth for "source asset path -> the asset paths its import writes". This logic used to be
// copied in ImportManager, MeshDnD, TextureImporter and FileExplorerPanel and DRIFTED apart (that drift caused the
// "textures outside Resources/Textures silently failed to cook" bug). All callers route through here now.
// These are PURE (no directory creation) — callers create_directories before writing.
namespace Desert::Editor::CookPaths
{
    // A MESH'S ASSETS LIVE BESIDE ITS SOURCE (UE: the package next to the content), never under Cooked/.
    // Static: Assets/Meshes/Props/base.fbx -> Assets/Meshes/Props/base.stmesh, the MeshSourceAsset envelope
    // (AF4d). Skinned (AF8b): the same folder, the source's stem plus the kind's suffix —
    // base.skmesh, base.skeleton, base_<clip>.anim. They are authored content from the moment they are
    // written: the content registry gathers them from the assets root like every other kind, and the
    // project's Cooked/ tree holds generated intermediates only.
    inline std::filesystem::path MeshAsset( const std::filesystem::path& source )
    {
        std::filesystem::path result = source;
        result.replace_extension( ".stmesh" );
        return result;
    }

    // `suffix` is appended to the source's stem verbatim: ".skmesh", ".skeleton", "_<clip>.anim".
    inline std::filesystem::path SkinnedAsset( const std::filesystem::path& source, const std::string& suffix )
    {
        return source.parent_path() / ( source.stem().string() + suffix );
    }

    // A MESH'S IDENTITY, WITH ITS DIRECTORY IN IT: the source path relative to Resources/Assets/Meshes,
    // extension dropped — "Props/base" for Assets/Meshes/Props/base.fbx. A source ANYWHERE ELSE under
    // content (e.g. a character pack in Resources/Assets/Collections/<pack>/) is taken relative to Assets/
    // (then Resources/) instead, so two packs never share an identity through a shared "../".
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
    // THE VALUE IS A STORED IDENTITY: every imported material's GUID is hashed from MaterialKey, below, so
    // this ladder must keep producing the same string for the same source. It used to be derived through
    // the skinned cook path under Cooked/Meshes; AF8b moved that output beside the source and kept the
    // ladder byte for byte (MeshImportKey pins it).
    inline std::filesystem::path MeshRelativeId( const std::filesystem::path& source )
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        fs::path   rel       = fs::relative( source, Common::Constants::Path::MESH_PATH, ec );
        const bool underMesh = !rel.empty() && rel.begin()->string() != "..";
        if ( !underMesh )
        {
            const fs::path relAssets = fs::relative( source, Common::Constants::Path::ASSETS_PATH, ec );
            if ( !relAssets.empty() && relAssets.begin()->string() != ".." )
                rel = relAssets;
            else
            {
                const fs::path relRes = fs::relative( source, Common::Constants::Path::RESOURCE_PATH, ec );
                if ( !relRes.empty() && relRes.begin()->string() != ".." )
                    rel = relRes;
            }
        }

        rel.replace_extension( "" );
        rel.replace_extension();
        return rel;
    }

    // Where an imported mesh's materials live as editable content:
    // Resources/Assets/Materials/<meshRelativeId>/<materialName>.demat.
    //
    // A FILE A SKINNED IMPORT WRITES (SkinnedAsset's three suffixes): `.skmesh`, `.skeleton`, `.anim`. Each is its
    // own cooked form (a picture of it is filed under the file itself), unlike a static mesh, whose `.stmesh` is
    // an extension swap of its source (MeshAsset).
    inline bool IsSkinnedAssetFile( const std::filesystem::path& asset )
    {
        const std::string extension = asset.extension().string();
        return extension == ".skmesh" || extension == ".skeleton" || extension == ".anim";
    }

    // Whether @p asset has the NAME a skinned import of @p source writes beside it (SkinnedAsset):
    // `<stem>.skmesh`,
    // `<stem>.skeleton` or `<stem>_<clip>.anim` in the source's folder. The name only: a clip's name is not in the
    // import record, so `base_Walk.anim` fits both `base.fbx` and a `base_Extra.fbx` beside it — the caller that
    // must choose one (MeshThumbnailHome) takes the longest stem.
    inline bool IsSkinnedAssetOf( const std::filesystem::path& source, const std::filesystem::path& asset )
    {
        if ( asset.parent_path() != source.parent_path() )
            return false;
        const std::string stem = source.stem().string();
        const std::string name = asset.filename().string();
        if ( name == stem + ".skmesh" || name == stem + ".skeleton" )
            return true;
        const std::string prefix = stem + "_";
        const std::string suffix = ".anim";
        return name.size() > prefix.size() + suffix.size() && name.starts_with( prefix ) &&
               name.ends_with( suffix );
    }

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
