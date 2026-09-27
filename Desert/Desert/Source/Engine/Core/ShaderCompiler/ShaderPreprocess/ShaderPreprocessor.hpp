#pragma once

#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>

// This header leaned on the engine PCH for these — which broke every PCH-less consumer (the offline
// cook and the packaging tests compile this TU directly).
#include <filesystem>
#include <string>
#include <unordered_map>

namespace Desert::Core::Preprocess
{
    class ShaderPreprocess
    {
    public:
        static std::unordered_map<Core::Formats::ShaderStage, std::string>
        PreProcessProgram( const std::string& source, const std::filesystem::path& basePath );

        // Parses program-level `#pragma param` / `#pragma state` lines from the .shader source into the
        // data-driven material metadata. Additive: a shader with no such pragmas yields an empty meta.
        static Core::Formats::ShaderProgramMeta ParseProgramMeta( const std::string& source );

        // Pass-aware variants for DSL multi-pass shaders. passName selects a `Pass "Name"` block;
        // an empty name means the default program. The legacy #pragma format only has the default
        // program — a non-empty passName on it is an error.
        static std::unordered_map<Core::Formats::ShaderStage, std::string>
        PreProcessProgramPass( const std::string& source, const std::filesystem::path& basePath,
                               const std::string& passName );

        static Core::Formats::ShaderProgramMeta ParseProgramMetaForPass( const std::string& source,
                                                                         const std::string& passName );

        // Both of the above from ONE parse. A shader-map miss needs the pass's metadata and its stages
        // together, and asking the two functions separately parsed the whole DShader twice — half of
        // the cold start's preprocess time (AL1-12a).
        struct PreprocessedPass
        {
            Core::Formats::ShaderProgramMeta                            Meta;
            std::unordered_map<Core::Formats::ShaderStage, std::string> Stages;
        };
        static PreprocessedPass PreProcessPass( const std::string& source, const std::filesystem::path& basePath,
                                                const std::string& passName );
    };
} // namespace Desert::Core::Preprocess