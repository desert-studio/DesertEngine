// MigrateLandscapeLayerModesV37ToV38 (LS-16): the landscape's built-in grass/rock/snow switches leave every
// LandscapeMaterial block - on a record and in a prefab override - and nothing else in the block moves.

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <gtest/gtest.h>

#include <TestSupport/scratch_dir.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
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
        return { std::move( block ) };
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

// THE CORPUS AFTER THE STEP, block by block: a landscape root states only the kept fields (named rows, UE's
// equivalents beside them), its look only the material, and every landscape lists the Ground layer first -
// the layer UE shows where nothing is painted.
TEST( SceneLandscapeLayerModesMigration, CorpusLandscapesStateOnlyTheKeptFields )
{
    const std::set<std::string> keptLandscape = {
         "QuadsPerTile", // UE ComponentSizeQuads / SubsectionSizeQuads
         "SpacingCm",    // ALandscape actor scale, XY
         "ZScale",       // ALandscape actor scale, Z
         "Layers",       // target layers -> ULandscapeLayerInfoObject
    };
    const std::set<std::string> keptLook = { "Material" }; // ALandscape::LandscapeMaterial

    const std::filesystem::path scenes =
         Desert::TestSupport::RepositoryRoot() / "Editor" / "Resources" / "Assets" / "Scenes";
    ASSERT_TRUE( std::filesystem::is_directory( scenes ) ) << std::filesystem::absolute( scenes );
    size_t roots = 0;
    for ( const auto& entry : std::filesystem::directory_iterator( scenes ) )
    {
        if ( entry.path().extension() != ".desce" )
            continue;
        const std::ifstream in( entry.path(), std::ios::binary );
        std::stringstream text;
        text << in.rdbuf();
        const auto scene = rfl::json::read<Migration::SceneSerialized>( text.str() );
        ASSERT_TRUE( scene ) << entry.path();
        for ( const EntityData& e : scene.value().Entities )
        {
            const auto landscape = e.Components.get( "Landscape" );
            if ( !landscape )
                continue;
            ++roots;
            const auto block = landscape.value().to_object().value();
            for ( const auto& [key, value] : block )
                EXPECT_TRUE( keptLandscape.contains( key ) ) << entry.path() << ": Landscape." << key;
            const auto layers = block.get( "Layers" );
            ASSERT_TRUE( layers ) << entry.path() << " lists no layer; its ground would be white";
            const auto first = layers.value().to_array().value().at( 0 ).to_object().value().get( "Path" );
            EXPECT_EQ( first.value().to_string().value(), "Landscape/Layers/Ground.delayerinfo" ) << entry.path();
            if ( const auto look = e.Components.get( "LandscapeMaterial" ) )
            {
                // Bound first: iterating `look.value().to_object().value()` directly walks an object owned by
                // a temporary that dies before the loop body (ASan: stack-use-after-scope).
                const auto lookBlock = look.value().to_object().value();
                for ( const auto& [key, value] : lookBlock )
                    EXPECT_TRUE( keptLook.contains( key ) ) << entry.path() << ": LandscapeMaterial." << key;
            }
        }
    }
    // G3_TwoTerrains (2), Terrain_Grass, Terrain_MatProbe.
    EXPECT_EQ( roots, 4u );
}

// NOLINTEND(bugprone-unchecked-optional-access)
