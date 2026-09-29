#pragma once

// Text -> shader map (metadata + SPIR-V of every stage), the CPU half of building a shader program.
//
// WHY IT IS ITS OWN STEP: VulkanShader::Reload used to do this and then create the VkShaderModules in one
// call, so a cold start compiled its programs one after another on the main thread (SHC1: ~80 programs,
// every one a shaderc run). This half touches no device and no AssetManager, so it can run on the job
// system; the device half (BuildFromSpirv) stays on the main thread, in the caller's order.

#include <Engine/Core/ShaderCompiler/ShaderMapCache.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Desert::Core
{
    // One program to build: the asset's text, the file it came from (includes resolve against it), the pass
    // (empty = the default program), the variant, and the name the logs call it by.
    struct ShaderMapRequest
    {
        std::string           Source;
        std::filesystem::path Path;
        std::string           PassName;
        ShaderVariant         Variant;
        std::string           Name;
    };

    // Reads the shader map cache first; on a miss preprocesses, compiles every stage (through the SPIR-V
    // cache) and stores the map. Safe to call from a worker thread.
    Common::ResultStr<ShaderMap> BuildShaderMap( const ShaderMapRequest& request );

    // One request's answer: the map, or why there is none (Error non-empty).
    struct ShaderMapOutcome
    {
        ShaderMap   Map;
        std::string Error;
    };

    // BuildShaderMap for every request on the job system. The answer for requests[i] is outcomes[i] —
    // written by index, never appended — so the result does not depend on which worker finished first.
    std::vector<ShaderMapOutcome> BuildShaderMaps( std::span<const ShaderMapRequest> requests );
} // namespace Desert::Core
