#pragma once

#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>
#include <vector>
#include <cstdint>
#include <string>

namespace Desert::Core
{
    /**
     * @brief Specialized class for compiling GLSL shader source to SPIR-V.
     */
    class ShaderCompiler
    {
    public:
        /**
         * @brief Compiles GLSL source to SPIR-V.
         *
         * @param stage The shader stage (Vertex, Fragment, Compute).
         * @param source The GLSL source code.
         * @param shaderPath The path to the shader file (for error reporting and includes).
         * @param variant Include targets whose bytes the CALLER supplies — see ShaderVariant.hpp. Part
         *        of the cache key, so two variants of one stage are two artifacts and never one.
         * @return Common::ResultStr<std::vector<uint32_t>> The compiled SPIR-V binary or an error message.
         */
        static Common::ResultStr<std::vector<uint32_t>> CompileGLSLToSPIRV( Formats::ShaderStage stage,
                                                                            const std::string&   source,
                                                                            const std::string&   shaderPath,
                                                                            const ShaderVariant& variant = {} );

        /**
         * @brief The same compile for an EXPLICIT SPIR-V debug-info profile.
         *
         * The overload above compiles the way THIS build runs (debug info in Debug). The game
         * packager instead cooks for the runtime the player will launch — a Debug editor packaging a
         * Release game must produce Release artifacts under Release cache keys, or the shipped cache
         * never hits. Pass Core::SpirvDebugInfoForConfigName(<target config>) here.
         */
        static Common::ResultStr<std::vector<uint32_t>>
        CompileGLSLToSPIRVForProfile( Formats::ShaderStage stage, const std::string& source,
                                      const std::string& shaderPath, bool spirvDebugInfo,
                                      const ShaderVariant& variant = {} );
    };

} // namespace Desert::Core
