// GP3, scene schema v44: SceneSettings gains PlayerController and RespawnDelay after DefaultPawn, where the saver
// writes them; a stated key is kept.
#include <SceneMigration.hpp>
#include <Engine/Core/SceneSettings.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace Migration = Desert::Migration;

namespace
{
    std::vector<std::string> Keys( const Migration::SceneSerialized& scene )
    {
        std::vector<std::string> out;
        for ( const auto& [key, field] : scene.Settings.value().to_object().value() )
            out.push_back( key );
        return out;
    }
} // namespace

TEST( GameModeSettingsMigration, AddsControllerAndDelayRightAfterTheDefaultPawn )
{
    static_assert( Migration::kSceneVersionGameModeSettings == Desert::Core::kSceneVersion );
    rfl::Generic::Object pawn;
    pawn["Guid"] = rfl::Generic( std::string() );
    pawn["Path"] = rfl::Generic( std::string() );
    rfl::Generic::Object settings;
    settings["RenderingPath"] = rfl::Generic( 1 );
    settings["DefaultPawn"]   = rfl::Generic( pawn );
    settings["SplashSprite"]  = rfl::Generic( pawn );
    Migration::SceneSerialized scene;
    scene.Settings = rfl::Generic( settings );

    EXPECT_EQ( Migration::MigrateGameModeSettingsV43ToV44( scene ), 2 );
    EXPECT_EQ( Keys( scene ), ( std::vector<std::string>{ "RenderingPath", "DefaultPawn", "PlayerController",
                                                          "RespawnDelay", "SplashSprite" } ) );
    const auto delay =
         scene.Settings.value().to_object().value().get( "RespawnDelay" ).value().to_double().value();
    EXPECT_EQ( delay, static_cast<double>( Desert::Core::SceneSettings{}.RespawnDelay ) );
}

TEST( GameModeSettingsMigration, AStatedDelayIsKeptAndNothingIsAddedTwice )
{
    rfl::Generic::Object settings;
    settings["RespawnDelay"] = rfl::Generic( 7.5 );
    Migration::SceneSerialized scene;
    scene.Settings = rfl::Generic( settings );

    EXPECT_EQ( Migration::MigrateGameModeSettingsV43ToV44( scene ), 1 );
    EXPECT_EQ( Migration::MigrateGameModeSettingsV43ToV44( scene ), 0 );
    EXPECT_EQ( scene.Settings.value().to_object().value().get( "RespawnDelay" ).value().to_double().value(), 7.5 );
}
