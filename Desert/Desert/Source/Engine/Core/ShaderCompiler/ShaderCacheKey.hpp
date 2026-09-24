#pragma once

// What a compiled shader is MADE OF — the include closure of a stage — and the content hash derived
// from it.
//
// One definition, two consumers, and that is the whole point of the file existing:
//
//   * Core::ShaderCompiler keys its SPIR-V disk cache on this hash. A key that does not cover an
//     included file is the worst kind of failure there is: the machine that has the stale artifact
//     renders differently from the machine that does not, and neither reports anything.
//   * Runtime::AssetHotReload watches these files for edits. A `.glslh` is not an asset and has no
//     mtime anybody was tracking, so editing one used to change nothing until the next restart — the
//     same staleness as an under-specified cache key, arriving through the other door.
//
// Both questions are "which files does this stage's SPIR-V depend on", so both are answered here.

#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/ShaderCompiler/ShaderVariant.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    /**
     * Every file the assembled stage @p source pulls in, transitively, in the order first seen and
     * without duplicates.
     *
     * Mirrors ShaderIncluder's resolution exactly: `#include "x"` is relative to the including file,
     * `#include <x>` is relative to the engine shader root. A path that does not resolve is left OUT
     * rather than guessed at — a missing include is a compile error the compiler will report with a
     * better message than this walk could.
     *
     * A VIRTUAL include is followed through the variant's bytes rather than through the file's, so a
     * header pulled in only by a substituted body is still found — and it is still listed under its own
     * path, because that is what the hot-reload watcher has to watch and what the key has to hash.
     *
     * @param requestingFile the file @p source came from; the anchor for quoted includes.
     * @param variant        the compile-time substitution this stage is being compiled under.
     */
    std::vector<std::filesystem::path> CollectShaderIncludes( const std::string&           source,
                                                              const std::filesystem::path& requestingFile,
                                                              const ShaderVariant&         variant = {} );

    /**
     * The SPIR-V disk-cache key: stage + compile-options fingerprint + the assembled source + the
     * content of every file CollectShaderIncludes finds + THE VARIANT.
     *
     * Content-addressed on purpose — no mtimes, so a checkout that restores an older file is a
     * different key rather than a same-key-newer-timestamp, and two machines with the same tree agree.
     *
     * THE VARIANT IS PART OF THE KEY AND HAS TO BE. Two materials authoring two different cloud media
     * compile the SAME stage of the SAME file against the same headers on disk; only the substituted
     * bytes differ. Leaving them out would make the cache answer a question it was not asked — the
     * first variant to compile would be served to every other one, for ever, across restarts, and the
     * symptom is "my graph does nothing" rather than anything that names a cache. Desert/Tests/Engine/
     * ShaderCacheKey drives exactly that: same name, different bytes, the key must move.
     *
     * The DEFAULT variant mixes nothing at all, which is what keeps every artifact already on disk
     * valid instead of invalidating the whole cache on the day this axis was added.
     *
     * The options fingerprint includes whether SPIR-V debug info is generated, and THIS overload asks
     * with the current build's own policy — what the engine does at runtime.
     */
    uint64_t ComputeShaderCacheKey( Formats::ShaderStage stage, const std::string& source,
                                    const std::filesystem::path& requestingFile,
                                    const ShaderVariant&         variant = {} );

    /**
     * The same key for an EXPLICIT debug-info profile — what the game packager asks with, because it
     * cooks for the runtime the player will launch, not for the editor doing the cooking: a Debug
     * editor packaging a Release runtime must produce Release keys or the shipped cache is dead on
     * arrival.
     */
    uint64_t ComputeShaderCacheKeyForProfile( Formats::ShaderStage stage, const std::string& source,
                                              const std::filesystem::path& requestingFile, bool spirvDebugInfo,
                                              const ShaderVariant& variant = {} );

    /**
     * The SHADER MAP key (ShaderMapCache.hpp): computed over the program's RAW text — no DShader parse —
     * plus the path and content hash of every file its text includes (transitively, plus the headers the
     * parser injects), the pass, the variant and the debug-info profile. A hit therefore needs neither
     * the parse nor the preprocessing the SPIR-V key needs. Include files are read once per process and
     * re-read when their size or write time changes.
     */
    uint64_t ComputeShaderMapKey( const std::string& programSource, const std::filesystem::path& programPath,
                                  const std::string& passName, bool spirvDebugInfo,
                                  const ShaderVariant& variant = {} );

    /** The debug-info policy of THIS build — the profile the 3-argument overloads resolve to. */
    bool SpirvDebugInfoThisBuild();

    /**
     * The policy of the build a configuration NAME ("Debug" / "Release") produces. The packager maps
     * its target-config choice through this so the cook and the shipped runtime cannot disagree; it
     * must therefore mirror the #ifdef the engine compiles under, and a test pins the two together.
     */
    bool SpirvDebugInfoForConfigName( std::string_view configName );
} // namespace Desert::Core
