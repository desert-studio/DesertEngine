local baseDir = "%{wks.location}/ThirdParty"

-- ============================================================================
-- Vulkan SDK discovery (platform-aware).
-- Returns a table { include = <dir>, lib = <dir> } or nil.
--   Windows: LunarG SDK layout  <sdk>/Include, <sdk>/Lib
--   macOS:   LunarG SDK layout  $VULKAN_SDK/include, $VULKAN_SDK/lib
--            or Homebrew (vulkan-headers + vulkan-loader + molten-vk + shaderc + spirv-cross)
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
           "           shaderc + spirv-cross), or install the LunarG SDK into ~/VulkanSDK.\n" ..
           "  Windows: run scripts\\Windows\\Setup.bat, which installs the pinned LunarG SDK and sets\n" ..
           "           VULKAN_SDK. A headers-only install is not enough: the engine links shaderc*.lib\n" ..
           "           and spirv-cross-*.lib out of the SDK's Lib directory." )
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
        table.insert(libs, vulkan.lib .. "/shaderc_shared" .. suffix .. ".lib")
        table.insert(libs, vulkan.lib .. "/shaderc_combined" .. suffix .. ".lib")
        table.insert(libs, vulkan.lib .. "/shaderc_util" .. suffix .. ".lib")

        table.insert(libs, vulkan.lib .. "/spirv-cross-core" .. suffix .. ".lib")
        table.insert(libs, vulkan.lib .. "/spirv-cross-glsl" .. suffix .. ".lib")

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
        table.insert(libs, "spirv-cross-glsl")
        table.insert(libs, "spirv-cross-core")
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
            yaml_cpp = baseDir .. "/yaml-cpp/include",
            glm = baseDir .. "/glm",
            GLFW = baseDir .. "/GLFW/include",
            optick = baseDir .. "/optick/src",
        },
        Libraries = {
            yaml_cpp = "yaml-cpp"
        },
        Defines = {
            "YAML_CPP_STATIC_DEFINE",
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
            imgui = baseDir .. "/stb/ImGui",
            entt = baseDir .. "/entt/include",
            reflect_cpp = baseDir .. "/reflect-cpp/include",
            meshoptimizer = baseDir .. "/meshoptimizer/src",
            jolt = baseDir .. "/JoltPhysics",
            lua = baseDir .. "/lua",
            sol2 = baseDir .. "/sol2/include",
            Vulkan = vulkan and vulkan.include or nil,
            shaderc = vulkan and (vulkan.include .. "/shaderc") or nil,
            spirv_cross = vulkan and (vulkan.include .. "/spirv_cross") or nil,
        },
        Libraries = {
            Debug = {
                reflect_cpp = getReflectCppLib("Debug"),
                getVulkanLibs("Debug")
            },
            Release = {
                reflect_cpp = getReflectCppLib("Release"),
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
        Defines = {
            "GTEST",
            "TESTING"
        },
        EnvVars = {
            GTEST_OUTPUT = "xml:%{wks.location}/build/TestReports/%{prj.name}.xml",
            TEST_REPORTS_DIR = "%{wks.location}/build/TestReports"
        }
    }
}

return Dependencies
