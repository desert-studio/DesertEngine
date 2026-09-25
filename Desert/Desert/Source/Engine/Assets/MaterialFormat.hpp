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

#include <algorithm>
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
        auto header =
             StampTextHeader( material.Header, Common::Content::ContentKind::Material, MaterialTextSubsystems() );
        // Every outgoing reference (an instance's parent, each texture and cloud asset), stated where a reader
        // of the header alone finds it.
        header.Dependencies = material.ReferencedGuidTexts();
        material.Header = std::move( header );
        return material;
    }

    // The canonical text of `material`, header stamped (StampMaterialHeader).
    [[nodiscard]] inline Common::ResultStr<std::string> WriteMaterialJson( const MaterialData& material )
    {
        return Common::Content::CanonicalJsonTextOfWriterOutput(
             rfl::json::write( StampMaterialHeader( material ) ) );
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

    namespace Detail
    {
        // The header alone: read FIRST, so a file of another schema generation is refused by its stated
        // version and not by whichever payload member its older shape lacks (rfl ignores the rest).
        struct MaterialHeaderProbe
        {
            std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        };

        // A slot's GUID: empty (an authored empty slot), or a non-null GUID the header states as a Dependency.
        [[nodiscard]] inline Common::ResultStr<bool>
        CheckStatedRef( std::string_view source, std::string_view list, const MaterialAssetRef& ref,
                        const std::vector<Common::Content::AssetGuid>& deps )
        {
            const auto refuse = [&]( const std::string& why )
            {
                return Common::MakeError<bool>( "[Material] '" + std::string( source ) +
                                                "': " + std::string( list ) + " slot '" + ref.Name + "' " + why );
            };
            if ( ref.Guid.empty() )
            {
                if ( !ref.Path.empty() )
                    return refuse( "states no GUID but a path '" + ref.Path +
                                   "' - a path is a locator, not an identity" );
                return Common::MakeSuccess( true );
            }
            const auto guid = Common::Content::AssetGuidFromText( ref.Guid );
            if ( !guid || guid.GetValue().IsNull() )
                return refuse( "states '" + ref.Guid + "', which is not an asset GUID" );
            if ( std::find( deps.begin(), deps.end(), guid.GetValue() ) == deps.end() )
                return refuse( "names GUID " + ref.Guid + ", which is not among the header's Dependencies" );
            return Common::MakeSuccess( true );
        }
    } // namespace Detail

    // `json` as a material, or a refusal naming `source`: no header (a file from before the header), a schema
    // generation other than this build's (v1: a MaterialId beside the GUID; v2: slots by path-derived number;
    // v3: the shader by name - Tools/SceneMigrator raises all three), unreadable, a malformed header or one
    // naming another kind, a Parent, Shader or slot GUID that is malformed or not among the header's
    // Dependencies, a Shader stated with no GUID, or a slot with a path and no GUID.
    [[nodiscard]] inline Common::ResultStr<MaterialData> ParseMaterialJson( std::string_view   source,
                                                                            const std::string& json )
    {
        const auto probe = rfl::json::read<Detail::MaterialHeaderProbe>( json );
        if ( !probe )
            return Common::MakeError<MaterialData>( "[Material] '" + std::string( source ) +
                                                    "' is not a readable material file: " + probe.error().what() );
        const auto wrongSchema = [&source]( int stated )
        {
            return Common::MakeError<MaterialData>(
                 "[Material] '" + std::string( source ) + "' states material schema v" + std::to_string( stated ) +
                 " and this engine reads v" + std::to_string( kMaterialSchemaVersion ) +
                 " only (v0 = no header, v1 = a MaterialId beside the GUID, v2 = texture and cloud slots by "
                 "path-derived number, v3 = the shader by name: run Tools/SceneMigrator over it once)" );
        };
        // No header at all is schema v0: refused here, by name, before anything reads the header.
        if ( !probe.value().Header.has_value() )
            return wrongSchema( 0 );
        const int stated = StatedVersion( probe.value().Header, kMaterialSchemaTag );
        if ( stated != static_cast<int>( kMaterialSchemaVersion ) )
            return wrongSchema( stated );

        auto parsed = rfl::json::read<MaterialData>( json );
        if ( !parsed )
            return Common::MakeError<MaterialData>(
                 "[Material] '" + std::string( source ) +
                 "' is not a readable material file: " + parsed.error().what() );
        const MaterialData& material = parsed.value();
        // The probe above read the same text, but this is a second parse: its header is asked again
        // rather than assumed, so a disagreement between the two reads is a refusal and not a crash.
        if ( !material.Header.has_value() )
            return wrongSchema( 0 );
        const Common::Content::TextAssetHeaderSerialized& textHeader = *material.Header;
        const Common::Content::AssetHeaderReadContext     context{ MaterialTextSubsystems() };
        const auto header = Common::Content::TextHeaderToAssetHeader( textHeader, context );
        if ( !header )
            return Common::MakeError<MaterialData>( "[Material] '" + std::string( source ) +
                                                    "': " + header.GetError() );
        if ( header.GetValue().Kind != Common::Content::ContentKind::Material )
            return Common::MakeError<MaterialData>( "[Material] '" + std::string( source ) +
                                                    "': the header says kind '" + textHeader.Kind +
                                                    "', not 'Material'" );
        const auto& deps = header.GetValue().Dependencies;
        if ( const std::string* parentTextOrNull = material.ParentText() )
        {
            const std::string& parentText = *parentTextOrNull;
            const auto         parent     = Common::Content::AssetGuidFromText( parentText );
            if ( !parent || parent.GetValue().IsNull() )
                return Common::MakeError<MaterialData>( "[Material] '" + std::string( source ) + "': Parent '" +
                                                        parentText + "' is not a material GUID" );
            if ( std::find( deps.begin(), deps.end(), parent.GetValue() ) == deps.end() )
                return Common::MakeError<MaterialData>( "[Material] '" + std::string( source ) + "': Parent '" +
                                                        parentText + "' is not among the header's Dependencies" );
        }
        // A stated Shader is never an empty slot: the default surface is said by stating none.
        if ( material.Shader.has_value() )
        {
            if ( material.Shader->Guid.empty() )
                return Common::MakeError<MaterialData>( "[Material] '" + std::string( source ) +
                                                        "': Shader states no GUID (path '" + material.Shader->Path +
                                                        "'); leave Shader out for the standard surface" );
            const MaterialAssetRef shaderRef{ "Shader", material.Shader->Guid, material.Shader->Path };
            if ( const auto ok = Detail::CheckStatedRef( source, "shader", shaderRef, deps ); !ok )
                return Common::MakeError<MaterialData>( ok.GetError() );
        }
        for ( const auto& ref : material.Textures )
            if ( const auto ok = Detail::CheckStatedRef( source, "texture", ref, deps ); !ok )
                return Common::MakeError<MaterialData>( ok.GetError() );
        for ( const auto& ref : material.CloudAssets )
            if ( const auto ok = Detail::CheckStatedRef( source, "cloud asset", ref, deps ); !ok )
                return Common::MakeError<MaterialData>( ok.GetError() );
        return Common::MakeSuccess( std::move( parsed.value() ) );
    }
} // namespace Desert::Assets
