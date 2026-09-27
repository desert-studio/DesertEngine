local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- ThemeManager.cpp is compiled by no other suite and needs a live ImGui context to write into, so
    -- the four ImGui translation units come along (not the ImGui static library: its Windows runtime
    -- flags differ from the test runtime).
    files {
        test_files,
        "%{wks.location}/Editor/Source/Editor/Core/ThemeManager.cpp",
        "%{wks.location}/ThirdParty/ImGui/imgui.cpp",
        "%{wks.location}/ThirdParty/ImGui/imgui_draw.cpp",
        "%{wks.location}/ThirdParty/ImGui/imgui_tables.cpp",
        "%{wks.location}/ThirdParty/ImGui/imgui_widgets.cpp",
    }

    includedirs {
        "%{wks.location}/Editor/Source",
        "%{wks.location}/ThirdParty",         -- <ImGui/imgui.h>
        "%{wks.location}/ThirdParty/ImGui",   -- ThemeManager.cpp includes "imgui_internal.h" unqualified
    }

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    filter "configurations:Debug"
        for name, path in pairs(deps.TestSpecific.Libraries.Debug) do
            links { path }
        end

    filter "configurations:Release"
        for name, path in pairs(deps.TestSpecific.Libraries.Release) do
            links { path }
        end

    filter {}

print("Configured test project: " .. test_name)
