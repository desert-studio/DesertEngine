#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// THE TEXT ASSET HEADER (AF6f, decision D3): the second way of stating an AssetHeader, beside the binary
// envelope. A scene, a material and a prefab stay JSON, and their document's FIRST member is
//
//     "Header": { "Kind": "Scene", "Guid": "<32 hex>", "Versions": { "SCNE": 26, "UNIT": 1 }, "Dependencies": [] }
//
// so what the file is, who it is and which schema generations wrote it can be answered from a prefix of the
// file: the reader stops at the brace that closes this object and never sees the body. That is what lets a
// registry scan, a version gate or a dependency walk look at thousands of files without parsing an entity.
//
// The header is the ONE place a text asset states its versions: the loader's gates read them from here and
// nowhere else. The members are declared in the order the canonical writer keeps (CanonicalText.hpp).
namespace Common::Content
{
    // The member name the header sits under, first in the document.
    inline constexpr std::string_view kTextHeaderMember = "Header";

    // The header as it is spelled in JSON. Versions are keyed by the subsystem's FourCC as text; std::map
    // so the same header always writes the same bytes.
    struct TextAssetHeaderSerialized
    {
        std::string                     Kind;
        std::string                     Guid;
        std::map<std::string, uint32_t> Versions;
        std::vector<std::string>        Dependencies;

        // A document holding a header (CloudTypeData) compares its edit against its on-disk copy member-wise.
        [[nodiscard]] bool operator==( const TextAssetHeaderSerialized& ) const = default;
    };

    // 32 lower-case hex digits, Hi then Lo. The inverse refuses anything else, naming what it got.
    std::string          AssetGuidToText( const AssetGuid& guid );
    ResultStr<AssetGuid> AssetGuidFromText( std::string_view text );

    // The kind whose ContentKindSpec::Name is `name`.
    std::optional<ContentKind> ContentKindNamed( std::string_view name );

    // The header a writer states: its kind's registry name, its GUID and its subsystem versions.
    TextAssetHeaderSerialized MakeTextHeader( ContentKind kind, const AssetGuid& guid,
                                              std::span<const SubsystemVersion> versions );

    // The version the header states for `tag`, if it states one.
    std::optional<uint32_t> TextHeaderVersion( const TextAssetHeaderSerialized& header, uint32_t tag );

    // Converts and checks: kind known, GUID well-formed and not null, every tag four characters and known to
    // `context` at a version no newer than this build's, every dependency a well-formed GUID.
    ResultStr<AssetHeader> TextHeaderToAssetHeader( const TextAssetHeaderSerialized& header,
                                                    const AssetHeaderReadContext&    context );

    // The `{...}` text of the header object: the document must open with `{`, and its first member must be
    // "Header" with an object value. Reads `in` only up to the closing brace of that object.
    ResultStr<std::string> ReadTextHeaderObject( std::istream& in );

    // The same over a JSON header object's text.
    ResultStr<TextAssetHeaderSerialized> ParseTextHeaderObject( std::string_view object );

    const IAssetHeaderFormat& TextHeaderFormat();
} // namespace Common::Content
