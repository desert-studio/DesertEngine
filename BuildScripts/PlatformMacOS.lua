-- macOS-wide workspace settings (Apple Silicon).

filter "system:macosx"
    architecture "ARM64"
    -- 14.0 minimum: Homebrew bottles (shaderc, MoltenVK) target 14.0, and libc++ gates
    -- std::to_chars for floats (std::format) behind 13.3 anyway.
    buildoptions { "-mmacosx-version-min=14.0" }
    linkoptions  { "-mmacosx-version-min=14.0" }

    -- Library search paths for every project: gmake links libraries by name
    -- (-lvulkan, -lassimp, ...), so the Homebrew/LunarG lib dirs must be known.
    if DesertPlatform.HomebrewPrefix then
        libdirs { DesertPlatform.HomebrewPrefix .. "/lib" }
    end
    -- THE DEVELOPMENT MACHINE'S MoltenVK DRIVER MANIFEST, recorded at configure time so a binary started
    -- directly (IDE, Finder, `cd /`) finds its GPU without a script exporting VK_ICD_FILENAMES. Read by
    -- VulkanContext.cpp SelectDriverManifest AFTER the bundle's own manifest; stringified there, so the
    -- value is passed bare (a Homebrew path has no spaces).
    if DesertPlatform.HomebrewPrefix then
        local icd = DesertPlatform.HomebrewPrefix .. "/etc/vulkan/icd.d/MoltenVK_icd.json"
        if os.isfile(icd) then
            defines { "DESERT_MOLTENVK_ICD=" .. icd }
        end
    end
    if os.getenv("VULKAN_SDK") then
        libdirs { os.getenv("VULKAN_SDK") .. "/lib" }
    end

-- AN ARCHIVE IS EXACTLY ITS CURRENT OBJECT LIST. gmake archives with `ar -rcs`, which replaces and appends
-- members but never removes one: a source deleted or moved keeps its last object inside the .a forever, and
-- the linker may resolve a symbol from that member. 2026-10-09 (UI-FW2c): UICanvasResources.cpp moved to
-- UI/Ecs/RegistryUICanvasResources.cpp, the old member still defined the previous vtable of
-- RegistryUICanvasResources, and the editor crashed (SIGBUS) on the first virtual call through it. Deleting
-- the archive before each link makes `ar` write it from $(OBJECTS) alone. (MSVC's lib.exe rewrites the whole
-- .lib every time, so Windows needs nothing.)
filter { "system:macosx", "kind:StaticLib" }
    prelinkcommands { "rm -f \"%{cfg.buildtarget.abspath}\"" }

filter {}
