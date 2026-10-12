// SCENE-DEPS: the records a save writes for a mesh with a material slot and a sound gather to exactly those
// three GUIDs, sorted - the rule (Core::GatherSceneDependencies) that both the save and SceneMigrator call.

#include <Engine/Core/Serialize/SceneDependencies.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

TEST( SceneDependenciesRule, AMeshAMaterialAndASoundAreExactlyTheirThreeGuids )
{
    const auto record = rfl::json::read<Desert::Assets::EntityData>(
         R"({"id":7,"Tag":"Crate","Translation":[1.0,2.0,3.0],)"
         R"("StaticMesh":{"MeshGuid":"3333333333333333333333333333333a","MeshPath":"Meshes/Crate.stmesh",)"
         R"("MaterialGuids":["1111111111111111111111111111111b"],"MaterialPaths":["Materials/Crate.demat"]},)"
         R"("AudioSource":{"Sound":{"Guid":"2222222222222222222222222222222c","Path":"Sounds/Hit.desound"},"Volume":0.5}})" );
    ASSERT_TRUE( record ) << record.error().what();

    const auto gather = Desert::Core::GatherSceneDependencies( { record.value() }, nullptr,
                                                               std::filesystem::temp_directory_path() );
    EXPECT_TRUE( gather.Refused.empty() );
    const std::vector<std::string> expected = { "1111111111111111111111111111111b",
                                                "2222222222222222222222222222222c",
                                                "3333333333333333333333333333333a" };
    EXPECT_EQ( gather.Guids, expected );
}
