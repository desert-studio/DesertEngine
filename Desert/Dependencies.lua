local baseDir = "%{_MAIN_SCRIPT_DIR}/ThirdParty"

-- ============================================================================
-- Vulkan SDK discovery (platform-aware).
-- Returns a table { include = <dir>, lib = <dir> } or nil.
--   Windows: LunarG SDK layout  <sdk>/Include, <sdk>/Lib
--   macOS:   LunarG SDK layout  $VULKAN_SDK/include, $VULKAN_SDK/lib
--            or Homebrew (vulkan-headers + vulkan-loader + molten-vk + shaderc)
--   SPIRV-Cross is never the SDK's: the vendored ThirdParty/SPIRV-Cross (BuildScripts/ThirdParty/SPIRVCross.lua).
--   Linux:   /usr/local/vulkan fallback
-- ============================================================================
local function findVulkanSDK()
    print("[Vulkan] Searching for Vulkan SDK...")

    local vulkan_sdk = os.getenv("VULKAN_SDK")

    if vulkan_sdk and vulkan_sdk ~= "" then
        print("[Vulkan] Found via VULKAN_SDK environment variable: " .. vulkan_sdk)
        if os.target() == "windows" then
            return { include = vulkan_sdk .. "/Include", lib = vulkan_sdk .. "/Lib" }
        end
        return { include = vulkan_sdk .. "/include", lib = vulkan_sdk .. "/lib" }
    end

    print("[Vulkan] VULKAN_SDK environment variable not set or empty")

    if os.target() == "windows" then
        print("[Vulkan] Trying to find VulkanSDK in Program Files...")
        local program_files = os.getenv("PROGRAMFILES")
        vulkan_sdk = program_files .. "/VulkanSDK"

        -- Latest version
        local versions = os.matchdirs(vulkan_sdk .. "/*")
        table.sort(versions, function(a, b) return a > b end)
        if #versions > 0 then
            vulkan_sdk = versions[1]
            print("[Vulkan] Found in Program Files: " .. vulkan_sdk)
            return { include = vulkan_sdk .. "/Include", lib = vulkan_sdk .. "/Lib" }
        end

        print("[Vulkan] Warning: no VulkanSDK versions found in " .. vulkan_sdk)
        return nil
    end

    if os.target() == "macosx" then
        -- LunarG SDK installed to the default per-user location
        local versions = os.matchdirs(os.getenv("HOME") .. "/VulkanSDK/*")
        table.sort(versions, function(a, b) return a > b end)
        if #versions > 0 and os.isdir(versions[1] .. "/macOS") then
            local sdk = versions[1] .. "/macOS"
            print("[Vulkan] Found LunarG SDK: " .. sdk)
            return { include = sdk .. "/include", lib = sdk .. "/lib" }
        end

        -- Homebrew (installed by scripts/MacOS/Setup.sh)
        local brew = DesertPlatform.HomebrewPrefix
        if brew and os.isfile(brew .. "/include/vulkan/vulkan.h") then
            print("[Vulkan] Found via Homebrew: " .. brew)
            return { include = brew .. "/include", lib = brew .. "/lib" }
        end

        print("[Vulkan] Warning: Vulkan SDK not found! Run scripts/MacOS/Setup.sh")
        return nil
    end

    -- Linux fallback
    local sdk = "/usr/local/vulkan"
    print("[Vulkan] Trying default path for Linux: " .. sdk)
    if os.isdir(sdk) then
        return { include = sdk .. "/include", lib = sdk .. "/lib" }
    end
    return nil
end

local vulkan = findVulkanSDK()

-- A REFUSAL, NOT A WARNING, and the difference is the whole point. findVulkanSDK() used to return nil
-- after a `print("Warning: ...")`, getVulkanLibs() then returned an EMPTY library list, and premake
-- generated a complete, plausible workspace against no Vulkan at all. The failure surfaced hundreds
-- of compile errors later as "'vulkan/vulkan.h' file not found" and a wall of undefined symbols,
-- which reads as a broken checkout rather than as a missing SDK — and the one line that said so had
-- scrolled past long before. Nothing in this engine builds without Vulkan, so there is no
-- configuration this silence was serving.
if not vulkan then
    error( "Vulkan SDK not found.\n" ..
           "  macOS:   run scripts/MacOS/Setup.sh (Homebrew vulkan-headers + vulkan-loader + molten-vk +\n" ..
           "           shaderc), or install the LunarG SDK into ~/VulkanSDK.\n" ..
           "  Windows: run scripts\\Windows\\Setup.bat, which installs the pinned LunarG SDK and sets\n" ..
           "           VULKAN_SDK. A headers-only install is not enough: the engine links shaderc*.lib\n" ..
           "           out of the SDK's Lib directory (SPIRV-Cross is vendored)." )
end

-- Shader-toolchain + Vulkan libraries the engine links against.
local function getVulkanLibs(config)
    local libs = {}

    if not vulkan then
        return libs
    end

    print(string.format("[Vulkan] Getting libraries for config: %s", config))

    if os.target() == "windows" then
        local suffix = config:find("Debug") and "d" or ""

        table.insert(libs, vulkan.lib .. "/vulkan-1.lib")

        table.insert(libs, vulkan.lib .. "/shaderc" .. suffix .. ".lib")
        table.insert(libs, vulkan.lib .. "/shaderc_combined" .. suffix .. ".lib")
        table.insert(libs, vulkan.lib .. "/shaderc_util" .. suffix .. ".lib")

        -- spirv-cross is NOT the SDK's: the vendored project (BuildScripts/ThirdParty/SPIRVCross.lua), linked by name
        -- in Libraries below — the 1.3.296 SDK parser has no SPV_KHR_cooperative_matrix.

        -- shaderc_shared.lib is deliberately NOT here either: it is the IMPORT library of
        -- shaderc_shared.dll, and a static library (Desert) that merges it can hand the linker an
        -- import thunk for the very symbols shaderc_combined provides — making the packaged game
        -- depend on a DLL nobody ships. scripts/CI/CheckWindowsDlls.sh would refuse that binary.
        --
        -- OGLCompiler.lib is deliberately NOT here. It was a legacy glslang shim (OGLCompilersDLL,
        -- two process-attach stubs) that glslang 14 folded into the main library and stopped shipping,
        -- so the SDK this build pins to — 1.3.290 — has no such file and the link died on it. What it
        -- used to provide comes in through shaderc_combined, which bundles glslang and SPIRV-Tools.
    else
        -- Link by name (the lib dirs are set workspace-wide in PlatformMacOS.lua);
        -- gmake mangles absolute library paths into broken -l flags.
        -- No debug-suffixed binaries on unix-likes: same set in both configs.
        table.insert(libs, "vulkan")
        table.insert(libs, "shaderc_combined")   -- shaderc + glslang + SPIRV-Tools in one archive
    end

    return libs
end

-- reflect-cpp: the compiled-from-source project on EVERY platform (see
-- BuildScripts/ThirdParty/ReflectCpp.lua). Windows used to link a prebuilt
-- reflectcpp.lib of which only the Debug flavour was ever committed, so the
-- Release configuration could not link at all. Linking the project by name
-- also gives MSBuild the dependency edge the bare .lib path never carried.
local function getReflectCppLib(config)
    return "ReflectCpp"
end

-- GoogleTest: the compiled-from-source project on Windows (see
-- BuildScripts/ThirdParty/GoogleTest.lua — same story as reflect-cpp above);
-- elsewhere linked by name from the package-manager lib dirs (set
-- workspace-wide in PlatformMacOS.lua).
local function getGTestLib(config)
    if os.target() == "windows" then
        return "GoogleTest"
    end
    return "gtest"
end

local function getGTestIncludeDir()
    if os.target() == "windows" then
        return baseDir .. "/google-test/include"
    end
    return DesertPlatform.HomebrewPrefix and (DesertPlatform.HomebrewPrefix .. "/include") or "/usr/local/include"
end

-- EVERY LOOP OVER A DEPENDENCY TABLE GOES THROUGH THIS, NEVER THROUGH pairs(). The tables below are
-- keyed by name, and Lua seeds its string hash per process, so pairs() hands the keys out in a new order
-- on every premake run. That order IS the order of -isystem flags on clang and of EXTERNAL_INCLUDE on
-- MSVC, both of which ccache hashes: five premake runs over the same tree gave five orders and therefore
-- five cache keys for every engine object. Integer keys (a list such as getVulkanLibs' result) come first
-- in their own order, then names in byte order, so the generated project files are identical run to run.
-- NVIDIA STREAMLINE (DLSS Super Resolution), Windows only. The headers and licence are committed under
-- ThirdParty/Streamline; the runtime binaries are DOWNLOADED, never committed (owner 2026-10-15):
-- ThirdParty/Streamline/manifest.json pins the release and every file's SHA-256, and
-- scripts/Windows/FetchVendorBinaries.py (run by Setup.bat) fetches them into DesertStreamlineRuntimeDir. The engine
-- compiles against the headers (Desert/Desert/premake5.lua) and LOADS sl.interposer.dll at run time from the
-- executable's directory (Graphic/API/Vulkan/Streamline/StreamlineRuntime): nothing links it, so a machine without
-- it starts and reports DLSS unavailable by name. The manifest's Files ARE the list of files that run, plus NVIDIA's
-- redistribution licence the DLSS runtime requires shipped with it: this copies them next to every executable that
-- renders, and the game packager ships the same list beside the packaged game
-- (Editor/Source/Editor/Packaging/GamePackager.cpp), so the two can never disagree.
DesertStreamlineManifest = "ThirdParty/Streamline/manifest.json"
DesertStreamlineRuntimeDir = "ThirdParty/Streamline/bin/x64"

-- The runtime file names, in manifest order (premake must generate before the fetch has run, so the list comes from
-- the committed manifest, never from the directory).
function DesertStreamlineRuntimeFiles()
    local manifestPath = path.join(_MAIN_SCRIPT_DIR, DesertStreamlineManifest)
    local text = io.readfile(manifestPath)
    if text == nil then
        error(DesertStreamlineManifest .. " is missing: it pins the DLSS runtime the build copies")
    end
    local manifest, decodeError = json.decode(text)
    if manifest == nil then
        error(DesertStreamlineManifest .. ": " .. tostring(decodeError))
    end
    local names = {}
    for _, entry in ipairs(manifest.Files) do
        table.insert(names, path.getname(entry.Archive))
    end
    return names
end

-- Called inside an executable's project: copies the Streamline runtime into its target directory after the link
-- (Windows only). A file not fetched yet is not copied and the build says which one and how to fetch it: the
-- executable still runs and reports DLSS unavailable, which is the state of a machine without the files.
function DesertCopyStreamlineRuntime()
    filter "system:windows"
        for _, name in ipairs(DesertStreamlineRuntimeFiles()) do
            -- Backslashes: cmd's `if exist` is the one reader here that does not take forward slashes reliably.
            local source = path.translate(path.join(_MAIN_SCRIPT_DIR, DesertStreamlineRuntimeDir, name), "\\")
            -- Two plain `if` lines, no ( ) block: a parenthesis in the checkout path would close a block early.
            postbuildcommands {
                'if exist "' .. source .. '" {COPYFILE} "' .. source .. '" "%{cfg.targetdir}"',
                'if not exist "' .. source .. '" echo ' .. DesertStreamlineRuntimeDir .. '/' .. name ..
                    ' is not fetched, DLSS will be unavailable: run python scripts/Windows/FetchVendorBinaries.py',
            }
        end
    filter {}
end

-- INTEL OPEN IMAGE DENOISE 2.x (the path tracer's PathTracing.Denoiser OIDN), Windows x64 and macOS arm64. The
-- headers and the Apache-2.0 licence are committed under ThirdParty/OIDN; the runtime libraries are DOWNLOADED from
-- Intel's release, never committed: ThirdParty/OIDN/manifest.<platform>.json pins the archive and every file's
-- SHA-256 and scripts/Windows/FetchVendorBinaries.py (Setup.bat, scripts/MacOS/Setup.sh) fetches them. The engine
-- compiles against the headers and LOADS the library at run time (Engine/Graphic/Denoise/OidnDenoiser.cpp): Windows
-- OpenImageDenoise.dll beside the executable, macOS lib/libOpenImageDenoise.2.dylib under it (the dylibs find each
-- other through their own @loader_path/../lib rpath, which is their own directory only when it is named lib). OIDN
-- loads its device modules (CPU, CUDA, HIP, SYCL, Metal) from its own directory. Like UE's OpenImageDenoise plugin
-- it is an editor / movie-render tool: copied beside the editor and the runtime (whose dev build renders movies),
-- not shipped by the game packager.
DesertOidnManifests = {
    windows = { manifest = "ThirdParty/OIDN/manifest.windows.json", dir = "ThirdParty/OIDN/bin/x64" },
    macosx  = { manifest = "ThirdParty/OIDN/manifest.macos.json", dir = "ThirdParty/OIDN/bin/macos-arm64/lib" },
}

local function DesertManifestFileNames(manifestRelative)
    local text = io.readfile(path.join(_MAIN_SCRIPT_DIR, manifestRelative))
    if text == nil then
        error(manifestRelative .. " is missing: it pins the runtime the build copies")
    end
    local manifest, decodeError = json.decode(text)
    if manifest == nil then
        error(manifestRelative .. ": " .. tostring(decodeError))
    end
    local names = {}
    for _, entry in ipairs(manifest.Files) do
        table.insert(names, path.getname(entry.Archive))
    end
    return names
end

-- Called inside an executable's project: copies the OIDN runtime beside its target after the link. A file not
-- fetched yet is not copied and the build names it: the executable runs and PathTracing.Denoiser OIDN reports the
-- library missing by name.
function DesertCopyOidnRuntime()
    filter "system:windows"
        for _, name in ipairs(DesertManifestFileNames(DesertOidnManifests.windows.manifest)) do
            local source = path.translate(path.join(_MAIN_SCRIPT_DIR, DesertOidnManifests.windows.dir, name), "\\")
            postbuildcommands {
                'if exist "' .. source .. '" {COPYFILE} "' .. source .. '" "%{cfg.targetdir}"',
                'if not exist "' .. source .. '" echo ' .. DesertOidnManifests.windows.dir .. '/' .. name ..
                    ' is not fetched, PathTracing.Denoiser OIDN will be unavailable: run python scripts/Windows/FetchVendorBinaries.py',
            }
        end
    filter "system:macosx"
        postbuildcommands { 'mkdir -p "%{cfg.targetdir}/lib"' }
        for _, name in ipairs(DesertManifestFileNames(DesertOidnManifests.macosx.manifest)) do
            local source = path.join(_MAIN_SCRIPT_DIR, DesertOidnManifests.macosx.dir, name)
            postbuildcommands {
                'if [ -f "' .. source .. '" ]; then cp -f "' .. source .. '" "%{cfg.targetdir}/lib/"; else echo ' ..
                    DesertOidnManifests.macosx.dir .. '/' .. name ..
                    ' is not fetched, PathTracing.Denoiser OIDN will be unavailable: run python3 scripts/Windows/FetchVendorBinaries.py; fi',
            }
        end
    filter {}
end

function DesertSortedPairs(t)
    local keys = {}
    for key in pairs(t) do
        table.insert(keys, key)
    end
    table.sort(keys, function(a, b)
        local ta, tb = type(a), type(b)
        if ta ~= tb then
            return ta == "number"
        end
        return a < b
    end)
    local i = 0
    return function()
        i = i + 1
        local key = keys[i]
        if key ~= nil then
            return key, t[key]
        end
    end
end

-- EVERY PATH IN THESE `IncludeDir` TABLES IS THIRD-PARTY, AND THAT IS NOW LOAD-BEARING RATHER THAN
-- INCIDENTAL. Each consumer declares them with `externalincludedirs`, which is `-isystem` on clang and
-- `/external:I` on MSVC, so `externalwarnings "Off"` (BuildScripts/Workspace.lua) keeps vendored headers
-- out of our diagnostics without a single `-Wno-` anywhere. Put a first-party path in here and you have
-- silently switched the compiler off for it — `desert-shared` used to sit in `Common` and was moved to a
-- plain `includedirs` in Workspace.lua for exactly that reason.
Dependencies = {
    Common = {
        IncludeDir = {
            spdlog = baseDir .. "/spdlog/include",
            glm = baseDir .. "/glm",
            GLFW = baseDir .. "/GLFW/include",
            optick = baseDir .. "/optick/src",
        },
        Defines = {
            -- Optick: keep these identical to BuildScripts/ThirdParty/Optick.lua so optick.h compiles the
            -- same way in every consumer (CPU profiling only).
            "USE_OPTICK=1",
            "OPTICK_ENABLE_GPU=0",
            "OPTICK_ENABLE_TRACING=0",
        }
    },

    CommonSpecific = {
        IncludeDir = {
            reflect_cpp = baseDir .. "/reflect-cpp/include",
        },
        Libraries = {
            reflect_cpp = getReflectCppLib("Debug"),
        },
        Defines = {
        }
    },

    DesertSpecific = {
        IncludeDir = {
            base = baseDir,
            stb = baseDir .. "/stb/include",
            vkallocator = baseDir .. "/VulkanAllocator",
            entt = baseDir .. "/entt/include",
            reflect_cpp = baseDir .. "/reflect-cpp/include",
            meshoptimizer = baseDir .. "/meshoptimizer/src",
            -- <opensubdiv/far/...>: the CPU refiner the Subdivide port runs on (BuildScripts/ThirdParty/OpenSubdiv.lua).
            opensubdiv = baseDir .. "/OpenSubdiv",
            -- <embree4/rtcore.h>: the distance-field builder (BuildScripts/ThirdParty/Embree.lua); its configured
            -- rtcore_config.h lives beside the build script, not in the submodule.
            embree = baseDir .. "/embree/include",
            embree_config = baseDir .. "/../BuildScripts/ThirdParty/EmbreeConfig/include/embree4",
            jolt = baseDir .. "/JoltPhysics",
            lua = baseDir .. "/lua",
            Vulkan = vulkan and vulkan.include or nil,
            shaderc = vulkan and (vulkan.include .. "/shaderc") or nil,
            -- The vendored SPIRV-Cross (BuildScripts/ThirdParty/SPIRVCross.lua), included FLAT (<spirv_glsl.hpp>): the
            -- SDK's include dir carries its own older spirv_cross/ and comes first, so <spirv_cross/...> would pick it.
            spirv_cross = baseDir .. "/SPIRV-Cross",
        },
        Libraries = {
            Debug = {
                reflect_cpp = getReflectCppLib("Debug"),
                spirv_cross = "SPIRVCross",
                getVulkanLibs("Debug")
            },
            Release = {
                reflect_cpp = getReflectCppLib("Release"),
                spirv_cross = "SPIRVCross",
                getVulkanLibs("Release")
            }
        },
        Defines = {
        }
    },

    TestSpecific = {
        IncludeDir = {
            gtest = getGTestIncludeDir(),
            reflect_cpp = baseDir .. "/reflect-cpp/include",
            -- Shader headers compiled as C++ (LandscapeHeight.glslh via LandscapeData.cpp, and the rest of
            -- Shaders/Common). Every suite gets it because the partitioner reaches LandscapeData.cpp, and a
            -- per-suite line was forgotten four times in one day (2026-09-24).
            desert_shaders = "%{_MAIN_SCRIPT_DIR}/Editor/Resources/Shaders",
        },
        Libraries = {
            Debug = {
                reflect_cpp = getReflectCppLib("Debug"),
                gtest = getGTestLib("Debug"),
            },
            Release = {
                reflect_cpp = getReflectCppLib("Release"),
                gtest = getGTestLib("Release"),
            }
        },
        -- THE ROOTS A SUITE READS, BAKED IN AS ABSOLUTE PATHS (UE: FPaths::EngineDir): the engine directory it
        -- hands to SetEngineDir (TestSupport/engine_dir.hpp), the checkout, and the suite data tree
        -- Desert/Tests/Data. A suite never finds them from its working directory.
        Defines = {
            "GTEST",
            "TESTING",
            'DESERT_TEST_ENGINE_DIR="' .. path.getabsolute(_MAIN_SCRIPT_DIR .. "/Editor") .. '"',
            'DESERT_TEST_REPO_ROOT="' .. path.getabsolute(_MAIN_SCRIPT_DIR) .. '"',
            'DESERT_TEST_DATA_DIR="' .. path.getabsolute(_MAIN_SCRIPT_DIR .. "/Desert/Tests/Data") .. '"',
        },
        EnvVars = {
            GTEST_OUTPUT = "xml:%{_MAIN_SCRIPT_DIR}/build/TestReports/%{prj.name}.xml",
            TEST_REPORTS_DIR = "%{_MAIN_SCRIPT_DIR}/build/TestReports"
        }
    }
}

return Dependencies
