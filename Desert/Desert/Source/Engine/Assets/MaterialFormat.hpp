#pragma once

// THE ONE READER AND THE ONE WRITER OF .demat TEXT. Every place that turns a MaterialData into a file or a
// file into a MaterialData goes through these two, so the header (Common/Content/TextAssetHeader.hpp) is
// stamped by one rule and checked by one gate - the same split PrefabFormat.hpp makes for .deprefab.

#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <rflcpp/rfl/json.hpp>

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace Desert::Assets
{
    // The subsystem versions a .demat of this build states: the material schema, and nothing else.
    [[nodiscard]] inline std::span<const Common::Content::SubsystemVersion> MaterialTextSubsystems()
    {
        static const std::array<Common::Content::SubsystemVersion, 1> versions = {
             Common::Content::SubsystemVersion{ kMaterialSchemaTag, kMaterialSchemaVersion } };
        return versions;
    }

    // `material` with its header stamped: the GUID it was loaded with is kept, a material that never had one
    // is minted one (StampTextHeader). A caller that keeps the material after writing it (an asset that will
    // be saved again) assigns this back, so the minted GUID is the one every later save states.
    [[nodiscard]] inline MaterialData StampMaterialHeader( MaterialData material )
    {
        material.Header = StampTextHeader( material.Header, Common::Content::ContentKind::Material,
                                           MaterialTextSubsystems() );
        return material;
    }

    // The canonical text of `material`, header stamped (StampMaterialHeader).
    [[nodiscard]] inline Common::ResultStr<std::string> WriteMaterialJson( const MaterialData& material )
    {
        return Common::Content::CanonicalJsonTextOfWriterOutput( rfl::json::write( StampMaterialHeader( material ) ) );
    }

    // WriteMaterialJson, written to `file` atomically (WriteCanonicalJsonFileAtomic).
    [[nodiscard]] inline Common::ResultStr<bool> WriteMaterialFile( const std::filesystem::path& file,
                                                                    const MaterialData&          material )
    {
        const auto text = WriteMaterialJson( material );
        if ( !text )
            return Common::MakeError<bool>( "[Material] '" + file.generic_string() + "': " + text.GetError() );
        return Common::Content::WriteCanonicalJsonFileAtomic( file, text.GetValue() );
    }

    // `json` as a material, or a refusal naming `source`: unreadable, no header (a file from before the
    // header - Tools/SceneMigrator stamps it), a schema generation other than this build's, a malformed
    // header, or one naming another kind.
    [[nodiscard]] inline Common::ResultStr<MaterialData> ParseMaterialJson( std::string_view   source,
                                                                           const std::string& json )
    {
        auto parsed = rfl::json::read<MaterialData>( json );
        if ( !parsed )
            return Common::MakeError<MaterialData>( "[Material] '" + std::string( source ) +
                                                    "' is not a readable material file: " + parsed.error().what() );
        const int stated = StatedVersion( parsed.value().Header, kMaterialSchemaTag );
        if ( stated != static_cast<int>( kMaterialSchemaVersion ) )
            return Common::MakeError<MaterialData>(
                 "[Material] '" + std::string( source ) + "' states material schema v" + std::to_string( stated ) +
                 " and this engine reads v" + std::to_string( kMaterialSchemaVersion ) +
                 " only (v0 = no header: run Tools/SceneMigrator over it once)" );
        const Common::Content::AssetHeaderReadContext context{ MaterialTextSubsystems() };
        const auto header = Common::Content::TextHeaderToAssetHeader( *parsed.value().Header, context );
        if ( !header )
            return Common::MakeError<MaterialData>( "[Material] '" + std::string( source ) + "': " +
                                                    header.GetError() );
        if ( header.GetValue().Kind != Common::Content::ContentKind::Material )
            return Common::MakeError<MaterialData>( "[Material] '" + std::string( source ) +
                                                    "': the header says kind '" + parsed.value().Header->Kind +
                                                    "', not 'Material'" );
        return Common::MakeSuccess( std::move( parsed.value() ) );
    }
} // namespace Desert::Assets
