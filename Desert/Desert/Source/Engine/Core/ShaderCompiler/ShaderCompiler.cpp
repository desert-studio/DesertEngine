#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>
#include "ShaderCompiler.hpp"
#include <Engine/Core/ShaderCompiler/Includer/ShaderIncluder.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <shaderc/shaderc.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <future>
#include <mutex>
#include <unordered_map>
#include <fstream>
#include <optional>

namespace Desert::Core
{
    namespace
    {
        static shaderc_shader_kind ConvertShaderStage( Formats::ShaderStage stage )
        {
            switch ( stage )
            {
                case Formats::ShaderStage::Vertex:         return shaderc_vertex_shader;
                case Formats::ShaderStage::TessControl:    return shaderc_tess_control_shader;
                case Formats::ShaderStage::TessEvaluation: return shaderc_tess_evaluation_shader;
                case Formats::ShaderStage::Fragment:       return shaderc_fragment_shader;
                case Formats::ShaderStage::Compute:        return shaderc_compute_shader;
                default:
                    DESERT_VERIFY( false, "Unsupported shader stage for compilation" );
                    return (shaderc_shader_kind)0;
            }
        }
    } // namespace

    // ---- SPIR-V disk cache -----------------------------------------------------------------------
    // Content-addressed: the key hashes the assembled stage source PLUS the content of every
    // (recursively) included file — Core::ComputeShaderCacheKey, shared with the hot-reload
    // watcher so "what this stage is made of" has exactly one definition. Artifact location and
    // (VFS-aware) load/store live in ShaderSpirvCache — the seam the game packager cooks into and a
    // packaged game reads back out of its mounted archive.

    Common::ResultStr<std::vector<uint32_t>> ShaderCompiler::CompileGLSLToSPIRV( Formats::ShaderStage stage,
                                                                                 const std::string&   source,
                                                                                 const std::string&   shaderPath,
                                                                                 const ShaderVariant& variant )
    {
        return CompileGLSLToSPIRVForProfile( stage, source, shaderPath, SpirvDebugInfoThisBuild(), variant );
    }

    Common::ResultStr<std::vector<uint32_t>>
    ShaderCompiler::CompileGLSLToSPIRVForProfile( Formats::ShaderStage stage, const std::string& source,
                                                  const std::string& shaderPath, bool spirvDebugInfo,
                                                  const ShaderVariant& variant )
    {
        // Cache key: stage + compile-options fingerprint (incl. the debug-info profile) + assembled
        // source + every included file's content (recursive) + the VARIANT's substituted bytes.
        // Content-addressed, so any edit produces a fresh key — no mtime races — and two materials
        // substituting two different cloud media are two artifacts rather than one served twice.
        uint64_t key = 0;
        {
            const ScopedShaderPhase timer( ShaderPhase::CacheKey );
            key = ComputeShaderCacheKeyForProfile( stage, source, shaderPath, spirvDebugInfo, variant );
        }

        std::optional<std::vector<uint32_t>> cached;
        {
            const ScopedShaderPhase timer( ShaderPhase::SpirvLoad );
            cached = TryLoadCachedSpirv( key );
        }
        if ( cached )
        {
            CountShaderCacheHit();
            return Common::MakeSuccess( std::move( *cached ) );
        }

        // ONE COMPILE PER KEY IN FLIGHT. Without debug info the key does not name the file, so the same stage
        // text in two programs is one key; two workers compiling it at once would both write that key's one
        // <file>.tmp. The first caller compiles and stores; a caller arriving meanwhile waits for its answer.
        using CompileResult = Common::ResultStr<std::vector<uint32_t>>;
        static std::mutex                                                      inFlightMutex;
        static std::unordered_map<uint64_t, std::shared_future<CompileResult>> inFlight;
        std::promise<CompileResult>                                            owned;
        std::shared_future<CompileResult>                                      another;
        {
            const std::lock_guard lock( inFlightMutex );
            const auto [entry, inserted] = inFlight.try_emplace( key );
            if ( inserted )
                entry->second = owned.get_future().share();
            else
                another = entry->second;
        }
        if ( another.valid() )
        {
            CountShaderCacheHit();
            return another.get();
        }
        CompileResult result = [&]() -> CompileResult
        {
            const ScopedShaderPhase        compileTimer( ShaderPhase::Compile );
            static const shaderc::Compiler compiler;
            shaderc::CompileOptions        options;

            options.SetIncluder( std::make_unique<ShaderIncluder>( shaderPath, variant ) );
            // THE OLDEST ACCEPTED DEVICE, NOT THE CURRENT ONE. SPIR-V is cached on disk and shipped, so its
            // target cannot follow the GPU of the machine that compiled it; it follows DeviceCaps' minimum,
            // which every device that passes CheckRequired meets. Raising it is VKF2's call (with the minimum).
            static_assert( Graphic::API::Vulkan::kMinimumDeviceApiVersion == VK_API_VERSION_1_1,
                           "the shader target below must move with kMinimumDeviceApiVersion" );
            options.SetTargetEnvironment( shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1 );
            options.SetWarningsAsErrors();

            // NO SetOptimizationLevel, measured (PSO2, 2026-09-27, 126 shaders of the SPIR-V cache). shaderc's
            // performance/size levels without debug info add StripDebugInfo: every OpName/OpMemberName goes, so
            // spirv-cross reflects blocks as `_20` and members as `_m0`, and VulkanShaderReflection binds by
            // name. Dead-variable elimination also drops declared-but-unread bindings (3 in 2 shaders), which
            // changes the reflected set layout. The gain did not pay for either: code 1.00 -> 0.64 MB (names
            // stripped both ways), Metal front-end compile of the whole set 8.4 -> 8.1 s (within run-to-run
            // noise). Worth revisiting when reflection stops depending on names, or when a target's driver
            // shows a measured pipeline-creation win on optimized SPIR-V.
            if ( spirvDebugInfo )
                options.SetGenerateDebugInfo();

            const auto compileStart = std::chrono::steady_clock::now();

            const shaderc::SpvCompilationResult result =
                 compiler.CompileGlslToSpv( source, ConvertShaderStage( stage ), shaderPath.c_str(), options );

            if ( result.GetCompilationStatus() != shaderc_compilation_status_success )
            {
                std::string errorMsg = std::format( "Shader Compilation Error ({}): {}\nFile: {}",
                                                    Graphic::Shader::GetStringShaderStage( stage ),
                                                    result.GetErrorMessage(), shaderPath );
                // "{}", not the message itself: a shaderc diagnostic quotes the offending GLSL, so it can
                // contain braces, and passing it as a FORMAT string would throw while reporting a compile error.
                LOG_ERROR( "{}", errorMsg );
                return Common::MakeError<std::vector<uint32_t>>( errorMsg );
            }

            // A cache MISS is the event worth a line: a warm start compiles nothing, so every one of these
            // at startup is time the cook (or a previous run) should have paid already. Hits stay silent —
            // they are the normal case and there are hundreds of them.
            const auto compileMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - compileStart )
                                        .count();
            LOG_INFO( "[ShaderCompiler] cache miss {:016x}: compiled {} [{}] in {} ms", key, shaderPath,
                      Graphic::Shader::GetStringShaderStage( stage ), compileMs );

            std::vector<uint32_t> spirv( result.begin(), result.end() );
            // A failed store is not fatal to THIS compile, but it keeps the cache cold: the next start pays
            // the same compile again. Say so, with the path and the file system's reason.
            const auto stored = StoreCachedSpirv( key, spirv );
            if ( !stored )
                LOG_WARN( "[ShaderCache] could not store {}: {}", SpirvCachePathForKey( key ).string(),
                          stored.GetError() );
            CountShaderCacheCompile( static_cast<bool>( stored ) );
            return Common::MakeSuccess( std::move( spirv ) );
        }();
        owned.set_value( result );
        {
            const std::lock_guard lock( inFlightMutex );
            inFlight.erase( key );
        }
        return result;
    }

} // namespace Desert::Core
