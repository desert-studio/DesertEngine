#pragma once

#include <Common/Content/TextAssetHeader.hpp>

#include <istream>
#include <string>
#include <string_view>

// THE SHADER'S HEADER (T7j, lead decision 2026-09-25): the fourth way of stating an AssetHeader. A .shader is
// DSL source, not JSON, so its header cannot be a document's first member; it is the file's FIRST LINE, a
// comment the DSL parser already skips:
//
//     // DesertAsset {"Kind":"Shader","Guid":"<32 hex>","Versions":{"SHDR":1},"Dependencies":[]}
//
// The object behind the prefix IS the text header object (TextAssetHeader.hpp), spelled, parsed and checked
// by the same functions; only where it sits differs. The GUID lives in the asset's own file, as UE keeps a
// package's GUID in the package: a sidecar .meta would be a second source for one identity. A .glslh is an
// include, not an asset, and states no header.
namespace Common::Content
{
    inline constexpr std::string_view kShaderHeaderPrefix = "// DesertAsset ";

    // The header line, newline included, a writer puts before the shader's source.
    std::string WriteShaderHeaderLine( const TextAssetHeaderSerialized& header );

    // The `{...}` text on the first line of `in`. Refuses a first line that does not open with the prefix,
    // naming the prefix; reads nothing past that line.
    ResultStr<std::string> ReadShaderHeaderObject( std::istream& in );

    // The parsed header of a shader's whole source text.
    ResultStr<TextAssetHeaderSerialized> ReadShaderHeader( std::string_view source );

    // The name the DSL declares - `X` of the first `Shader "X"` line, the first line past the header comment
    // and blank or `//` lines. The runtime names a shader by its file STEM (VulkanShader, ShaderService), and a
    // material resolves its shader GUID to that stem; ShaderAsset refuses a file whose declared name differs,
    // so the two can never name different shaders. Refuses a source with no such line, naming what it found.
    ResultStr<std::string> ReadShaderDeclaredName( std::string_view source );

    // The template manifest a shader declares in its body: `Role <Name>` (what engine code asks for — a role,
    // never a shader name) and `Default Surface` (the template a material is created with when the project
    // overrides nothing). The DSL parser reads the same two lines into ShaderProgramMeta. Refuses a second Role
    // line or a `Default` that is not `Surface`, naming the line.
    struct ShaderManifest
    {
        std::string Role;
        bool        DefaultSurface = false;
    };
    ResultStr<ShaderManifest> ReadShaderManifest( std::string_view source );

    // The roles engine code asks the template registry for. A role is declared by the shader file
    // (`Role <Name>`), exactly one loaded shader per role; the asset registry carries it as the Role tag so
    // the world cook knows a material's backend without loading the shader (WorldCells::CustomShaderFrom).
    inline constexpr std::string_view kPBRSurfaceRole = "PBRSurface"; // the batched PBR backend (until MAT1a)
    inline constexpr std::string_view kDebugColorRole = "DebugColor"; // the scripting flat-colour material
    inline constexpr std::string_view kTerrainRole    = "Terrain";    // a new landscape material's template
    inline constexpr std::string_view kCloudMaterialRole = "CloudMaterial"; // a new cloud material's template

    const IAssetHeaderFormat& ShaderCommentHeaderFormat();
} // namespace Common::Content
