local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- NOTHING FROM THE ENGINE IS COMPILED IN, and that is what this suite IS. It reads the source tree
    -- and the build scripts as TEXT, exactly as BuildScriptContract does: the property it checks — "the
    -- development instruments are not in the shipping build" — is a property of the sources and of the
    -- premake files, and a suite that linked the engine could only ever observe the configuration it was
    -- itself compiled in. A Debug binary cannot run a Shipping one's code to look for what is missing.
    --
    -- The end-to-end half of the proof is NOT here, because a test cannot build a second configuration of
    -- the engine: scripts/CI/ShippingSymbols.sh reads the linked Shipping binary's symbol table. This
    -- suite is the half that runs on every sweep and reddens when a call site is added without a boundary.
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
