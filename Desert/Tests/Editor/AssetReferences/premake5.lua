local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- Only the PURE index is compiled in (std-only). The project scanner (AssetReferencesScan.cpp)
        -- pulls in engine headers and is intentionally left out of the test.
        "%{_MAIN_SCRIPT_DIR}/Editor/Source/Editor/Core/AssetReferences.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Editor/Source", -- <Editor/Core/AssetReferences.hpp>
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

    -- Common contains Objective-C (MacOSFileSystem's file dialogs), and this suite now reaches it
    -- transitively: it tests the removal guard against a real ContentUpdatePlan, and the object that
    -- defines PlanContentUpdate also defines ApplyContentUpdate, which writes files. The linker pulls
    -- whole objects, so the ObjC runtime + AppKit have to come too — the same two lines every other
    -- suite that touches FileSystem carries.
    filter "system:macosx"
        links { "Cocoa.framework", "Foundation.framework" }
    filter {}

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
