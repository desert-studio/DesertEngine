#pragma once

// A program's derived data under ONE key: its metadata (parameters, render state, pass names, domain,
// medium) and the SPIR-V of every stage. Modelled on UE's FShaderMap in the DDC (ShaderCompiler/
// ShaderMap lookups keyed by the source files' hashes, Engine/Private/ShaderCompiler), adapted: one
// program per entry, our 64-bit FNV key (ComputeShaderMapKey) instead of an FSHAHash, the SPIR-V cache's
// DDC store instead of the UE cache hierarchy.
//
// WHY IT EXISTS: the SPIR-V cache is keyed by the PARSED stage text, so every start had to parse every
// DShader (two parses per program plus one per registration) before it could even ask the cache. The
// measured warm start was 64 % parsing. This entry is keyed by the RAW text and the include files, so a
// hit needs no parse at all.

#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>

#include <Common/Core/ResultStr.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    struct ShaderMapStage
    {
        Formats::ShaderStage  Stage = Formats::ShaderStage::Vertex;
        std::vector<uint32_t> Spirv;

        bool operator==( const ShaderMapStage& ) const = default;
    };

    struct ShaderMap
    {
        Formats::ShaderProgramMeta  Meta;
        std::vector<ShaderMapStage> Stages; // ascending by stage value, so two builds of one program agree

        bool operator==( const ShaderMap& ) const = default;
    };

    // The blob's own version: the reader refuses any other. Bump it when the byte layout changes.
    inline constexpr uint32_t kShaderMapFormatVersion = 1;

    // The key hashes the shader's TEXT, not the code that turns text into a map, so a change to the parser,
    // the preprocessor or the metadata types would keep serving maps the old code produced. This is the
    // fingerprint of that code (kShaderMapProducerSources, whitespace and comments stripped); it is part of
    // the deriver's version, so re-recording it moves every key. ShaderCacheKey's
    // TheShaderMapProducerFingerprintIsRecorded computes it from the files and prints the value to paste.
    inline constexpr uint64_t kShaderMapProducerFingerprint = 0xbe74aa9d73afa845ULL;

    // Repository-relative. ShaderMapCache.hpp is not listed: it holds the fingerprint itself.
    inline constexpr std::array<std::string_view, 8> kShaderMapProducerSources{
         "Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp",
         "Desert/Desert/Source/Engine/Core/ShaderCompiler/DShader/DShaderParser.cpp",
         "Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.hpp",
         "Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderPreprocess/ShaderPreprocessor.cpp",
         "Desert/Desert/Source/Engine/Core/ShaderCompiler/ShaderMapCache.cpp",
         "Desert/Desert/Source/Engine/Core/Formats/ShaderProgramMeta.hpp",
         "Desert/Desert/Source/Engine/Core/Formats/Shader.hpp",
         "Desert/Desert/Source/Engine/Core/Formats/DefaultTexture.hpp" };

    std::string                  SerializeShaderMap( const ShaderMap& map );
    Common::ResultStr<ShaderMap> DeserializeShaderMap( std::string_view bytes );

    struct ShaderMapLookup
    {
        std::optional<ShaderMap> Map;
        std::string              Rejected; // non-empty: an entry existed and could not be read (why)
    };
    ShaderMapLookup       TryLoadShaderMap( uint64_t key );
    Common::BoolResultStr StoreShaderMap( uint64_t key, const ShaderMap& map );
} // namespace Desert::Core
