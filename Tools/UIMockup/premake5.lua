-- UIMockup - a developer's sketchpad for editor UI that does not exist yet.
--
-- One mockup = one function in its own file under Source/Mockups/, drawn with the editor's OWN theme
-- (Editor/Source/Editor/Core/ThemeManager.cpp is compiled in, not copied) and the editor's own fonts, so a
-- picture taken here reads like the editor rather than like stock Dear ImGui.
--
--   UIMockup --list
--   UIMockup --mockup <name> --out <file.png> [--size WxH]   one frame to a PNG, no window shown
--   UIMockup --mockup <name> [--size WxH]                     interactive window (for the owner)
--
-- A DEV TOOL: removed from the Shipping configuration, and nothing in the engine, the editor or the game
-- links it. It does not link the engine either - GLFW and Dear ImGui are compiled straight in, the same
-- shape as Tools/CrashReporter (see that file for why the ImGui sources are compiled here rather than
-- linked from the "ImGui" project: one C runtime for the whole binary).
project "UIMockup"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"
    targetname "UIMockup"

    removeconfigurations { "Shipping" }

    files {
        "Source/**.hpp",
        "Source/**.cpp",

        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/ThemeManager.cpp",

        "%{_MAIN_SCRIPT_DIR}/ThirdParty/ImGui/imgui.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/ImGui/imgui_draw.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/ImGui/imgui_tables.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/ImGui/imgui_widgets.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/ImGui/backends/imgui_impl_glfw.cpp",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/ImGui/backends/imgui_impl_opengl3.cpp",
    }

    includedirs {
        "Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",   -- Editor/Core/ThemeManager.hpp, IconsMaterialDesignIcons.hpp
    }

    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty",                 -- <ImGui/imgui.h> as ThemeManager.cpp spells it
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/ImGui",           -- "imgui_internal.h" unqualified
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/ImGui/backends",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/GLFW/include",
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/stb/include",     -- stb_image/stb_image_write.h
    }

    -- ImVec2 arithmetic everywhere, as ThemeManager.cpp already asks for it (an identical redefinition there).
    defines { "IMGUI_DEFINE_MATH_OPERATORS" }

    links {
        "GLFW",
    }

    -- The editor's fonts travel with the binary and are resolved from ITS OWN directory, so the tool runs
    -- from any working directory. A missing font is an error, not a fallback to ImGui's default.
    postbuildcommands {
        "{MKDIR} %{cfg.targetdir}/Resources/Fonts",
        "{COPYFILE} %{_MAIN_SCRIPT_DIR}/Editor/Resources/Fonts/Roboto-Regular.ttf %{cfg.targetdir}/Resources/Fonts/",
        "{COPYFILE} %{_MAIN_SCRIPT_DIR}/Editor/Resources/Fonts/Roboto-Bold.ttf %{cfg.targetdir}/Resources/Fonts/",
        "{COPYFILE} %{_MAIN_SCRIPT_DIR}/Editor/Resources/Fonts/materialdesignicons-webfont.ttf %{cfg.targetdir}/Resources/Fonts/",
    }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
        links { "opengl32.lib" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
        links { "OpenGL.framework", "Cocoa.framework", "IOKit.framework", "CoreFoundation.framework",
                "CoreVideo.framework", "QuartzCore.framework", "Carbon.framework" }

    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
        links { "GL" }

    filter {}
