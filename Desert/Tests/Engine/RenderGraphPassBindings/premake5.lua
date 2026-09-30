local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        -- The render graph core, device-free, as in RenderGraphContracts. RDG-A2-1 adds RDGPassBindings.cpp here:
        -- until then this suite does not link (PassBindings has declarations only), by design of the contract.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/RDG/RDGBuilder.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/RDG/RDGCompile.cpp",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Graphic/RDG/RDGAsyncFallbackLog.cpp",
    }

    includedirs {
        "%{_MAIN_SCRIPT_DIR}/Desert/Common/Source",
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source",
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
