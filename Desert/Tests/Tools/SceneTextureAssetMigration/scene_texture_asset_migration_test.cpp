// The v23 -> v24 scene step (AF3e): a texture named by its per-texture cooked file
// (`cooked:Textures/<p>.tex`, a file no build writes any more) is renamed to the texture asset its source
// was imported into (`assets:Textures/<p>.detex`), in every field of every component and in Settings.

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs        = std::filesystem;
namespace Migration = Desert::Migration;

namespace
{
    // The two fields UI_SpriteSlots.desce carried, a nested array, a Settings field, and two strings that
    // only LOOK close: a `.tex` outside Textures/ and a Textures/ path that is not a `.tex`.
    constexpr const char* kV23Scene = R"({"SceneName":"S","Entities":[
        {"id":1,"Tag":"Canvas","UICanvas":{"Visible":true,"Sprite":"cooked:Textures/T_Checker.tex"}},
        {"id":2,"parent":1,"Tag":"Panel","UIPanel":{"Opacity":1.0,"Sprite":"cooked:Textures/Sub/T_Gone.tex"}},
        {"id":3,"Tag":"Other","Flipbook":{"Frames":["cooked:Textures/T_Checker.tex","assets:Textures/T_A.detex"]},
         "UIText":{"Text":"cooked:Meshes/M.tex","Font":"cooked:Textures/T_Checker.png"}}],
        "Settings":{"SplashSprite":"cooked:Textures/T_Checker.tex","Gravity":981.0},
        "UnitVersion":1,"SceneVersion":23})";

    struct Fixture
    {
        fs::path Root;
        Fixture()
        {
            // A fixed name, emptied first: no <unistd.h> getpid, which MSVC does not have.
            Root = fs::temp_directory_path() / "SceneTextureAssetMigration";
            std::error_code ec;
            fs::remove_all( Root, ec );
            fs::create_directories( Root / "Textures" );
            std::ofstream( Root / "Textures" / "T_Checker.detex" ) << "asset";
        }
        ~Fixture()
        {
            std::error_code ec;
            fs::remove_all( Root, ec );
        }
    };

    Desert::Core::SceneSerialized Parse( const char* json )
    {
        auto scene = rfl::json::read<Desert::Core::SceneSerialized>( json );
        EXPECT_TRUE( scene ) << "the fixture does not parse";
        return scene.value();
    }

    std::string Field( const Desert::Core::SceneSerialized& scene, size_t entity, const char* component,
                       const char* key )
    {
        const auto payload = scene.Entities[entity].Components.get( component ).value().to_object().value();
        return payload.get( key ).value().to_string().value();
    }
} // namespace

TEST( SceneTextureAssetMigration, TheStepIsTheHeadAndTheHeadIsWhatTheEngineRequires )
{
    EXPECT_EQ( Migration::kSceneVersionTextureAssetRefs, Desert::Core::kSceneVersion );
    EXPECT_EQ( Migration::kSceneVersionTextureAssetRefs, Migration::kSceneVersionProceduralTerrain + 1 );
}

TEST( SceneTextureAssetMigration, EveryCookedTextureStringNamesItsAssetAndNothingElseMoves )
{
    const Fixture fixture;
    auto          scene  = Parse( kV23Scene );
    const auto    report = Migration::MigrateScene( scene, fixture.Root );

    ASSERT_TRUE( report.TextureAssetRefsRaised );
    EXPECT_EQ( scene.SceneVersion.value_or( 0 ), Desert::Core::kSceneVersion );

    EXPECT_EQ( Field( scene, 0, "UICanvas", "Sprite" ), "assets:Textures/T_Checker.detex" );
    EXPECT_EQ( Field( scene, 1, "UIPanel", "Sprite" ), "assets:Textures/Sub/T_Gone.detex" );
    const auto frames = scene.Entities[2]
                             .Components.get( "Flipbook" )
                             .value()
                             .to_object()
                             .value()
                             .get( "Frames" )
                             .value()
                             .to_array()
                             .value();
    EXPECT_EQ( frames[0].to_string().value(), "assets:Textures/T_Checker.detex" );
    EXPECT_EQ( frames[1].to_string().value(), "assets:Textures/T_A.detex" );
    EXPECT_EQ( Field( scene, 2, "UIText", "Text" ), "cooked:Meshes/M.tex" );
    EXPECT_EQ( Field( scene, 2, "UIText", "Font" ), "cooked:Textures/T_Checker.png" );

    const auto settings = scene.Settings.value().to_object().value();
    EXPECT_EQ( settings.get( "SplashSprite" ).value().to_string().value(), "assets:Textures/T_Checker.detex" );
    EXPECT_EQ( Field( scene, 1, "UIPanel", "Sprite" ).find( "cooked:" ), std::string::npos );

    // Four rewritten, and the one whose asset is not under the root is NAMED, not silently kept.
    EXPECT_EQ( report.TextureAssetRefs.Rewritten, 4 );
    ASSERT_EQ( report.TextureAssetRefs.MissingNames.size(), 1u );
    EXPECT_NE( report.TextureAssetRefs.MissingNames[0].find( "Panel > UIPanel.Sprite" ), std::string::npos )
         << report.TextureAssetRefs.MissingNames[0];
}

TEST( SceneTextureAssetMigration, AFileAlreadyAtTheHeadIsNotTouched )
{
    const Fixture fixture;
    auto          scene = Parse( kV23Scene );
    Migration::MigrateScene( scene, fixture.Root );
    const std::string once  = rfl::json::write( scene );
    const auto        again = Migration::MigrateScene( scene, fixture.Root );
    EXPECT_FALSE( again.TextureAssetRefsRaised );
    EXPECT_EQ( rfl::json::write( scene ), once );
}

// The corpus: no tracked scene still names a cooked texture file once the tool has run over it.
TEST( SceneTextureAssetMigration, NoSceneInTheRepositoryNamesACookedTexture )
{
    fs::path repo = fs::current_path();
    while ( !repo.empty() && !fs::exists( repo / "Editor" / "Resources" ) && repo != repo.root_path() )
        repo = repo.parent_path();
    ASSERT_TRUE( fs::exists( repo / "Editor" / "Resources" ) ) << "run from inside the checkout";

    int scanned = 0;
    for ( const auto& entry : fs::recursive_directory_iterator( repo / "Editor" / "Resources" ) )
    {
        const auto ext = entry.path().extension();
        if ( ext != ".desce" && ext != ".deprefab" )
            continue;
        ++scanned;
        std::ifstream     in( entry.path(), std::ios::binary );
        const std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
        EXPECT_EQ( text.find( "cooked:Textures/" ), std::string::npos ) << entry.path().string();
    }
    EXPECT_GT( scanned, 10 ) << "the census found almost no scenes: it is looking in the wrong place";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
