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
#include <string_view>
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

    // THE SHARED-SAMPLER RULE (SharedSamplerBinding.hpp), applied to one program's compiled stages in place:
    // every sampler the DSL marked with kAutoSamplerBinding gets, per descriptor set, the bindings after the last
    // one any resource of any stage holds, in name order, one number per name across the stages. Refuses, naming
    // @p shaderName: a sampler on a binding another resource holds, a marked sampler no stage samples, a resource
    // left on the marker, a sampler without a name. Device-free: it reads and patches the SPIR-V words.
    Common::BoolResultStr AssignSharedSamplerBindings( std::vector<ShaderMapStage>& stages,
                                                       std::string_view             shaderName );

    // THE PROGRAM NUMBERS THE UNNUMBERED (SharedSamplerBinding.hpp kAutoResourceBinding), in place, after
    // CheckProgramSlots and before AssignSharedSamplerBindings: every resource the DSL marked takes, for all
    // stages, the slot an explicit declaration of its name holds in another stage, else per descriptor set the
    // bindings after the last explicit one, in name order. Explicit numbers never move. Refuses, naming
    // @p shaderName: one marked name in two sets, a marked name whose explicit slot is in another set, a number
    // that would reach the marker. Device-free: it reads and patches the SPIR-V words.
    Common::BoolResultStr AssignProgramBindings( std::vector<ShaderMapStage>& stages,
                                                 std::string_view             shaderName );

    // ONE PROGRAM, ONE NUMBERING (GPU-SHADER-AUTOBIND). Each stage is compiled alone, and a resource or varying
    // declared without a number takes the lowest free one of ITS stage (ShaderCompiler.cpp), so the stages of
    // one program can disagree. Refuses, naming @p shaderName and both stages: one resource name on two slots,
    // two resource names on one slot (a binding collision), and one varying name at two locations across a
    // graphics stage boundary. Device-free: it reads the SPIR-V words; both markers are skipped.
    Common::BoolResultStr CheckProgramSlots( const std::vector<ShaderMapStage>& stages,
                                             std::string_view                   shaderName );

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
