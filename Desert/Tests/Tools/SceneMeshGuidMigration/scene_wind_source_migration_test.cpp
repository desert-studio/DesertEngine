// MigrateWindSourceV41ToV42 (WIND-SRC): VolumetricCloud.WindDirection / WindSpeed leave the cloud block; a scene
// whose first enabled layer had wind gains ONE WindSource record with the same direction and speed, a scene
// without wind gains nothing, a prefab gains nothing, and a second layer's different wind is reported.

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace Migration = Desert::Migration;
using Desert::Assets::EntityData;

// NOLINTBEGIN(bugprone-unchecked-optional-access)
namespace
{
    // A cloud layer record; an unset optional = the key is not stated (the v41 default).
    EntityData Cloud( std::optional<std::vector<double>> direction, std::optional<double> speed )
    {
        rfl::Generic::Object block;
        block["Enabled"] = rfl::Generic( true );
        if ( direction )
        {
            rfl::Generic::Array array;
            for ( const double c : *direction )
                array.push_back( rfl::Generic( c ) );
            block["WindDirection"] = rfl::Generic( std::move( array ) );
        }
        if ( speed )
            block["WindSpeed"] = rfl::Generic( *speed );
        EntityData entity;
        entity.Tag                           = "Clouds";
        entity.Components["VolumetricCloud"] = rfl::Generic( std::move( block ) );
        return entity;
    }

    rfl::Generic::Object BlockOf( const EntityData& entity, const char* key )
    {
        return entity.Components.get( key ).value().to_object().value();
    }

    const EntityData* WindSourceOf( const std::vector<EntityData>& entities )
    {
        for ( const auto& entity : entities )
            if ( entity.Components.get( "WindSource" ).has_value() )
                return &entity;
        return nullptr;
    }
} // namespace

TEST( SceneWindSourceMigration, VersionIsTheGenerationAfterUIAnimationSequences )
{
    EXPECT_EQ( Migration::kSceneVersionWindSource, Migration::kSceneVersionUIAnimationSequences + 1 );
    EXPECT_EQ( Migration::kSceneVersionWindSource, Desert::Core::kSceneVersion );
}

TEST( SceneWindSourceMigration, CloudWindBecomesOneWindSourceWithTheSameDirectionAndSpeed )
{
    std::vector<EntityData> entities{ Cloud( std::vector<double>{ 0.0, 0.0, -1.0 }, 1250.0 ) };
    const auto              report = Migration::MigrateWindSourceV41ToV42( entities, "Fixture", true );

    EXPECT_EQ( report.CloudWinds, 1u );
    EXPECT_TRUE( report.Created );
    ASSERT_EQ( entities.size(), 2u );

    const auto cloud = BlockOf( entities[0], "VolumetricCloud" );
    EXPECT_FALSE( cloud.get( "WindDirection" ).has_value() );
    EXPECT_FALSE( cloud.get( "WindSpeed" ).has_value() );
    EXPECT_TRUE( cloud.get( "Enabled" ).has_value() );

    const EntityData* source = WindSourceOf( entities );
    ASSERT_NE( source, nullptr );
    ASSERT_TRUE( source->id.has_value() );
    const auto block     = BlockOf( *source, "WindSource" );
    const auto direction = block.get( "Direction" ).value().to_array().value();
    ASSERT_EQ( direction.size(), 3u );
    EXPECT_DOUBLE_EQ( direction[0].to_double().value(), 0.0 );
    EXPECT_DOUBLE_EQ( direction[2].to_double().value(), -1.0 );
    EXPECT_DOUBLE_EQ( block.get( "Speed" ).value().to_double().value(), 1250.0 );
    EXPECT_FALSE( block.get( "PointWind" ).value().to_bool().value() );

    // The same file migrated again names the same record.
    std::vector<EntityData> again{ Cloud( std::vector<double>{ 0.0, 0.0, -1.0 }, 1250.0 ) };
    Migration::MigrateWindSourceV41ToV42( again, "Fixture", true );
    EXPECT_EQ( WindSourceOf( again )->id, source->id );
}

TEST( SceneWindSourceMigration, MissingKeysWereTheOldDefaults )
{
    std::vector<EntityData> entities{ Cloud( std::nullopt, std::nullopt ) };
    const auto              report = Migration::MigrateWindSourceV41ToV42( entities, "Defaults", true );
    EXPECT_TRUE( report.Created );
    const auto block = BlockOf( *WindSourceOf( entities ), "WindSource" );
    EXPECT_DOUBLE_EQ( block.get( "Speed" ).value().to_double().value(), 3000.0 );
    EXPECT_DOUBLE_EQ( block.get( "Direction" ).value().to_array().value()[0].to_double().value(), 1.0 );
}

TEST( SceneWindSourceMigration, NoWindNoSource )
{
    std::vector<EntityData> entities{ Cloud( std::vector<double>{ 1.0, 0.0, 0.0 }, 0.0 ) };
    const auto              report = Migration::MigrateWindSourceV41ToV42( entities, "Still", true );
    EXPECT_EQ( report.CloudWinds, 1u );
    EXPECT_FALSE( report.Created );
    EXPECT_EQ( WindSourceOf( entities ), nullptr );
    EXPECT_FALSE( BlockOf( entities[0], "VolumetricCloud" ).get( "WindSpeed" ).has_value() );
}

TEST( SceneWindSourceMigration, APrefabLosesTheKeysAndGainsNoSource )
{
    std::vector<EntityData> entities{ Cloud( std::vector<double>{ 1.0, 0.0, 0.0 }, 900.0 ) };
    const auto              report = Migration::MigrateWindSourceV41ToV42( entities, "Prefab", false );
    EXPECT_EQ( report.CloudWinds, 1u );
    EXPECT_FALSE( report.Created );
    EXPECT_EQ( entities.size(), 1u );
}

TEST( SceneWindSourceMigration, ASecondLayersDifferentWindIsReported )
{
    std::vector<EntityData> entities{ Cloud( std::vector<double>{ 1.0, 0.0, 0.0 }, 900.0 ),
                                      Cloud( std::vector<double>{ 0.0, 0.0, 1.0 }, 900.0 ) };
    const auto              report = Migration::MigrateWindSourceV41ToV42( entities, "Two", true );
    EXPECT_EQ( report.CloudWinds, 2u );
    EXPECT_EQ( report.Disagreeing, 1u );
    EXPECT_EQ( entities.size(), 3u );
}
// NOLINTEND(bugprone-unchecked-optional-access)
