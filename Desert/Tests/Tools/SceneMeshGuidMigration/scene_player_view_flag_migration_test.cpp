// MigratePlayerViewFlagV39ToV40 (SPAWN1): Camera.IsMainCamera (default true on every camera) becomes
// Camera.AutoActivateForPlayer (default false). Only a scene's SOLE camera keeps its value - a missing key
// being the old default, true - and a prefab override's IsMainCamera goes.

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace Migration = Desert::Migration;
using Desert::Assets::EntityData;
using Desert::Assets::PrefabOverrideData;

// NOLINTBEGIN(bugprone-unchecked-optional-access)
namespace
{
    rfl::Generic::Object CameraOf( const rfl::ExtraFields<rfl::Generic>& components )
    {
        return components.get( "Camera" ).value().to_object().value();
    }

    // A camera record; `isMain` unset = the key is not stated (the old default).
    EntityData Camera( const char* tag, std::optional<bool> isMain )
    {
        rfl::Generic::Object block;
        block["FOV"] = rfl::Generic( 60.0 );
        if ( isMain )
            block["IsMainCamera"] = rfl::Generic( *isMain );
        EntityData entity;
        entity.Tag                  = tag;
        entity.Components["Camera"] = rfl::Generic( std::move( block ) );
        return entity;
    }

    std::optional<bool> Flag( const EntityData& entity )
    {
        const auto stated = CameraOf( entity.Components ).get( "AutoActivateForPlayer" );
        if ( !stated.has_value() )
            return std::nullopt;
        return stated.value().to_bool().value();
    }
} // namespace

TEST( ScenePlayerViewFlagMigration, VersionIsGeneration40 )
{
    EXPECT_EQ( Migration::kSceneVersionPlayerViewFlag, 40 );
    EXPECT_LT( Migration::kSceneVersionPlayerViewFlag, Desert::Core::kSceneVersion );
}

TEST( ScenePlayerViewFlagMigration, TheSoleCameraKeepsTheOldDefault )
{
    std::vector<EntityData> entities{ Camera( "Main Camera", std::nullopt ) };
    const auto              report = Migration::MigratePlayerViewFlagV39ToV40( entities );
    EXPECT_EQ( report.Cameras, 1u );
    EXPECT_TRUE( report.KeptOne );
    EXPECT_EQ( Flag( entities[0] ), true );
    EXPECT_FALSE( CameraOf( entities[0].Components ).get( "IsMainCamera" ).has_value() );
    EXPECT_TRUE( CameraOf( entities[0].Components ).get( "FOV" ).has_value() ) << "other keys stay";
}

TEST( ScenePlayerViewFlagMigration, TheSoleCameraKeepsAStatedFalse )
{
    std::vector<EntityData> entities{ Camera( "Preview", false ) };
    Migration::MigratePlayerViewFlagV39ToV40( entities );
    EXPECT_EQ( Flag( entities[0] ), false );
}

TEST( ScenePlayerViewFlagMigration, TwoCamerasBothStateFalse )
{
    // Both were "main" under the old default; which one the player got was registry order, not the file.
    std::vector<EntityData> entities{ Camera( "A", std::nullopt ), Camera( "B", true ) };
    const auto              report = Migration::MigratePlayerViewFlagV39ToV40( entities );
    EXPECT_EQ( report.Cameras, 2u );
    EXPECT_FALSE( report.KeptOne );
    EXPECT_EQ( Flag( entities[0] ), false );
    EXPECT_EQ( Flag( entities[1] ), false );
    EXPECT_FALSE( CameraOf( entities[1].Components ).get( "IsMainCamera" ).has_value() );
}

TEST( ScenePlayerViewFlagMigration, NoCameraTouchesNothing )
{
    EntityData light;
    light.Tag                      = "Sun";
    light.Components["PointLight"] = rfl::Generic( rfl::Generic::Object{} );
    std::vector<EntityData> entities{ light };
    const auto              report = Migration::MigratePlayerViewFlagV39ToV40( entities );
    EXPECT_EQ( report.Cameras, 0u );
    EXPECT_FALSE( entities[0].Components.get( "Camera" ).has_value() );
}

TEST( ScenePlayerViewFlagMigration, APrefabOverrideLosesTheKey )
{
    EntityData instance;
    instance.Tag = "Pawn";
    rfl::Generic::Object overridden;
    overridden["IsMainCamera"] = rfl::Generic( true );
    PrefabOverrideData override_;
    override_.Components["Camera"] = rfl::Generic( std::move( overridden ) );
    instance.PrefabOverrides       = std::vector<PrefabOverrideData>{ std::move( override_ ) };
    std::vector<EntityData> entities{ std::move( instance ), Camera( "Main Camera", std::nullopt ) };

    const auto report = Migration::MigratePlayerViewFlagV39ToV40( entities );
    EXPECT_EQ( report.OverridesDropped, 1u );
    EXPECT_EQ( report.Cameras, 1u ) << "an override is not one of the level's own cameras";
    EXPECT_FALSE( CameraOf( ( *entities[0].PrefabOverrides )[0].Components ).get( "IsMainCamera" ).has_value() );
    EXPECT_EQ( Flag( entities[1] ), true );
}
// NOLINTEND(bugprone-unchecked-optional-access)
