// The v26 -> v27 scene step (AF7d): StaticMesh/SkinnedMesh/InstancedStaticMesh `MaterialGuids` moves from
// the old MATL v1 u64 material id (read back as int64, often negative) to the GUID text of the `.demat`
// that stated it, translated through the legacy register (LegacyMaterialIds.hpp). 0 -> "". An id the
// register does not know REFUSES the file and leaves it unstamped (MigrateMaterialGuidsV26ToV27).

#include <SceneMigration.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <rflcpp/rfl/json.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Migration = Desert::Migration;
using Common::Content::AssetGuid;
using Common::Content::AssetGuidFromText;
using Common::Content::AssetGuidToText;
using Common::Content::HandleForGuid;

namespace
{
    // Two fixed material GUIDs (never Generate()d, so the test is deterministic and the printed text is
    // reproducible in a failure message).
    const AssetGuid kGuidA{ 0x1111111111111111ULL, 0x2222222222222222ULL }; // what old id -42 becomes
    const AssetGuid kGuidB{ 0x3333333333333333ULL, 0x4444444444444444ULL }; // what old id 7 becomes

    // The u64 the writer stored -42 as: MigrateMaterialGuidsV26ToV27 reads the array back as int64 (the
    // v1 writer stored the u64 through a signed JSON integer) and folds it with the same cast.
    constexpr uint64_t kOldIdNegative = static_cast<uint64_t>( -42LL );
    constexpr uint64_t kOldIdSeven    = 7ULL;
    constexpr uint64_t kOldIdUnknown  = 999ULL;

    std::string V26Header( const char* guidHex )
    {
        return std::string( R"("Header":{"Kind":"Scene","Guid":")" ) + guidHex +
               R"(","Versions":{"SCNE":26,"UNIT":1},"Dependencies":[]})";
    }

    const char* kSceneGuid = "00000000000000000000000000000001";

    // One StaticMesh, one SkinnedMesh, one InstancedStaticMesh: old id -42, old id 7, old id 0 (empty).
    std::string V26Scene()
    {
        return std::string( "{" ) + V26Header( kSceneGuid ) +
               R"(,"SceneName":"S","Entities":[
        {"id":1,"Tag":"Rock","StaticMesh":{"MeshPath":"m","MaterialGuids":[-42]}},
        {"id":2,"Tag":"Trooper","SkinnedMesh":{"MeshPath":"m","MaterialGuids":[7]}},
        {"id":3,"Tag":"Forest","InstancedStaticMesh":{"MeshPath":"m","MaterialGuids":[0]}}]})";
    }

    // What that scene should look like once already at v27, stating GUID text directly: built from the
    // same two GUIDs so the round trip through AssetGuidFromText/HandleForGuid can be compared against it.
    std::string V27Scene( const std::string& textA, const std::string& textB )
    {
        return std::string( "{" ) +
               V26Header( kSceneGuid ) + // Header states 26 here on purpose: the test
                                         // parses this string only to read its component text back out, never
                                         // through MigrateScene, so the stated schema number plays no part in it.
               R"(,"SceneName":"S","Entities":[
        {"id":1,"Tag":"Rock","StaticMesh":{"MeshPath":"m","MaterialGuids":[")" +
               textA + R"("]}},
        {"id":2,"Tag":"Trooper","SkinnedMesh":{"MeshPath":"m","MaterialGuids":[")" +
               textB + R"("]}},
        {"id":3,"Tag":"Forest","InstancedStaticMesh":{"MeshPath":"m","MaterialGuids":[""]}}]})";
    }

    std::string OneUnknownSlotScene()
    {
        return std::string( "{" ) + V26Header( kSceneGuid ) +
               R"(,"SceneName":"S","Entities":[
        {"id":1,"Tag":"Bad","StaticMesh":{"MeshPath":"m","MaterialGuids":[999]}}]})";
    }

    Migration::SceneSerialized Parse( const std::string& json )
    {
        auto scene = rfl::json::read<Migration::SceneSerialized>( json );
        EXPECT_TRUE( scene ) << "the fixture does not parse: " << json;
        return scene.value();
    }

    std::string SlotText( const Migration::SceneSerialized& scene, size_t entity, const char* component )
    {
        const auto payload = scene.Entities[entity].Components.get( component ).value().to_object().value();
        const auto slots   = payload.get( "MaterialGuids" ).value().to_array().value();
        return slots[0].to_string().value();
    }

    Migration::LegacyMaterialIdMap KnownLegacyIds()
    {
        return Migration::LegacyMaterialIdMap{
             { kOldIdNegative, kGuidA },
             { kOldIdSeven, kGuidB },
        };
    }
} // namespace

TEST( SceneMaterialGuidMigration, StaticSkinnedAndInstancedRaiseOldIdsToGuidText )
{
    auto       scene  = Parse( V26Scene() );
    const auto legacy = KnownLegacyIds();
    const auto report = Migration::MigrateScene( scene, "", "", legacy );

    ASSERT_TRUE( report.Refused.empty() ) << report.Refused;
    ASSERT_TRUE( report.MaterialGuidsRaised );
    EXPECT_EQ( report.MaterialGuids.Rewritten, 2 );
    EXPECT_EQ( report.MaterialGuids.Emptied, 1 );
    EXPECT_TRUE( report.MaterialGuids.UnknownNames.empty() );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ),
               Desert::Core::kSceneVersion );

    const std::string textA = AssetGuidToText( kGuidA );
    const std::string textB = AssetGuidToText( kGuidB );

    EXPECT_EQ( SlotText( scene, 0, "StaticMesh" ), textA );
    EXPECT_EQ( SlotText( scene, 1, "SkinnedMesh" ), textB );
    EXPECT_EQ( SlotText( scene, 2, "InstancedStaticMesh" ), "" );

    // The independently-stated v27 fixture must read back the SAME text, and both must fold to the SAME
    // handle: the whole point of AF7 is that a slot's identity survives whichever side wrote the text.
    const auto v27 = Parse( V27Scene( textA, textB ) );
    EXPECT_EQ( SlotText( v27, 0, "StaticMesh" ), textA );
    EXPECT_EQ( SlotText( v27, 1, "SkinnedMesh" ), textB );

    const auto guidFromMigrated = AssetGuidFromText( SlotText( scene, 0, "StaticMesh" ) );
    const auto guidFromStated   = AssetGuidFromText( SlotText( v27, 0, "StaticMesh" ) );
    ASSERT_TRUE( guidFromMigrated );
    ASSERT_TRUE( guidFromStated );
    EXPECT_EQ( HandleForGuid( guidFromMigrated.GetValue() ), HandleForGuid( guidFromStated.GetValue() ) );
    EXPECT_EQ( HandleForGuid( guidFromMigrated.GetValue() ), HandleForGuid( kGuidA ) );
}

TEST( SceneMaterialGuidMigration, AnIdTheRegisterDoesNotKnowRefusesAndLeavesTheSceneUnstamped )
{
    auto       scene  = Parse( OneUnknownSlotScene() );
    const auto legacy = KnownLegacyIds(); // does not know 999
    const auto report = Migration::MigrateScene( scene, "", "", legacy );

    ASSERT_FALSE( report.Refused.empty() );
    EXPECT_NE( report.Refused.find( std::to_string( kOldIdUnknown ) ), std::string::npos ) << report.Refused;
    EXPECT_NE( report.Refused.find( "Bad" ), std::string::npos ) << report.Refused;

    // Unstamped: the header this scene was parsed with (v26) is exactly the header it still carries.
    ASSERT_TRUE( scene.Header.has_value() );
    EXPECT_EQ( Desert::Assets::StatedVersion( scene.Header, Desert::Assets::kSceneSchemaTag ), 26 );
}

std::string OneUnknownSlotPrefab()
{
    return std::string( "{" ) + V26Header( kSceneGuid ) +
           R"(,"Name":"P","Entities":[
        {"id":1,"Tag":"Bad","StaticMesh":{"MeshPath":"m","MaterialGuids":[999]}}]})";
}

TEST( SceneMaterialGuidMigration, MigratePrefabRefusesTheSameUnknownId )
{
    auto parsed = rfl::json::read<Migration::PrefabData>( OneUnknownSlotPrefab() );
    ASSERT_TRUE( parsed ) << "the prefab fixture does not parse";
    auto prefab = parsed.value();

    const auto legacy  = KnownLegacyIds(); // does not know 999
    const auto outcome = Migration::MigratePrefab( prefab, "", "", legacy );

    ASSERT_FALSE( outcome.Refused.empty() );
    EXPECT_NE( outcome.Refused.find( std::to_string( kOldIdUnknown ) ), std::string::npos ) << outcome.Refused;
    EXPECT_NE( outcome.Refused.find( "Bad" ), std::string::npos ) << outcome.Refused;
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
