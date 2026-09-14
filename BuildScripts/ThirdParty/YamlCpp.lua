-- yaml-cpp static library, built from the ThirdParty/yaml-cpp submodule sources.
-- (Previously this file was generated on the fly by a Python pre-build step, since deleted;
-- it is now a committed config so all platforms build the same way.)

local root = _MAIN_SCRIPT_DIR .. "/ThirdParty/yaml-cpp"

project "yaml-cpp"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location ( _MAIN_SCRIPT_DIR .. "/build/Projects/" .. "YamlCpp" )

    files
    {
        root .. "/src/**.cpp",
        root .. "/src/**.h",
        root .. "/include/**.h",
    }

    includedirs { root .. "/include" }

    defines { "YAML_CPP_STATIC_DEFINE" }

    filter "system:windows"
        systemversion "latest"

    filter "system:not windows"
        pic "On"

    filter {}
