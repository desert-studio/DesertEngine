local baseDir = "%{wks.location}/ThirdParty"

-- Assimp: ONE source, ONE version, both platforms — BuildScripts/ThirdParty/Assimp.lua compiles the
-- pinned submodule as the `Assimp` static library. Consumers name the PROJECT, never a file, so the
-- toolset does not appear in a link line and updating assimp cannot break the build somewhere that does
-- not mention assimp. (It used to: Windows linked the committed `assimp-vc142-mtd.lib` by that name
-- while the workspace pins v143, and macOS linked Homebrew's `-lassimp`, i.e. the developer's version.)
--
-- Two include dirs, and both are required: the submodule's own `include/`, and the generated one that
-- holds `config.h` / `revision.h` — assimp's CMake produces those from `.in` templates, and Assimp.lua
-- produces them at generation time instead. A consumer that includes only the first gets
-- "assimp/config.h: No such file or directory" from the very first assimp header.
local assimpIncludeDirs = {
    "%{wks.location}/Editor/ThirdParty/assimp/include",
    "%{wks.location}/build/generated/assimp/include",
}

Dependencies = {

    EditorSpecific = {
         IncludeDir = {
            imGuizmo   = "ThirdParty/ImGuizmo",
            assimp     = assimpIncludeDirs[1],
            assimpGen  = assimpIncludeDirs[2],
            stb        = baseDir .. "/stb/include",
            reflect_cpp = baseDir .. "/reflect-cpp/include",
        },

        -- NO assimp ENTRY, AND THAT IS THE CHANGE. It is a premake project now, so consumers put
        -- "Assimp" in `links` beside "Desert" and "GLFW"; a per-configuration path to a prebuilt file
        -- is exactly what this task removed.
        Libraries = {
            Debug = {},
            Release = {}
        }
    }
}

return Dependencies
