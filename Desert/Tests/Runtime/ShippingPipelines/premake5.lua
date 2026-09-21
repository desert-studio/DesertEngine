local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- NOTHING FROM THE ENGINE IS COMPILED IN, for the same reason ShippingBoundary compiles none: the
    -- question is "which pipeline-creation sites are inside the compiler's shipping boundary", and that
    -- is a property of the SOURCE TEXT. A suite that linked the engine could only observe the one
    -- configuration it was itself built in, and the sweep builds Debug.
    --
    -- It cannot be answered at run time either, and that is worth saying because the tempting version of
    -- this suite is "boot a renderer and count pipelines". A test binary has no VkDevice on CI, and a
    -- count is the wrong shape anyway: a gate that pins a NUMBER is satisfied by editing the number.
    files { test_files }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
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

    filter "configurations:Release or Shipping"
        for name, path in pairs(deps.TestSpecific.Libraries.Release) do
            links { path }
        end

    filter {}

print("Configured test project: " .. test_name)
