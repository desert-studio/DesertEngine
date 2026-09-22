-- "The memory number has to speak about growth, and an unknown must not look like a zero."
--
-- WHY THIS LINKS THREE .cpp FILES AND NOT libDesert. `MemoryReadout::Take()` is the only impure part of
-- the readout — it reaches EngineContext, i.e. Application + Window + RendererContext — and it lives in
-- its own translation unit (MemoryReadoutSource.cpp) for exactly this reason. Everything this suite
-- asserts is a DECISION: which of three numbers is printed, whether a zero means "empty" or "nobody
-- answered", how a peak is folded, what growth is measured against. Those are where a defect would live,
-- and a suite that had to open a window to reach them would not have been written.
local deps = dofile(_MAIN_SCRIPT_DIR .. '/Desert/Dependencies.lua')

local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{wks.location}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{wks.location}/build/Tests/Intermediates/%{cfg.buildcfg}")

    files {
        test_files,
        "%{wks.location}/Desert/Desert/Source/Engine/Graphic/MemoryReadout.cpp",
        "%{wks.location}/Desert/Common/Source/Common/Utilities/ProcessMemory.cpp",
        -- THE THIRD FILE, AND WHY IT IS A FILE RATHER THAN libCommon. The ledger's rows carry a
        -- `Common::AssetHandle`, whose default constructor is `UUID(uint64_t)` — out of line, and the
        -- only symbol this suite needs from Common now that it opens ledger rows of its own. Linking the
        -- whole library to resolve one constructor would put Application, Window and the filesystem layer
        -- behind a suite whose entire value is that it asserts DECISIONS without opening a window.
        "%{wks.location}/Desert/Common/Source/Common/Core/UUID.cpp",
    }

    includedirs {
        "%{wks.location}/Desert/Common/Source",
        "%{wks.location}/Desert/Desert/Source",
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

    filter "system:windows"
        defines { "DESERT_PLATFORM_WINDOWS" }
    filter "system:macosx"
        defines { "DESERT_PLATFORM_MACOS" }
    filter "system:linux"
        defines { "DESERT_PLATFORM_LINUX" }
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
