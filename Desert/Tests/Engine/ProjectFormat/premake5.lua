local test_name = path.getname(_SCRIPT_DIR)
local test_files = os.matchfiles("*.cpp")

project(test_name)
    kind "ConsoleApp"
    language "C++"

    targetdir ("%{_MAIN_SCRIPT_DIR}/build/Bin/Tests/%{cfg.buildcfg}")
    objdir ("%{_MAIN_SCRIPT_DIR}/build/Tests/Intermediates/%{cfg.buildcfg}")

    -- The formats live in the desert-shared submodule; so does their CONFORMANCE suite, which both
    -- the engine and the launcher must run — this project compiles the submodule's test file next
    -- to the engine-local half (census vs Constants::Path + the gtest main). Needs only Common
    -- (the serializer is compiled into it) and ReflectCpp — no window, no device, no engine.
    files {
        test_files,
        "%{_MAIN_SCRIPT_DIR}/ThirdParty/desert-shared/Tests/project_format_test.cpp",
        -- The engine's WRITER for engines.json. It takes its config directory as an argument
        -- precisely so it can be compiled here and pointed at a temp folder — without that it would
        -- be reachable by no suite that is not willing to write into the developer's real
        -- ~/.desertengine, which is how a file-writing function ends up untested.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Project/EngineRegistration.cpp",
        -- Compiled here for TWO reasons that arrived from different directions and turned out to be
        -- the same shape, which is why this line is worth its comment:
        --   * the .deproj half asserts that ProjectContext::Save carries a foreign key across a
        --     read-modify-write, so the suite has to compile the thing that writes the file;
        --   * it is also the engine's half of the OTHER two-writer file, projects.json —
        --     RecentProjects/RegisterRecent take the config directory as an argument, so the protocol
        --     the launcher plays the other half of is reachable here without writing into the
        --     developer's own ~/.desertengine.
        -- Everything it touches is Common, which is what keeps this suite device-free.
        "%{_MAIN_SCRIPT_DIR}/Desert/Desert/Source/Engine/Project/ProjectContext.cpp",
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

    links { "Common", "Optick" }

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
