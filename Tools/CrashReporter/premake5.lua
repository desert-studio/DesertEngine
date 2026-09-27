-- DesertCrashReporter - the window Common::Crash::LaunchReporter starts after a crash.
--
-- THE NAME AND THE LOCATION ARE A CONTRACT, NOT A PREFERENCE. CrashHandler.cpp:1134-1146 looks for
-- "DesertCrashReporter.exe" in the directory of the crashed executable and clears the path when that
-- file is absent. So the target name is pinned below and the binary lands in the workspace targetdir
-- (build/Bin/<config>), which is where Editor.exe and Runtime.exe already are.
--
-- IT MUST NOT LINK THE ENGINE. It exists to explain a process that has just died; a dependency on
-- that process's own code is a dependency that can be broken exactly when it is needed. So: GLFW, and
-- Dear ImGui compiled straight into this target.
--
-- WHY IMGUI IS COMPILED IN RATHER THAN LINKED FROM THE "ImGui" PROJECT. That project is built with
-- staticruntime "On" (/MT) for the Editor, while GLFW is built with staticruntime "off" (/MD). Mixing
-- the two CRTs inside this small target buys nothing; compiling the six core sources here keeps one
-- runtime for the whole binary, and the backends have to be compiled per-target anyway - the core
-- library deliberately carries none (see BuildScripts/ThirdParty/ImGui.lua).
project "DesertCrashReporter"
    kind "WindowedApp"
    language "C++"
    cppdialect "C++20"
    targetname "DesertCrashReporter"

    -- WindowedApp means subsystem:windows, which would otherwise demand WinMain. The reporter has an
    -- ordinary main() because it takes a command-line argument; this entry point is what lets it keep
    -- one while still starting without a console window flashing behind the crash dialog.
    filter "system:windows"
        entrypoint "mainCRTStartup"
    filter {}

    files {
        "Source/**.hpp",
        "Resources/*.rc",
        "Source/**.cpp",

        "../../ThirdParty/ImGui/imgui.cpp",
        "../../ThirdParty/ImGui/imgui_draw.cpp",
        "../../ThirdParty/ImGui/imgui_tables.cpp",
        "../../ThirdParty/ImGui/imgui_widgets.cpp",
        "../../ThirdParty/ImGui/backends/imgui_impl_glfw.cpp",
        "../../ThirdParty/ImGui/backends/imgui_impl_opengl3.cpp",
    }

    externalincludedirs {
        "%{wks.location}/ThirdParty/ImGui",
        "%{wks.location}/ThirdParty/ImGui/backends",
        "%{wks.location}/ThirdParty/GLFW/include",
    }

    links {
        "GLFW",
    }

    -- THE FONTS TRAVEL WITH THE BINARY. The reporter resolves them from ITS OWN directory
    -- (Main.cpp ReporterDirectory + "Resources/Fonts"), not from a working directory, because the
    -- handler starts it with whatever cwd the crashed process happened to have. Copying them here is
    -- what makes that path true for a developer build as well as a packaged one; a font that is
    -- missing is reported in the window rather than swapped for the ImGui default.
    postbuildcommands {
        "{MKDIR} %{cfg.targetdir}/Resources/Fonts",
        "{COPYFILE} %{wks.location}/Editor/Resources/Fonts/Roboto-Regular.ttf %{cfg.targetdir}/Resources/Fonts/",
        "{COPYFILE} %{wks.location}/Editor/Resources/Fonts/Roboto-Bold.ttf %{cfg.targetdir}/Resources/Fonts/",
        "{COPYFILE} %{wks.location}/Editor/Resources/Fonts/RobotoMono-Regular.ttf %{cfg.targetdir}/Resources/Fonts/",
        "{COPYFILE} %{wks.location}/Editor/Resources/Fonts/fontawesome-webfont.ttf %{cfg.targetdir}/Resources/Fonts/",
    }

    filter "configurations:Debug"
        symbols "On"

    filter "configurations:Release"
        optimize "On"

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
        -- opengl32 is the ImGui OpenGL3 backend's only link dependency; it loads every entry point it
        -- needs through its own bundled loader, so there is no GLAD/GLEW here.
        -- Dwmapi carries DwmSetWindowAttribute, which is how the borderless window asks Windows 11
        -- for rounded corners; User32 carries the window subclassing behind our own title bar.
        links { "opengl32.lib", "Shell32.lib", "Dwmapi.lib", "User32.lib" }
        -- rc.exe resolves an ICON path against its include dirs, not against the .rc file, so
        -- the directory holding AppIcon.ico is named here; without it the resource compiles
        -- empty and the build still succeeds, which looks exactly like an icon-cache problem.
        resincludedirs { "%{wks.location}/Tools/CrashReporter/Resources" }

    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
        links { "OpenGL.framework" }

    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
        links { "GL" }

    filter {}
