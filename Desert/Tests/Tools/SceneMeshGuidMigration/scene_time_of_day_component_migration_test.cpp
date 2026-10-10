// MigrateTimeOfDayComponentV43ToV44 (TOD-SPLIT): the five clock keys a SkyAtmosphere block stated leave the
// sky and become a TimeOfDay block on the same record; a sky that stated none gets no clock; a prefab
// override that restates a clock key on the sky is refused by name.

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace Migration = Desert::Migration;
using Desert::Assets::EntityData;
using Desert::Assets::PrefabOverrideData;

// NOLINTBEGIN(bugprone-unchecked-optional-access)
namespace
{
    rfl::Generic::Object BlockOf( const EntityData& entity, const char* component )
    {
        return entity.Components.get( component ).value().to_object().value();
    }

    EntityData SkyWithClock()
    {
        rfl::Generic::Object sky;
        sky["SkyBrightness"]         = rfl::Generic( 1.5 );
        sky["DriveSunFromTimeOfDay"] = rfl::Generic( true );
        sky["TimeOfDay"]             = rfl::Generic( 7.5 );
        sky["DayLengthSeconds"]      = rfl::Generic( 1200.0 );
        sky["Latitude"]              = rfl::Generic( -33.0 );
        sky["NorthOffset"]           = rfl::Generic( 90.0 );
        EntityData entity;
        entity.Tag                         = "Sky";
        entity.Components["SkyAtmosphere"] = rfl::Generic( std::move( sky ) );
        return entity;
    }
} // namespace

TEST( SceneTimeOfDayComponentMigration, VersionIsTheGenerationAfterUIAnimationSequences )
{
    EXPECT_EQ( Migration::kSceneVersionTimeOfDayComponent, Migration::kSceneVersionUIAnimationSequences + 1 );
    EXPECT_EQ( Migration::kSceneVersionTimeOfDayComponent, Desert::Core::kSceneVersion );
}

TEST( SceneTimeOfDayComponentMigration, TheSkysClockBecomesATimeOfDayBlockOnTheSameRecord )
{
    std::vector<EntityData> entities{ SkyWithClock() };
    const auto              report = Migration::MigrateTimeOfDayComponentV43ToV44( entities );
    ASSERT_TRUE( report.Refused.empty() );
    EXPECT_EQ( report.Clocks, 1u );
    EXPECT_EQ( report.KeysMoved, 5u );

    const auto sky = BlockOf( entities[0], "SkyAtmosphere" );
    for ( const char* key :
          { "DriveSunFromTimeOfDay", "TimeOfDay", "DayLengthSeconds", "Latitude", "NorthOffset" } )
        EXPECT_FALSE( sky.get( key ).has_value() ) << "the sky still states " << key;
    EXPECT_DOUBLE_EQ( sky.get( "SkyBrightness" ).value().to_double().value(), 1.5 );

    const auto clock = BlockOf( entities[0], "TimeOfDay" );
    EXPECT_EQ( clock.get( "DriveSunFromTimeOfDay" ).value().to_bool().value(), true );
    EXPECT_DOUBLE_EQ( clock.get( "TimeOfDay" ).value().to_double().value(), 7.5 );
    EXPECT_DOUBLE_EQ( clock.get( "DayLengthSeconds" ).value().to_double().value(), 1200.0 );
    EXPECT_DOUBLE_EQ( clock.get( "Latitude" ).value().to_double().value(), -33.0 );
    EXPECT_DOUBLE_EQ( clock.get( "NorthOffset" ).value().to_double().value(), 90.0 );
}

TEST( SceneTimeOfDayComponentMigration, ASkyWithNoClockKeysGetsNoClock )
{
    rfl::Generic::Object sky;
    sky["SkyBrightness"] = rfl::Generic( 1.0 );
    EntityData entity;
    entity.Components["SkyAtmosphere"] = rfl::Generic( std::move( sky ) );
    std::vector<EntityData> entities{ entity };

    const auto report = Migration::MigrateTimeOfDayComponentV43ToV44( entities );
    EXPECT_TRUE( report.Refused.empty() );
    EXPECT_EQ( report.Clocks, 0u );
    EXPECT_FALSE( entities[0].Components.get( "TimeOfDay" ).has_value() );
}

TEST( SceneTimeOfDayComponentMigration, AClockKeyInAPrefabOverrideIsRefused )
{
    rfl::Generic::Object sky;
    sky["TimeOfDay"] = rfl::Generic( 18.0 );
    PrefabOverrideData override_;
    override_.Components["SkyAtmosphere"] = rfl::Generic( std::move( sky ) );
    EntityData entity;
    entity.PrefabOverrides = std::vector<PrefabOverrideData>{ override_ };
    std::vector<EntityData> entities{ entity };

    const auto report = Migration::MigrateTimeOfDayComponentV43ToV44( entities );
    ASSERT_EQ( report.Refused.size(), 1u );
    EXPECT_NE( report.Refused.front().find( "TimeOfDay" ), std::string::npos );
}
// NOLINTEND(bugprone-unchecked-optional-access)
