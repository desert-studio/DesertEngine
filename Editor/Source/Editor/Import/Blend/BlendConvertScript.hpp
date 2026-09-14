#pragma once

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <filesystem>
#include <system_error>

namespace Desert::Editor
{
    // The Python that drives headless Blender, and the one function that puts it on disk.
    //
    // WHY IT IS ITS OWN HEADER (Д35). It used to be a private static of BlendImporter, which includes
    // AssimpImporter.hpp and therefore cannot be compiled anywhere Assimp is not linked — so the one
    // branch worth asserting about it, "the script was NOT written and we said so", was unreachable to
    // every test binary. Nothing here needs Assimp; separating it is what makes the refusal testable
    // (Desert/Tests/Editor/CookedWriteRefusal) rather than argued.
    inline constexpr const char* kBlendConvertScript = R"PY(
import bpy, sys, os
argv = sys.argv
out = argv[argv.index("--") + 1]
os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.export_scene.fbx(
    filepath=out,
    use_selection=False,
    apply_unit_scale=True,
    bake_space_transform=False,
    object_types={'MESH', 'ARMATURE'},
    use_mesh_modifiers=True,
    add_leaf_bones=False,
    path_mode='COPY',
    embed_textures=False,
    bake_anim=True,
)
)PY";

    /**
     * @brief Write the Blender export script to @p path. Returns @p path, or an EMPTY path if it is not
     *        on the disk — and empty means the caller must not launch Blender.
     *
     * It goes through Common::Utils::FileSystem::WriteContentToFileAtomic. The previous version opened a
     * local std::ofstream, checked the OPEN, did `out << kScript` and returned the path with nothing
     * checked afterwards. The script is under a kilobyte, i.e. smaller than one filebuf, so on a failure
     * that only the flush could see NOTHING reached the disk and the path came back anyway — Blender was
     * then launched on an empty file, and the failure was reported to the user as Blender complaining
     * about broken Python. A diagnostic pointing at the wrong file is worse than no diagnostic.
     */
    [[nodiscard]] inline std::filesystem::path WriteBlendConvertScript( const std::filesystem::path& path )
    {
        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );

        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, kBlendConvertScript );
             !written )
        {
            LOG_ERROR( "[Blend] the convert script {} was not written: {}", path.string(), written.GetError() );
            return {};
        }
        return path;
    }
} // namespace Desert::Editor
