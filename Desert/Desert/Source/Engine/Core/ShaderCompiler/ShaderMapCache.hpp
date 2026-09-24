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

    // The blob's own version. Bump it when the byte layout changes OR when DShaderParser starts producing
    // different metadata from the same text — the key hashes the text, not the parser, so a parser change
    // without a bump serves the OLD metadata.
    inline constexpr uint32_t kShaderMapFormatVersion = 1;

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
