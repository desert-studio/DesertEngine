// Scene v41 -> v42 (MigrateSoundRefsV41ToV42): an AudioSource's Clip path becomes a Sound reference
// {Guid, Path} to the `.desound` beside the audio file, and a sequence Audio section's Sound path becomes that
// GUID. A path with no `.desound` naming it is refused by name and left as it was.

#include <SceneMigration.hpp>

#include <Engine/Assets/SoundAsset.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs        = std::filesystem;
namespace Migration = Desert::Migration;

namespace
{
    struct SoundFixture
    {
        fs::path                   Root;
        Common::Content::AssetGuid Guid{ 0x5151, 0x0042 };

        SoundFixture()
        {
            Root = fs::temp_directory_path() / "desert_sound_refs_v42";
            fs::remove_all( Root );
            fs::create_directories( Root / "Audio" );
            std::ofstream( Root / "Audio" / "Hit.wav", std::ios::binary ) << "RIFF";
            const auto text = Desert::Assets::SoundAsset::Write( Guid, "Hit.wav" );
            EXPECT_TRUE( text );
            std::ofstream( Root / "Audio" / "Hit.desound", std::ios::binary ) << text.GetValue();
        }

        ~SoundFixture()
        {
            fs::remove_all( Root );
        }
    };

    Desert::Assets::EntityData SourceStating( const std::string& clip )
    {
        rfl::Generic::Object block;
        block["Clip"]   = rfl::Generic( clip );
        block["Volume"] = rfl::Generic( 0.5 );
        Desert::Assets::EntityData entity;
        entity.Tag                       = "Speaker";
        entity.Components["AudioSource"] = rfl::Generic( std::move( block ) );
        return entity;
    }
} // namespace

TEST( SoundRefsMigration, AnAudioSourceClipBecomesASoundReferenceAndASecondPassFindsNothing )
{
    const SoundFixture                      fixture;
    std::vector<Desert::Assets::EntityData> entities{ SourceStating( "Audio/Hit.wav" ) };

    const auto report = Migration::MigrateSoundRefsV41ToV42( entities, fixture.Root );
    EXPECT_TRUE( report.Refused.empty() ) << report.Refused.front();
    EXPECT_EQ( report.Sources, 1U );

    const auto block = entities[0].Components.get( "AudioSource" ).value().to_object().value();
    EXPECT_FALSE( block.get( "Clip" ).has_value() );
    EXPECT_DOUBLE_EQ( block.get( "Volume" ).value().to_double().value(), 0.5 );
    const auto sound = block.get( "Sound" ).value().to_object().value();
    EXPECT_EQ( sound.get( "Guid" ).value().to_string().value(), Common::Content::AssetGuidToText( fixture.Guid ) );
    EXPECT_EQ( sound.get( "Path" ).value().to_string().value(), "Audio/Hit.desound" );

    EXPECT_EQ( Migration::MigrateSoundRefsV41ToV42( entities, fixture.Root ).Sources, 0U );
}

TEST( SoundRefsMigration, ASequenceAudioSectionsSoundPathBecomesTheGuid )
{
    const SoundFixture fixture;
    const auto         sequence = rfl::json::read<rfl::Generic>(
         R"({"Tracks":[{"Sections":[{"Audio":{"Sound":"Audio/Hit.wav","Gain":1.0}}]}]})" );
    ASSERT_TRUE( sequence );
    rfl::Generic::Object block;
    block["Sequence"] = sequence.value();
    Desert::Assets::EntityData entity;
    entity.Tag                  = "Movie";
    entity.Components["UIAnim"] = rfl::Generic( std::move( block ) );
    std::vector<Desert::Assets::EntityData> entities{ entity };

    const auto report = Migration::MigrateSoundRefsV41ToV42( entities, fixture.Root );
    EXPECT_TRUE( report.Refused.empty() ) << report.Refused.front();
    EXPECT_EQ( report.Sections, 1U );
    const std::string after = rfl::json::write( entities[0].Components.get( "UIAnim" ).value() );
    EXPECT_NE( after.find( Common::Content::AssetGuidToText( fixture.Guid ) ), std::string::npos ) << after;
    EXPECT_EQ( after.find( "Hit.wav" ), std::string::npos ) << after;

    EXPECT_EQ( Migration::MigrateSoundRefsV41ToV42( entities, fixture.Root ).Sections, 0U );
}

TEST( SoundRefsMigration, AClipWithNoDesoundIsRefusedByPath )
{
    const SoundFixture                      fixture;
    std::vector<Desert::Assets::EntityData> entities{ SourceStating( "Audio/Missing.wav" ) };

    const auto report = Migration::MigrateSoundRefsV41ToV42( entities, fixture.Root );
    ASSERT_EQ( report.Refused.size(), 1U );
    EXPECT_NE( report.Refused.front().find( "Missing.wav" ), std::string::npos ) << report.Refused.front();
    const auto block = entities[0].Components.get( "AudioSource" ).value().to_object().value();
    EXPECT_TRUE( block.get( "Clip" ).has_value() );
}
