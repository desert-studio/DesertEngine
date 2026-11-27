#pragma once
// The generator's stages: check the joined include list, emit each include's GLSL, the engine's lookup table and
// the census manifest. Every stage is a pure function of its inputs; Main does the file I/O.
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::ShaderProtocolGen
{
    using Graphic::ShaderProtocols::ShaderIncludeDecl;
    using Graphic::ShaderProtocols::ShaderStructDecl;

    // One generated include, emitted.
    struct GeneratedInclude
    {
        const ShaderIncludeDecl* Decl = nullptr;
        std::string              Text;
        uint64_t                 Hash = 0;
    };

    // A struct or block the generator emits, by GLSL name with its ordered member names: what TwinCheck compares
    // hand-written shader text against and what the manifest lists.
    struct GeneratedShape
    {
        std::string              Name;
        std::vector<std::string> Members;
        std::string_view         Path;         // the generated include that emits it
        std::string_view         SourceHeader; // its C++ declaration
    };

    [[nodiscard]] uint64_t Fnv1a64( std::string_view text ) noexcept;

    // The refusals a descriptor list can earn, as `SourceHeader: path: reason` lines (a descriptor has no line):
    // a path colliding with a file under @p shaderRoot, two descriptors on one path, two emitted structs or blocks
    // of one GLSL name, an unknown Requires, a struct used before its declaration, two Fixed bindings of one
    // number in one include closure, and two Fixed scene blocks on one number. Empty = the list is sound.
    // @p sceneIncludes are the scene part (every lit program includes them side by side).
    [[nodiscard]] std::vector<std::string> CheckIncludes( std::span<const ShaderIncludeDecl* const> includes,
                                                          std::span<const ShaderIncludeDecl* const> sceneIncludes,
                                                          const std::filesystem::path&              shaderRoot );

    // The GLSL of one include: guard, banner, Requires, structs (+ DESERT_<Glsl>_MEMBERS), constants, blocks,
    // Text.
    [[nodiscard]] std::string EmitGlsl( const ShaderIncludeDecl& include );

    // Every emitted struct and block of @p includes.
    [[nodiscard]] std::vector<GeneratedShape> CollectShapes( std::span<const ShaderIncludeDecl* const> includes );

    // CppEmit.cpp: GeneratedShaderIncludes.gen.cpp, the table sorted by path plus IsGeneratedVertexInputName.
    [[nodiscard]] std::string EmitCppTable( std::span<const GeneratedInclude> includes,
                                            std::span<const std::string_view> vertexInputNames );

    struct TwinFinding;
    struct TwinAllow;
    // Manifest.cpp: protocol_manifest.json for the census.
    [[nodiscard]] std::string EmitManifest( std::span<const GeneratedInclude> includes,
                                            std::span<const GeneratedShape>   shapes,
                                            std::span<const TwinFinding>      twins,
                                            std::span<const TwinAllow>        allowList );
} // namespace Desert::ShaderProtocolGen
