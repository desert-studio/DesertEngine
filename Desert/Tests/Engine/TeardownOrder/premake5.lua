local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- WHAT THIS SUITE ASSERTS, AND WHY IT READS SOURCE RATHER THAN CALLING IT. The subject is the ORDER
    -- and the PLACE in which the engine tears itself down, and none of that is a function that can be
    -- called: it is the declaration order of Application's members, the agreement between the fourteen
    -- getters ResourceRegistry exposes and the fourteen it clears, and the absence of std::exit() from
    -- inside a running frame. Every one of them is two places that must agree, which is the exact defect
    -- class DEV_CONTRACT 2.3.1 says a unit test of a function never catches. So the files ARE the unit,
    -- and there is nothing to compile from the engine and nothing to link.
    files {
        test_files,
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
