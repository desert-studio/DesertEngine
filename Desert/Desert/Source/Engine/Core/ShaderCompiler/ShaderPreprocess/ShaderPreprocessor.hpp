#pragma once

#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>

#include <Common/Core/ResultStr.hpp>

// This header leaned on the engine PCH for these — which broke every PCH-less consumer (the offline
// cook and the packaging tests compile this TU directly).
#include <filesystem>
#include <string>
#include <unordered_map>

namespace Desert::Core::Preprocess
{
    // EVERY ANSWER IS A RESULT NAMING THE SHADER. A .shader that is not DSL text, does not parse, or has no pass
    // of the asked name is refused with @p basePath (the file) and the reason — never a DESERT_VERIFY: the answer
    // travels BuildShaderMap -> Shader::Build -> ShaderService::Register (registered, not compiled, named in the
    // log), and a required engine shader turns it into a refused start (CompileEngineShaders), not a SIGTRAP on
    // a job-system worker.
    class ShaderPreprocess
    {
    public:
        // Pass-aware preprocessing for DSL multi-pass shaders. passName selects a `Pass "Name"` block; an empty
        // name means the default program. A medium-only shader (no passes) answers no stages.
        static Common::ResultStr<std::unordered_map<Core::Formats::ShaderStage, std::string>>
        PreProcessProgramPass( const std::string& source, const std::filesystem::path& basePath,
                               const std::string& passName );

        static Common::ResultStr<Core::Formats::ShaderProgramMeta>
        ParseProgramMetaForPass( const std::string& source, const std::filesystem::path& basePath,
                                 const std::string& passName );

        // Both of the above from ONE parse. A shader-map miss needs the pass's metadata and its stages
        // together, and asking the two functions separately parsed the whole DShader twice — half of
        // the cold start's preprocess time (AL1-12a).
        struct PreprocessedPass
        {
            Core::Formats::ShaderProgramMeta                            Meta;
            std::unordered_map<Core::Formats::ShaderStage, std::string> Stages;
        };
        static Common::ResultStr<PreprocessedPass> PreProcessPass( const std::string&           source,
                                                                   const std::filesystem::path& basePath,
                                                                   const std::string&           passName );
    };
} // namespace Desert::Core::Preprocess