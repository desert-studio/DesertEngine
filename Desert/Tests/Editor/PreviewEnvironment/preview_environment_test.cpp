// Preview Scene Settings (Editor/Widgets/PreviewEnvironment.hpp): defaults, the editor.json round trip,
// the look the renderer samples with, and the refusal of a skybox path the registry does not know.

#include <Editor/Widgets/PreviewEnvironment.hpp>

#include <gtest/gtest.h>

#include <rflcpp/rfl/DefaultIfMissing.hpp>
#include <rflcpp/rfl/json.hpp>

#include <cmath>
#include <optional>
#include <string>

using namespace Desert::Editor::PreviewEnvironment;

namespace
{
    // A registry holding exactly one HDR, the way AssetManager::FindByPath<SkyboxAsset> answers.
    std::optional<uint64_t> OneSkybox( const std::string& path )
    {
        if ( path == "Resources/Assets/HDR/rural_asphalt_road_2k.hdr" )
            return 42u;
        return std::nullopt;
    }
} // namespace

TEST( PreviewEnvironment, DefaultsAreThePresetSkyAtUnitGainWithEverythingShown )
{
    const Settings settings;
    EXPECT_TRUE( settings.Skybox.empty() ) << "a fresh editor.json must open previews on the preset sky";
    EXPECT_EQ( settings.RotationDegrees, 0.0f );
    EXPECT_EQ( settings.ExposureEV, 0.0f );
    EXPECT_TRUE( settings.ShowEnvironment );
    EXPECT_TRUE( settings.ShowFloor );

    const auto look = LookOf( settings );
    EXPECT_EQ( look, Desert::Graphic::SkyLook{} ) << "the defaults must sample the HDR exactly as baked";

    const auto resolved = Resolve( settings, OneSkybox );
    EXPECT_FALSE( resolved.Skybox.has_value() );
    EXPECT_TRUE( resolved.Error.empty() ) << "no skybox chosen is the preset sky, not an error";
}

TEST( PreviewEnvironment, RoundTripsThroughTheSameJsonWriterAsEditorJson )
{
    Settings authored;
    authored.Skybox          = "Resources/Assets/HDR/rural_asphalt_road_2k.hdr";
    authored.RotationDegrees = -135.0f;
    authored.ExposureEV      = 2.0f;
    authored.ShowEnvironment = false;
    authored.ShowFloor       = false;

    const std::string text = rfl::json::write( authored );
    const auto        back = rfl::json::read<Settings, rfl::DefaultIfMissing>( text );
    ASSERT_TRUE( back ) << text;
    EXPECT_EQ( back.value(), authored ) << text;

    // An editor.json written before the record existed, or holding only part of it, reads as defaults
    // for what it does not state — the same rfl::DefaultIfMissing EditorPreferences::Load reads with.
    const auto partial = rfl::json::read<Settings, rfl::DefaultIfMissing>( R"({"ExposureEV":1.5})" );
    ASSERT_TRUE( partial );
    Settings expected;
    expected.ExposureEV = 1.5f;
    EXPECT_EQ( partial.value(), expected );
}

TEST( PreviewEnvironment, RotationAndEvReachTheLookThatIsSampled )
{
    Settings settings;
    settings.ExposureEV      = 2.0f;
    settings.RotationDegrees = 270.0f;
    const auto look          = LookOf( settings );
    EXPECT_FLOAT_EQ( look.Intensity, 4.0f ) << "EV +2 is four times the radiance";
    EXPECT_FLOAT_EQ( look.RotationDegrees, -90.0f ) << "rotation is kept in [-180, 180]";
    EXPECT_EQ( look.Tint, glm::vec3( 1.0f ) );

    settings.ExposureEV = 40.0f;
    EXPECT_FLOAT_EQ( LookOf( settings ).Intensity, std::exp2( kMaxEV ) ) << "EV is clamped to the slider range";
}

TEST( PreviewEnvironment, AKnownPathResolvesAndAnUnknownOneIsRefusedByName )
{
    Settings settings;
    settings.Skybox  = "Resources/Assets/HDR/rural_asphalt_road_2k.hdr";
    const auto known = Resolve( settings, OneSkybox );
    ASSERT_TRUE( known.Skybox.has_value() );
    EXPECT_EQ( *known.Skybox, 42u );
    EXPECT_TRUE( known.Error.empty() );

    settings.Skybox    = "Resources/Assets/HDR/renamed_away.hdr";
    const auto unknown = Resolve( settings, OneSkybox );
    EXPECT_FALSE( unknown.Skybox.has_value() ) << "an unknown path must not fall back to some other sky";
    EXPECT_NE( unknown.Error.find( "Resources/Assets/HDR/renamed_away.hdr" ), std::string::npos )
         << "the refusal must name the path: " << unknown.Error;
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
