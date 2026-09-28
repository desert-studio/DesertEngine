// MigrateInstanceTransformsV36ToV37 (PFX1): a v36 scene's prefab-instance record gains its root's transform.
// Each of Translation/Rotation/Scale comes from the instance's root override when it states it (and the
// override loses it), else from the prefab's root record, else the TransformComponent default - the order the
// v36 loader resolved it in. A prefab that cannot be read refuses the file by record and path.

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Json/Json.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Migration = Desert::Migration;
namespace fs        = std::filesystem;
using Common::UUID;
using Desert::Assets::EntityData;
using Desert::Assets::PrefabOverrideData;

// NOLINTBEGIN(bugprone-unchecked-optional-access)
namespace
{
    constexpr const char* kPrefab = "Prefabs/Crate.deprefab";

    // <scratch>/Resources/Assets/Prefabs/Crate.deprefab, whose root 7001 stands at (10, 20, 30) turned by
    // (0, 0.5, 0) and states no Scale.
    struct Project
    {
        Desert::TestSupport::ScratchDir Scratch{ "pfx1-migrate" };
        fs::path                        AssetsRoot = Scratch.Path() / "Resources" / "Assets";

        Project()
        {
            fs::create_directories( AssetsRoot / "Prefabs" );
            Desert::Assets::PrefabData prefab;
            prefab.Name = "Crate";
            prefab.Root = UUID( 7001 );
            EntityData root;
            root.id          = UUID( 7001 );
            root.Tag         = std::string( "Crate" );
            root.Translation = glm::vec3( 10.0f, 20.0f, 30.0f );
            root.Rotation    = glm::vec3( 0.0f, 0.5f, 0.0f );
            prefab.Entities.push_back( root );
            EntityData lid;
            lid.id     = UUID( 7002 );
            lid.parent = UUID( 7001 );
            prefab.Entities.push_back( lid );
            std::ofstream( AssetsRoot / kPrefab, std::ios::binary ) << Common::Json::Write( prefab );
        }
    };

    EntityData Instance( const char* prefabPath = kPrefab )
    {
        EntityData record;
        record.id         = UUID( 42 );
        record.PrefabPath = std::string( prefabPath );
        return record;
    }
} // namespace

TEST( SceneInstanceTransformsMigration, TheRootOverrideMovesOntoTheRecordAndTheRestComesFromThePrefabRoot )
{
    const Project      project;
    EntityData         record = Instance();
    PrefabOverrideData root;
    root.Path        = { UUID( 7001 ) };
    root.Tag         = std::string( "Crate (moved)" );
    root.Translation = glm::vec3( -450.0f, 0.0f, 1200.0f );
    PrefabOverrideData lid;
    lid.Path               = { UUID( 7002 ) };
    lid.Scale              = glm::vec3( 3.0f );
    record.PrefabOverrides = std::vector<PrefabOverrideData>{ root, lid };
    std::vector<EntityData> entities{ record };

    const auto report = Migration::MigrateInstanceTransformsV36ToV37( entities, project.AssetsRoot );

    EXPECT_TRUE( report.UnknownNames.empty() ) << report.UnknownNames.front();
    EXPECT_EQ( report.Stated, 1 );
    const EntityData& out = entities.front();
    ASSERT_TRUE( out.Translation && out.Rotation && out.Scale );
    EXPECT_EQ( *out.Translation, glm::vec3( -450.0f, 0.0f, 1200.0f ) ) << "the override, not the prefab root";
    EXPECT_EQ( *out.Rotation, glm::vec3( 0.0f, 0.5f, 0.0f ) ) << "the prefab root, the override states none";
    EXPECT_EQ( *out.Scale, glm::vec3( 1.0f ) ) << "neither states it: the component default";
    ASSERT_TRUE( out.PrefabOverrides.has_value() );
    ASSERT_EQ( out.PrefabOverrides->size(), 2u );
    EXPECT_FALSE( out.PrefabOverrides->at( 0 ).Translation.has_value() ) << "stated once, on the record";
    EXPECT_EQ( out.PrefabOverrides->at( 0 ).Tag, std::optional<std::string>( "Crate (moved)" ) );
    EXPECT_EQ( out.PrefabOverrides->at( 1 ).Scale, std::optional<glm::vec3>( glm::vec3( 3.0f ) ) )
         << "a child's transform override is not the instance's";
}

TEST( SceneInstanceTransformsMigration, WithoutAnOverrideThePrefabRootIsWhereTheInstanceStood )
{
    const Project           project;
    std::vector<EntityData> entities{ Instance() };

    const auto report = Migration::MigrateInstanceTransformsV36ToV37( entities, project.AssetsRoot );

    EXPECT_TRUE( report.UnknownNames.empty() );
    EXPECT_EQ( report.Stated, 1 );
    const EntityData& out = entities.front();
    ASSERT_TRUE( out.Translation && out.Rotation && out.Scale );
    EXPECT_EQ( *out.Translation, glm::vec3( 10.0f, 20.0f, 30.0f ) );
    EXPECT_EQ( *out.Rotation, glm::vec3( 0.0f, 0.5f, 0.0f ) );
    EXPECT_EQ( *out.Scale, glm::vec3( 1.0f ) );
    EXPECT_FALSE( out.PrefabOverrides.has_value() );
}

TEST( SceneInstanceTransformsMigration, AnOverrideOfTheTransformAloneIsDroppedAndAStatedRecordIsLeftAlone )
{
    const Project      project;
    EntityData         moved = Instance();
    PrefabOverrideData root;
    root.Path             = { UUID( 7001 ) };
    root.Scale            = glm::vec3( 2.0f );
    moved.PrefabOverrides = std::vector<PrefabOverrideData>{ root };
    EntityData stated     = Instance();
    stated.id             = UUID( 43 );
    stated.Translation    = glm::vec3( 1.0f );
    stated.Rotation       = glm::vec3( 0.0f );
    stated.Scale          = glm::vec3( 5.0f );
    std::vector<EntityData> entities{ moved, stated };

    const auto report = Migration::MigrateInstanceTransformsV36ToV37( entities, project.AssetsRoot );

    EXPECT_EQ( report.Stated, 1 ) << "the stated record is not counted";
    EXPECT_EQ( entities[0].Scale, std::optional<glm::vec3>( glm::vec3( 2.0f ) ) );
    EXPECT_FALSE( entities[0].PrefabOverrides.has_value() ) << "nothing else was in the override";
    EXPECT_EQ( entities[1].Scale, std::optional<glm::vec3>( glm::vec3( 5.0f ) ) );
}

TEST( SceneInstanceTransformsMigration, AMissingPrefabRefusesTheRecordByIdAndPathAndLeavesItAlone )
{
    const Project           project;
    std::vector<EntityData> entities{ Instance( "Prefabs/Gone.deprefab" ) };

    const auto report = Migration::MigrateInstanceTransformsV36ToV37( entities, project.AssetsRoot );

    EXPECT_EQ( report.Stated, 0 );
    ASSERT_EQ( report.UnknownNames.size(), 1u );
    EXPECT_NE( report.UnknownNames.front().find( "Entities[id=42] > 'Prefabs/Gone.deprefab'" ), std::string::npos )
         << report.UnknownNames.front();
    EXPECT_FALSE( entities.front().Translation.has_value() );
}

TEST( SceneInstanceTransformsMigration, AMissingPrefabRefusesTheWholeSceneUnstamped )
{
    const Project project;
    auto          scene = rfl::json::read<Migration::SceneSerialized>(
         R"({"Header":{"Kind":"Scene","Guid":"00000000000000000000000000000037",)"
                  R"("Versions":{"SCNE":36,"UNIT":1},"Dependencies":[]},"SceneName":"S","Entities":[)"
                  R"({"id":42,"PrefabPath":"Prefabs/Gone.deprefab"}]})" );
    ASSERT_TRUE( scene.has_value() ) << scene.error().what();

    const auto report = Migration::MigrateScene( scene.value(), project.AssetsRoot, "" );

    EXPECT_NE( report.Refused.find( "Prefabs/Gone.deprefab" ), std::string::npos ) << report.Refused;
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.value().Header, Desert::Assets::kSceneSchemaTag ), 36 );
}
// NOLINTEND(bugprone-unchecked-optional-access)
