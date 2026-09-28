// WHERE A PREFAB INSTANCE STANDS survives a save and a load (scene v37, PFX1).
//
// Since v37 a scene's instance record states its root's Translation/Rotation/Scale itself and the root's
// override no longer does. SceneSerializer.cpp reaches the renderer and no suite compiles it, so the two
// halves it runs are pure functions in PrefabData.hpp (ReduceInstanceRecord on save; StatedInstanceTransform
// and PlaceInstanceRoot on load) and this suite holds them - through the file, and against the loader's own
// order: prefab root, then its overrides, then the record's transform. The last test reads
// SceneSerializer.cpp and pins that it runs exactly these functions, in that order.

#include <Engine/Assets/Prefab/PrefabOverrides.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Json/Json.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Common::UUID;
using Desert::Assets::EntityData;
using Desert::Assets::PrefabOverrideData;

// NOLINTBEGIN(bugprone-unchecked-optional-access)
namespace
{
    // The prefab's root record: where the `.deprefab` puts its root, and a tag.
    EntityData PrefabRootRecord()
    {
        EntityData root;
        root.id          = UUID( 7001 );
        root.Tag         = std::string( "Crate" );
        root.Translation = glm::vec3( 10.0f, 20.0f, 30.0f );
        root.Rotation    = glm::vec3( 0.0f, 0.5f, 0.0f );
        root.Scale       = glm::vec3( 1.0f );
        return root;
    }

    // What the entity serializer writes for a live instance root that was moved to (-450, 0, 1200), turned and
    // scaled, renamed, and whose child 7002 was renamed too.
    struct LiveInstance
    {
        EntityData                      Record;
        std::vector<PrefabOverrideData> Overrides;
    };
    LiveInstance MovedAndRenamedInstance()
    {
        LiveInstance live;
        live.Record.id          = UUID( 42 );
        live.Record.PrefabPath  = std::string( "Prefabs/Crate.deprefab" );
        live.Record.Tag         = std::string( "Crate (moved)" );
        live.Record.Translation = glm::vec3( -450.0f, 0.0f, 1200.0f );
        live.Record.Rotation    = glm::vec3( 0.0f, 1.25f, 0.0f );
        live.Record.Scale       = glm::vec3( 2.0f );

        PrefabOverrideData root; // what CapturePrefabInstance diffs off the root: its tag AND its transform
        root.Path        = { UUID( 7001 ) };
        root.Tag         = std::string( "Crate (moved)" );
        root.Translation = live.Record.Translation;
        root.Rotation    = live.Record.Rotation;
        root.Scale       = live.Record.Scale;
        PrefabOverrideData child;
        child.Path     = { UUID( 7002 ) };
        child.Tag      = std::string( "Lid (renamed)" );
        live.Overrides = { root, child };
        return live;
    }

    // Save: the saver's reduction, then the file.
    Desert::Core::SceneSerialized SaveThroughTheFile( const LiveInstance& live )
    {
        EntityData              record   = live.Record;
        const std::vector<UUID> rootPath = { UUID( 7001 ) };
        Desert::Assets::ReduceInstanceRecord( record, live.Overrides, &rootPath );

        Desert::Core::SceneSerialized scene;
        scene.SceneName = "PrefabInstanceTransform";
        scene.Entities.push_back( record );
        const std::string json   = Common::Json::Write( scene );
        const auto        parsed = Common::Json::Read<Desert::Core::SceneSerialized>( json );
        EXPECT_TRUE( parsed ) << json;
        return parsed ? parsed.GetValue() : Desert::Core::SceneSerialized{};
    }

    // Stands in for ECS::TransformComponent: PlaceInstanceRoot writes the same three fields.
    struct Transform
    {
        glm::vec3 Translation{ 0.0f };
        glm::vec3 Rotation{ 0.0f };
        glm::vec3 Scale{ 1.0f };
    };

    // Load, in the loader's order: the prefab's root, its override laid over it, then the record's transform.
    // Refused -> nothing is instantiated, which the returned flag says.
    struct Loaded
    {
        bool        Instantiated = false;
        std::string Refusal;
        EntityData  Root;
        Transform   RootTransform;
    };
    Loaded LoadInstance( const EntityData& record )
    {
        Loaded     loaded;
        const auto stated = Desert::Assets::StatedInstanceTransform( record );
        if ( !stated )
        {
            loaded.Refusal = stated.GetError();
            return loaded;
        }
        loaded.Instantiated = true;
        loaded.Root         = PrefabRootRecord();
        if ( record.PrefabOverrides.has_value() )
            for ( const PrefabOverrideData& over : *record.PrefabOverrides )
                if ( over.Path == std::vector<UUID>{ UUID( 7001 ) } )
                    Desert::Assets::LayerOverrideOntoRecord( loaded.Root, over );
        loaded.RootTransform = { *loaded.Root.Translation, *loaded.Root.Rotation, *loaded.Root.Scale };
        Desert::Assets::PlaceInstanceRoot( loaded.RootTransform, stated.GetValue() );
        return loaded;
    }

    std::string ReadText( const std::filesystem::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  text;
        text << in.rdbuf();
        return text.str();
    }
} // namespace

TEST( PrefabInstanceTransform, TheRecordStatesTheTransformAndTheRootOverrideDoesNot )
{
    const auto scene = SaveThroughTheFile( MovedAndRenamedInstance() );
    ASSERT_EQ( scene.Entities.size(), 1u );
    const EntityData& saved = scene.Entities.front();

    ASSERT_TRUE( saved.Translation && saved.Rotation && saved.Scale ) << "the record states where it stands";
    EXPECT_EQ( *saved.Translation, glm::vec3( -450.0f, 0.0f, 1200.0f ) );
    EXPECT_EQ( *saved.Rotation, glm::vec3( 0.0f, 1.25f, 0.0f ) );
    EXPECT_EQ( *saved.Scale, glm::vec3( 2.0f ) );
    EXPECT_FALSE( saved.Tag.has_value() ) << "the root's tag is an override, not a record field";

    ASSERT_TRUE( saved.PrefabOverrides.has_value() );
    ASSERT_EQ( saved.PrefabOverrides->size(), 2u ) << "the root override keeps its tag, the child's is untouched";
    const PrefabOverrideData& root = saved.PrefabOverrides->at( 0 );
    EXPECT_EQ( root.Path, std::vector<UUID>{ UUID( 7001 ) } );
    EXPECT_FALSE( root.Translation.has_value() );
    EXPECT_FALSE( root.Rotation.has_value() );
    EXPECT_FALSE( root.Scale.has_value() );
    EXPECT_EQ( root.Tag, std::optional<std::string>( "Crate (moved)" ) );
    EXPECT_EQ( saved.PrefabOverrides->at( 1 ).Tag, std::optional<std::string>( "Lid (renamed)" ) );
}

TEST( PrefabInstanceTransform, ARootOverrideOfTheTransformAloneIsDroppedWhole )
{
    LiveInstance live = MovedAndRenamedInstance();
    live.Overrides.front().Tag.reset();
    live.Overrides.pop_back();
    const auto scene = SaveThroughTheFile( live );
    ASSERT_EQ( scene.Entities.size(), 1u );
    EXPECT_FALSE( scene.Entities.front().PrefabOverrides.has_value() ) << Common::Json::Write( scene );
    EXPECT_TRUE( scene.Entities.front().Translation.has_value() );
}

TEST( PrefabInstanceTransform, TheLoaderPutsTheInstanceWhereTheRecordSaysNotWhereThePrefabRootStands )
{
    const auto scene  = SaveThroughTheFile( MovedAndRenamedInstance() );
    const auto loaded = LoadInstance( scene.Entities.front() );

    ASSERT_TRUE( loaded.Instantiated ) << loaded.Refusal;
    EXPECT_EQ( loaded.RootTransform.Translation, glm::vec3( -450.0f, 0.0f, 1200.0f ) )
         << "the prefab's root stands at (10, 20, 30); the instance must not";
    EXPECT_EQ( loaded.RootTransform.Rotation, glm::vec3( 0.0f, 1.25f, 0.0f ) );
    EXPECT_EQ( loaded.RootTransform.Scale, glm::vec3( 2.0f ) );
    EXPECT_EQ( loaded.Root.Tag, std::optional<std::string>( "Crate (moved)" ) )
         << "the rest of the override applies";
}

TEST( PrefabInstanceTransform, SaveLoadSaveStatesTheSameRecord )
{
    const auto first  = SaveThroughTheFile( MovedAndRenamedInstance() );
    const auto loaded = LoadInstance( first.Entities.front() );
    ASSERT_TRUE( loaded.Instantiated ) << loaded.Refusal;

    LiveInstance again       = MovedAndRenamedInstance();
    again.Record.Translation = loaded.RootTransform.Translation;
    again.Record.Rotation    = loaded.RootTransform.Rotation;
    again.Record.Scale       = loaded.RootTransform.Scale;
    EXPECT_EQ( Common::Json::Write( SaveThroughTheFile( again ) ), Common::Json::Write( first ) );
}

TEST( PrefabInstanceTransform, ARecordWithoutItsTransformIsRefusedByPrefabAndReasonAndNotLoaded )
{
    auto       scene  = SaveThroughTheFile( MovedAndRenamedInstance() );
    EntityData record = scene.Entities.front();
    record.Rotation.reset();
    record.Scale.reset();

    const auto loaded = LoadInstance( record );
    EXPECT_FALSE( loaded.Instantiated );
    EXPECT_NE( loaded.Refusal.find( "'Prefabs/Crate.deprefab'" ), std::string::npos ) << loaded.Refusal;
    EXPECT_NE( loaded.Refusal.find( "states no Rotation, Scale" ), std::string::npos ) << loaded.Refusal;
    EXPECT_EQ( loaded.Refusal.find( "Translation" ), std::string::npos )
         << "only what is missing: " << loaded.Refusal;
}

TEST( PrefabInstanceTransform, TheSceneLoaderAndSaverRunTheseFunctionsInThisOrder )
{
    const auto root = Desert::TestSupport::RepositoryRoot();
    ASSERT_FALSE( root.empty() ) << "no checkout above the working directory";
    const std::string loader = ReadText( root / "Desert" / "Desert" / "Source" / "Engine" / "Core" / "Serialize" /
                                         "SceneSerializer.cpp" );
    ASSERT_FALSE( loader.empty() );

    const auto reduce = loader.find( "Assets::ReduceInstanceRecord(" );
    const auto stated = loader.find( "Assets::StatedInstanceTransform(" );
    const auto find   = loader.find( "FindByPath<Assets::PrefabAsset>(", stated );
    const auto apply  = loader.find( "PrefabFactory::ApplyOverrides(", stated );
    const auto place  = loader.find( "Assets::PlaceInstanceRoot(", stated );
    ASSERT_NE( reduce, std::string::npos ) << "the saver reduces an instance record with ReduceInstanceRecord";
    ASSERT_NE( stated, std::string::npos )
         << "the loader reads the record's transform with StatedInstanceTransform";
    ASSERT_NE( find, std::string::npos );
    ASSERT_NE( apply, std::string::npos );
    ASSERT_NE( place, std::string::npos ) << "the loader places the root with PlaceInstanceRoot";
    EXPECT_LT( stated, find ) << "a refused record must be refused before its prefab is looked up";
    EXPECT_LT( apply, place ) << "the record's transform goes on AFTER the overrides";
    EXPECT_EQ( loader.find( "Assets::PlaceInstanceRoot(", place + 1 ), std::string::npos );
}
// NOLINTEND(bugprone-unchecked-optional-access)
