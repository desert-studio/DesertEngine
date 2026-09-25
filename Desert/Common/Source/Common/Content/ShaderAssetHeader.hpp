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

    const IAssetHeaderFormat& ShaderCommentHeaderFormat();
} // namespace Common::Content
