#pragma once

#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include <rflcpp/rfl/json.hpp>

// The reader's half of the text asset header: the two refusals every text kind with a header makes, spelled
// once. The writer's half is StampTextHeader (TextAssetHeaderStamp.hpp).
namespace Desert::Assets
{
    // THE HEADER IS LOOKED FOR FIRST, ON ITS OWN, as an untyped tree, and that ordering is the whole
    // difference between a diagnosable refusal and a puzzling one: a file from an older format misses fields
    // a full parse would name one by one, when what the reader needs to be told is that the FORMAT moved. A
    // struct would impose the rest of the schema on a document whose whole problem may be that it does not
    // match it.
    //
    // A FILE WITHOUT A HEADER IS REFUSED, NOT READ ANYWAY: it states no identity, and reading it would hand
    // it a handle nobody can reference again. The migration mints its GUID once, in the file. The old
    // generation named its version in a top-level `FormatVersion`; `absentMeans` is the version a file that
    // left it out was (nullopt when leaving it out was never legal).
    [[nodiscard]] inline Common::BoolResultStr RefuseTextWithoutHeader( const std::string& text, int current,
                                                                        std::optional<int> absentMeans )
    {
        const auto tree = rfl::json::read<rfl::Generic>( text );
        if ( !tree )
            return BOOLSUCCESS; // Not JSON: the typed parse names what is wrong with it.
        const auto fields = tree.value().to_object();
        if ( !fields || fields.value().get( std::string( Common::Content::kTextHeaderMember ) ).has_value() )
            return BOOLSUCCESS;
        std::string version = absentMeans ? std::to_string( *absentMeans ) : "(unstated)";
        if ( const auto stated = fields.value().get( "FormatVersion" ); stated.has_value() )
            if ( const auto number = stated.value().to_int(); number.has_value() )
                version = std::to_string( number.value() );
        return Common::MakeFormattedError<bool>(
             "format version {} states no header; this build reads version {} (a "
             "Header with a GUID): run Tools/SceneMigrator over it once",
             version, current );
    }

    // After the typed parse: the header states THIS build's version under `tag`, is well formed for the
    // subsystems this kind states, and names `kind`.
    [[nodiscard]] inline Common::BoolResultStr
    CheckStatedHeader( const std::optional<Common::Content::TextAssetHeaderSerialized>& header,
                       Common::Content::ContentKind kind, uint32_t tag, int current,
                       std::span<const Common::Content::SubsystemVersion> subsystems )
    {
        const int stated = StatedVersion( header, tag );
        if ( stated != current )
            return Common::MakeFormattedError<bool>(
                 "format version {} was written by a different build; this one reads version {}", stated,
                 current );
        if ( !header.has_value() )
            return Common::MakeFormattedError<bool>( "the file states no header; this build reads version {}",
                                                     current );
        const Common::Content::AssetHeaderReadContext context{ subsystems };
        const auto read = Common::Content::TextHeaderToAssetHeader( header.value(), context );
        if ( !read )
            return Common::MakeFormattedError<bool>( "{}", read.GetError() );
        if ( read.GetValue().Kind != kind )
            return Common::MakeFormattedError<bool>( "the header says kind '{}', not '{}'", header.value().Kind,
                                                     Common::Content::KindName( kind ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
