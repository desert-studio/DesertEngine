// The protocol scene is a MEASURING INSTRUMENT, and this suite is what keeps it from moving.
//
// WHY IT EXISTS. Every phase of the cloud programme measures itself at the same six camera points on the
// same scene, and Docs/Clouds/CALIBRATION.md §PR establishes what that scene actually was: seven of the
// cloud layer's fifty-one parameters were written in the file and the other forty-four came from whatever
// VolumetricCloudComponent.hpp said on the day somebody pressed render. Eight later phases moved a default
// in exactly those forty-four. So the sky the protocol measured changed twelve times while the file it was
// read from changed eight, and the two sets barely overlap — a diff of the scene between two phases showed
// nothing, correctly and uselessly.
//
// The cure is a scene that states ALL of its parameters, so that no change to a C++ default can reach it.
// A cure that nothing checks is a comment: the day somebody adds a fifty-second field to
// VolumetricCloudData, Clouds_Protocol.desce silently goes back to having a default in it, and the next
// six-point table is measured through that default with nobody the wiser.
//
// So the property is asserted rather than remembered, and it is asserted as a RELATION between two things
// obliged to agree (contract §2.3.1): the reflection table and the file on disk. Adding a field to a
// reflected component turns this suite RED, and the message names the field.
//
// It is a pure-function suite: it parses JSON, walks the reflection registry, and asks the loader's own
// version gate whether each file is one the engine will read. No GPU, no asset manager, no scene graph.

#include <Common/Json/Json.hpp>
#include <Engine/Assets/MaterialData.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <Common/Settings/MachineSettings.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using Desert::Core::SceneIsAtCurrentVersion;
using Desert::Core::SceneSerialized;
using Desert::Reflection::ReflectionRegistry;
using Desert::Reflection::TypeInfo;

namespace
{
    // The scenes this suite guards, and the ONE place that list lives. Clouds_Protocol is the six-point
    // ruler; the three PR_Hero scenes are the committed legs of §PR's per-pass cost measurement, and they
    // are guarded for the same reason — a cost number whose scene can be moved by a default is a cost
    // number that will not reproduce.
    const char* const kProtocolScenes[] = {
         "Clouds_Protocol.desce",
         "PR_Hero0.desce",
         "PR_Hero3.desce",
         "PR_Hero8.desce",
    };

    // Component key on disk -> reflected type name. Only the REFLECTED components are listed: the others
    // (StaticMesh, DirectionLight, Skybox) go through hand-written serializers in ComponentRegistry.cpp
    // that write a fixed set of keys, so they cannot grow a field behind a scene's back.
    struct ReflectedComponent
    {
        const char* Key;
        const char* TypeName;
    };

    constexpr ReflectedComponent kReflected[] = {
         { "VolumetricCloud", "VolumetricCloudData" },
         { "SkyAtmosphere", "SkyAtmosphereData" },
         { "ExponentialHeightFog", "ExponentialHeightFogData" },
         { "HeroCloud", "HeroCloudData" },
    };

    // Walks up from the working directory looking for a file only the repository has. Copied in shape
    // from Desert/Tests/Engine/SceneTonemapMigration, which needs the same thing for the same reason:
    // the test runner's working directory is not fixed.
    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + "Desert/Desert/Source/Engine/Core/SceneSettings.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ScenePath( const std::string& name )
    {
        return RepoRoot() + "Editor/Resources/Assets/Scenes/" + name;
    }

    // A project-relative asset path (as a scene states it) -> a path this suite can open. The cloud
    // material a layer names since O1 lives here, and comparing two legs' skies means reading it.
    std::string AssetPath( const std::string& relative )
    {
        return RepoRoot() + "Editor/Resources/Assets/" + relative;
    }

    std::string ReadAll( const std::string& path )
    {
        std::ifstream      in( path, std::ios::binary );
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Every key the parsed payload carries. Written by hand because rfl::Object exposes no `contains`.
    std::set<std::string> KeysOf( const Common::Json::Object& object )
    {
        std::set<std::string> keys;
        for ( auto it = object.begin(); it != object.end(); ++it )
            keys.insert( it->first );
        return keys;
    }

    const TypeInfo* Reflected( const char* typeName )
    {
        return ReflectionRegistry::Get().Find( typeName );
    }
} // namespace

// THE CLAIM: no field of a reflected component reaches the renderer from a C++ default when a protocol
// scene is loaded. Every one of them is written in the file.
//
// This is the assertion that goes red when somebody adds a field, and the whole point of the suite. The
// remedy when it does is not to edit this list — it is to write the new field into the four scenes at the
// value the phase intends the protocol to be measured at, and to say in CALIBRATION.md that the protocol
// scene moved and why.
TEST( CloudProtocolScene, EveryReflectedFieldIsWrittenExplicitlySoNoDefaultCanMoveTheProtocol )
{
    for ( const char* sceneName : kProtocolScenes )
    {
        const std::string json = ReadAll( ScenePath( sceneName ) );
        ASSERT_FALSE( json.empty() ) << sceneName << " is missing or empty";

        const auto parsed = Common::Json::Read<SceneSerialized>( json );
        ASSERT_TRUE( parsed ) << sceneName << " does not parse as a scene";

        int checkedComponents = 0;
        for ( const auto& entity : parsed.GetValue().Entities )
        {
            for ( const auto& component : kReflected )
            {
                const auto found = entity.Components.get( component.Key );
                if ( !found.has_value() )
                    continue;

                const auto payload = found.value().to_object();
                ASSERT_TRUE( payload ) << sceneName << ": '" << component.Key << "' is not an object";

                const TypeInfo* type = Reflected( component.TypeName );
                ASSERT_NE( type, nullptr ) << component.TypeName
                                           << " is not in the reflection registry, so nothing here means "
                                              "anything";

                const std::set<std::string> present = KeysOf( payload.value() );
                for ( const auto& field : type->Fields )
                    EXPECT_TRUE( present.count( field.Name ) != 0 )
                         << sceneName << ": '" << component.Key << "." << field.Name
                         << "' is NOT written in the protocol scene, so it comes from the C++ default and "
                            "the protocol moves the next time that default does. Write it into the scene "
                            "at the value the protocol is to be measured at.";
                ++checkedComponents;
            }
        }
        EXPECT_GT( checkedComponents, 0 ) << sceneName << " carries no reflected component at all";
    }
}

// The scene-wide settings block is reflected too, so the same rule applies to it: every field it can
// state, it states, or a C++ default moves the instrument.
TEST( CloudProtocolScene, TheSettingsBlockIsWrittenInFull )
{
    const TypeInfo* type = Reflected( "SceneSettings" );
    ASSERT_NE( type, nullptr );

    for ( const char* sceneName : kProtocolScenes )
    {
        const auto parsed = Common::Json::Read<SceneSerialized>( ReadAll( ScenePath( sceneName ) ) );
        ASSERT_TRUE( parsed ) << sceneName;
        const auto& settingsBlock = parsed.GetValue().Settings;
        if ( !settingsBlock.has_value() )
        {
            ADD_FAILURE() << sceneName << " has no Settings block";
            continue;
        }

        const auto settings = settingsBlock->to_object();
        ASSERT_TRUE( settings ) << sceneName << ": Settings is not an object";

        const std::set<std::string> present = KeysOf( settings.value() );
        for ( const auto& field : type->Fields )
            EXPECT_TRUE( present.count( field.Name ) != 0 )
                 << sceneName << ": 'Settings." << field.Name << "' is not written, so it is a default";
    }
}

// THE TIER THE PROTOCOL IS MEASURED AT, AND IT IS NO LONGER A KEY IN THE FILE.
//
// `EXPECT_TRUE( present.count( "CloudQualityTier" ) )` stood at the end of the test above — the tier every
// six-point table in CALIBRATION.md was implicitly shot at without ever naming it, pinned into the file so
// that it could not move. К3 moved it out: a quality tier is what a MACHINE can afford, and while it lived
// in the level file a weak machine could only turn it down by editing a file that goes to everybody.
//
// SO THE INSTRUMENT IS PINNED DIFFERENTLY, AND — this is the part worth stating — more tightly than
// before. A file could always be edited to say Low, and then one protocol scene would have been measured
// at a different tier from its three siblings with nothing to catch it. There is no such key now: every
// protocol scene is measured at whatever ONE answer the machine gives, and on a machine that has never
// chosen (which is every CI runner and every fresh worktree) that answer is the schema default. This
// asserts the two halves of that: the default is High, and no protocol scene may state a tier of its own.
TEST( CloudProtocolScene, TheTierTheProtocolIsMeasuredAtIsTheMachineDefaultAndThatDefaultIsHigh )
{
    EXPECT_EQ( Common::Settings::MachineSettings{}.CloudQualityTier, Common::Settings::CloudQuality::High )
         << "the calibrated reference tier moved; every number in Docs/Clouds/CALIBRATION.md was taken at "
            "High and a new default silently re-measures all of them";

    for ( const char* sceneName : kProtocolScenes )
    {
        const auto parsed = Common::Json::Read<SceneSerialized>( ReadAll( ScenePath( sceneName ) ) );
        ASSERT_TRUE( parsed ) << sceneName;
        const auto& settingsBlock = parsed.GetValue().Settings;
        if ( !settingsBlock.has_value() )
        {
            ADD_FAILURE() << sceneName << " has no Settings block";
            continue;
        }
        const auto settings = settingsBlock->to_object();
        ASSERT_TRUE( settings ) << sceneName;

        const std::set<std::string> present = KeysOf( settings.value() );
        for ( const char* key : { "CloudQualityTier", "AA", "MeshLOD", "TextureFilterMode", "Anisotropy" } )
            EXPECT_TRUE( present.count( key ) == 0 )
                 << sceneName << " states Settings." << key
                 << ", which is machine quality and cannot be in a level file (К3). Run Tools/SceneMigrator.";
    }
}

// THE SECOND CHANNEL. What the renderer sees has to be what the FILE says, and for a while that was not
// guaranteed: a file at an old schema was MIGRATED on load, so the numbers reaching the renderer were the
// migration's output and no reader of the .desce could tell.
//
// That whole class is gone. The loader REFUSES an old file now instead of repairing it (§4.3), so any scene
// it accepts is a scene it read verbatim, and the thing left to assert is the ACCEPTANCE: a protocol scene
// must be at the current generation of BOTH version integers, which is exactly the gate the loader applies.
// If one of these files ever falls behind, this suite goes red here rather than the measurement quietly
// moving - and the fix is the run of Tools/SceneMigrator that the loader's own refusal would name.
TEST( CloudProtocolScene, TheLoaderReadsTheseFilesVerbatimBecauseItAcceptsThemAtAll )
{
    for ( const char* sceneName : kProtocolScenes )
    {
        auto parsed = Common::Json::Read<SceneSerialized>( ReadAll( ScenePath( sceneName ) ) );
        ASSERT_TRUE( parsed ) << sceneName;

        const SceneSerialized& scene = parsed.GetValue();
        ASSERT_TRUE( scene.Header.has_value() ) << sceneName << " states no SceneVersion";
        ASSERT_TRUE( scene.Header.has_value() ) << sceneName << " states no UnitVersion";
        EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ),
                   Desert::Core::kSceneVersion )
             << sceneName;
        EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kUnitSchemaTag ),
                   Desert::Core::kUnitVersion )
             << sceneName;

        EXPECT_TRUE( SceneIsAtCurrentVersion( scene ) )
             << sceneName
             << " is not at the generation this engine loads, so the loader refuses it and the measurement "
                "this scene exists for cannot be taken at all. Convert it: Tools/SceneMigrator.";
    }
}

// The three cost legs differ in ONE thing, and a measurement whose A and B differ in two things measures
// neither. Asserted here rather than trusted to the script that wrote them.
TEST( CloudProtocolScene, TheThreeHeroCostLegsDifferOnlyInHowManyHeroCloudsAreEnabled )
{
    struct Leg
    {
        const char* Scene;
        int         Expected;
    };
    constexpr Leg kLegs[] = { { "PR_Hero0.desce", 0 }, { "PR_Hero3.desce", 3 }, { "PR_Hero8.desce", 8 } };

    std::vector<std::string> normalised;
    std::vector<std::string> cloudLook; // the CONTENT of each leg's cloud material, see below
    for ( const auto& leg : kLegs )
    {
        auto parsed = Common::Json::Read<SceneSerialized>( ReadAll( ScenePath( leg.Scene ) ) );
        ASSERT_TRUE( parsed ) << leg.Scene;

        SceneSerialized scene = parsed.GetValue();
        scene.SceneName       = "normalised";
        scene.Header          = std::nullopt; // each file is its own asset: the GUID differs by design

        int live = 0;
        for ( auto& entity : scene.Entities )
        {
            const auto found = entity.Components.get( "HeroCloud" );
            if ( !found.has_value() )
                continue;

            auto payload = found.value().to_object();
            ASSERT_TRUE( payload ) << leg.Scene;
            const auto enabled = payload.value()["Enabled"].to_bool();
            ASSERT_TRUE( enabled ) << leg.Scene << ": HeroCloud.Enabled is not a bool";
            if ( *enabled )
                ++live;

            // Erase the one field the legs are allowed to differ in, so the rest can be compared whole.
            payload.value()["Enabled"]     = false;
            entity.Components["HeroCloud"] = Common::Json::Value( payload.value() );
        }
        EXPECT_EQ( live, leg.Expected ) << leg.Scene << " does not carry the instance count its name claims";

        // SINCE O1 THE LOOK IS NOT IN THE SCENE, so comparing the scene text alone would no longer be
        // comparing the sky. Each leg names its own Materials/M_<Scene>_Clouds.demat — the migration is
        // per-scene and pure, so it cannot know three files should share one, and per-scene is the
        // behaviour an author wants anyway (editing one leg's material must not move the other two).
        // The path is therefore normalised out of the comparison and the thing it points AT is compared
        // instead: three legs whose materials differ by a digit measure three different skies however
        // identical the .desce files look.
        for ( auto& entity : scene.Entities )
        {
            const auto found = entity.Components.get( "VolumetricCloud" );
            if ( !found.has_value() )
                continue;

            auto payload = found.value().to_object();
            ASSERT_TRUE( payload ) << leg.Scene << ": the VolumetricCloud payload is not an object";
            const auto material = payload.value()["Material"].to_string();
            ASSERT_TRUE( material ) << leg.Scene
                                    << ": the cloud layer names no material, so its look comes from "
                                       "nowhere this suite can pin";

            const std::string materialJson = ReadAll( AssetPath( *material ) );
            EXPECT_FALSE( materialJson.empty() )
                 << leg.Scene << " names '" << *material << "', which is not on disk";

            auto parsedMaterial = Common::Json::Read<Desert::Assets::MaterialData>( materialJson );
            ASSERT_TRUE( parsedMaterial ) << leg.Scene << ": '" << *material << "' is not a material";

            // The header GUID is the FILE's identity, not the sky's — it is derived from the file's own
            // path so that two runs of the migration produce byte-identical output, which means three
            // legs that name three files necessarily carry three GUIDs. Comparing it would fail on a
            // difference that cannot reach a pixel; comparing everything else is the sky.
            Desert::Assets::MaterialData look = parsedMaterial.GetValue();
            look.Header                       = std::nullopt; // the file's identity, cleared for the comparison
            cloudLook.push_back( Common::Json::Write( look ) );

            payload.value()["Material"]          = std::string( "normalised" );
            entity.Components["VolumetricCloud"] = Common::Json::Value( payload.value() );
        }

        normalised.push_back( Common::Json::Write( scene ) );
    }

    for ( std::size_t i = 1; i < normalised.size(); ++i )
        EXPECT_EQ( normalised[0], normalised[i] )
             << "the hero cost legs differ in something other than HeroCloud.Enabled, so their A/B measures "
                "more than the instance count";

    ASSERT_EQ( cloudLook.size(), std::size( kLegs ) )
         << "a leg carries no cloud layer at all, so there is no sky to compare";
    for ( std::size_t i = 1; i < cloudLook.size(); ++i )
        EXPECT_EQ( cloudLook[0], cloudLook[i] )
             << "the hero cost legs' CLOUD MATERIALS differ, so their A/B measures a different sky as well "
                "as a different instance count";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
