// MigrateLandscapeLayerModesV37ToV38 (LS-16): the landscape's built-in grass/rock/snow switches leave every
// LandscapeMaterial block - on a record and in a prefab override - and nothing else in the block moves.

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
    rfl::Generic LandscapeMaterialBlock()
    {
        rfl::Generic::Object block;
        block["Material"]  = rfl::Generic( std::string( "Materials/M_Ground.demat" ) );
        block["GrassMode"] = rfl::Generic( 0 );
        block["RockMode"]  = rfl::Generic( 1 );
        block["SnowMode"]  = rfl::Generic( 1 );
        return rfl::Generic( std::move( block ) );
    }

    rfl::Generic::Object BlockOf( const rfl::ExtraFields<rfl::Generic>& components )
    {
        return components.get( "LandscapeMaterial" ).value().to_object().value();
    }
} // namespace

TEST( SceneLandscapeLayerModesMigration, VersionIsTheStepsGeneration )
{
    EXPECT_EQ( Migration::kSceneVersionNoLandscapeLayerModes, 38 );
    EXPECT_EQ( Desert::Core::kSceneVersion, Migration::kSceneVersionNoLandscapeLayerModes );
}

TEST( SceneLandscapeLayerModesMigration, DropsTheThreeModesAndKeepsTheMaterial )
{
    std::vector<EntityData> entities( 1 );
    entities[0].Components["LandscapeMaterial"] = LandscapeMaterialBlock();

    EXPECT_EQ( Migration::MigrateLandscapeLayerModesV37ToV38( entities ), 3u );

    const auto block = BlockOf( entities[0].Components );
    for ( const char* retired : Migration::kRetiredLandscapeLayerModeKeys )
        EXPECT_FALSE( block.get( retired ).has_value() ) << retired;
    ASSERT_TRUE( block.get( "Material" ).has_value() );
    EXPECT_EQ( block.get( "Material" ).value().to_string().value(), "Materials/M_Ground.demat" );
}

TEST( SceneLandscapeLayerModesMigration, PrefabOverridesLoseThemToo )
{
    std::vector<EntityData> entities( 1 );
    PrefabOverrideData      override_;
    override_.Components["LandscapeMaterial"] = LandscapeMaterialBlock();
    entities[0].PrefabOverrides               = std::vector<PrefabOverrideData>{ override_ };

    EXPECT_EQ( Migration::MigrateLandscapeLayerModesV37ToV38( entities ), 3u );
    EXPECT_FALSE( BlockOf( entities[0].PrefabOverrides->at( 0 ).Components ).get( "GrassMode" ).has_value() );
}

TEST( SceneLandscapeLayerModesMigration, OtherComponentsAreUntouched )
{
    std::vector<EntityData> entities( 1 );
    rfl::Generic::Object    landscape;
    landscape["GrassMode"]              = rfl::Generic( 1 ); // not a LandscapeMaterial block: not ours
    entities[0].Components["Landscape"] = rfl::Generic( std::move( landscape ) );

    EXPECT_EQ( Migration::MigrateLandscapeLayerModesV37ToV38( entities ), 0u );
    EXPECT_TRUE(
         entities[0].Components.get( "Landscape" ).value().to_object().value().get( "GrassMode" ).has_value() );
}
// NOLINTEND(bugprone-unchecked-optional-access)
