#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace Desert::Core
{
    struct GeneratedShaderInclude
    {
        std::string_view Path;     // as written in #include, e.g. "Common/CameraUB.glslh"
        std::string_view Text;     // GLSL with DSL sugar; the includer runs TranslateSugar on it like on a file
        uint64_t         Hash = 0; // FNV-1a 64 of Text, computed by the tool; the cache key hashes this
    };

    // Shader includes generated at build time by Tools/ShaderProtocolGen from the C++ layout declarations.
    // nullptr = not a generated path (the includer then reads the file tree).
    [[nodiscard]] const GeneratedShaderInclude* FindGeneratedShaderInclude( std::string_view path ) noexcept;
    [[nodiscard]] std::span<const GeneratedShaderInclude> GeneratedShaderIncludes() noexcept;

    // True when @p name is a vertex attribute a generated vertex-input include declares
    // (Mesh/MeshVertexInputs.glslh and its siblings): the includer refuses a hand-written `in` of that name.
    // Generated with the table above from the same attribute lists (Tools/ShaderProtocolGen
    // GeneratedVertexInputNames).
    [[nodiscard]] bool IsGeneratedVertexInputName( std::string_view name ) noexcept;
} // namespace Desert::Core
