local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- PropertyReset is the DECISION behind the Details reset button (bytes -> default + the recorded
    -- edit), deliberately std-only so it compiles here without ImGui, a window or a GPU. MultiEdit
    -- rides along for the reset-then-broadcast relation — the multi-select half of Д29.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Panels/PropertyEditor/PropertyReset.cpp",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/MultiEdit.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",                  -- <Engine/Reflection/ReflectionTypes.hpp>
        "%{_MAIN_SCRIPT_DIR}/Editor/Source",                         -- <Editor/Panels/PropertyEditor/PropertyReset.hpp>
    }
    externalincludedirs {
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/reflect-cpp/include",        -- ReflectionTypes.hpp -> <rflcpp/rfl/Generic.hpp>
    }

    for name, path in pairs(deps.Common.IncludeDir) do
        externalincludedirs { path }
    end

    for name, path in pairs(deps.TestSpecific.IncludeDir) do
        externalincludedirs { path }
    end

    for _, define in ipairs(deps.TestSpecific.Defines) do
        defines { define }
    end

    links { "Common", "Optick" }

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
