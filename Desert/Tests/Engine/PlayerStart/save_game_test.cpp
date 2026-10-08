// GP5: save games - a slot holds the player pawn's transform and the PROPERTY(SaveGame) fields of every entity, by
// its UUID, and nothing else (UE: USaveGame + UPROPERTY(SaveGame) + UGameplayStatics' slot functions).
#include <Engine/Core/SaveGame.hpp>
#include <Engine/ECS/Components.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Desert;
using namespace Desert::Core;

namespace
{
    // PointLightData as the registry would describe it, with Intensity marked SaveGame and Color NOT marked. A
    // lookup of our own, so the test flags a field without touching the process-wide registry.
    Reflection::TypeInfo PointLightType( const char* intensityTypeName )
    {
        Reflection::TypeInfo type;
        type.Name = "PointLightData";
        type.Size = sizeof( ECS::PointLightData );
        Reflection::FieldInfo color;
        color.Name     = "Color";
        color.Type     = Reflection::FieldType::Vec3;
        color.Offset   = offsetof( ECS::PointLightData, Color );
        color.Size     = sizeof( glm::vec3 );
        color.TypeName = "glm::vec3";
        Reflection::FieldInfo intensity;
        intensity.Name          = "Intensity";
        intensity.Type          = Reflection::FieldType::Float;
        intensity.Offset        = offsetof( ECS::PointLightData, Intensity );
        intensity.Size          = sizeof( float );
        intensity.TypeName      = intensityTypeName;
        intensity.Meta.SaveGame = true;
        type.Fields             = { color, intensity };
        return type;
    }

    ReflectedTypeLookup LookupWith( const Reflection::TypeInfo& pointLight )
    {
        return [&pointLight]( const std::string& name ) -> const Reflection::TypeInfo*
        { return name == "PointLightData" ? &pointLight : nullptr; };
    }

    const SaveGameSceneIdentity kArena{ "11111111-2222-3333-4444-555555555555", "Arena" };

    entt::entity MakeLamp( entt::registry& registry, std::uint64_t uuid, const char* name, float intensity )
    {
        const entt::entity lamp                           = registry.create();
        registry.emplace<ECS::UUIDComponent>( lamp ).UUID = Common::UUID( uuid );
        registry.emplace<ECS::TagComponent>( lamp ).Tag   = name;
        registry.emplace<ECS::TransformComponent>( lamp );
        auto& light          = registry.emplace<ECS::PointLightComponent>( lamp );
        light.Data.Intensity = intensity;
        light.Data.Color     = glm::vec3( 0.25f );
        return lamp;
    }

    entt::entity MakePawn( entt::registry& registry, std::uint64_t uuid, const glm::vec3& at )
    {
        const entt::entity pawn                                       = registry.create();
        registry.emplace<ECS::UUIDComponent>( pawn ).UUID             = Common::UUID( uuid );
        registry.emplace<ECS::TransformComponent>( pawn ).Translation = at;
        return pawn;
    }

    bool AnyProblemMentions( const SaveGameLoadReport& report, const std::string& text )
    {
        for ( const std::string& problem : report.Problems )
            if ( problem.find( text ) != std::string::npos )
                return true;
        return false;
    }
} // namespace

TEST( SaveGame, RoundTripRestoresFlaggedFieldsOnly )
{
    const auto     type  = PointLightType( "float" );
    const auto     types = LookupWith( type );
    entt::registry registry;
    const auto     lamp     = MakeLamp( registry, 101u, "Lamp", 7.5f );
    const auto     document = CaptureSaveGame( registry, entt::null, kArena, types );

    auto& light        = registry.get<ECS::PointLightComponent>( lamp ).Data;
    light.Intensity    = 1.0f;              // progress lost -> must come back
    light.Color        = glm::vec3( 0.9f ); // level data edited -> must be left alone
    light.Radius       = 333.0f;
    const auto applied = ApplySaveGame( registry, entt::null, kArena, document, types );
    ASSERT_TRUE( applied ) << applied.GetError();
    EXPECT_TRUE( applied.GetValue().Problems.empty() );
    EXPECT_EQ( applied.GetValue().AppliedFields, 1u );
    EXPECT_FLOAT_EQ( light.Intensity, 7.5f );
    EXPECT_EQ( light.Color, glm::vec3( 0.9f ) ) << "an unflagged field was written by a load";
    EXPECT_FLOAT_EQ( light.Radius, 333.0f ) << "an unflagged field was written by a load";
}

TEST( SaveGame, PawnTransformIsRestored )
{
    const auto     type = PointLightType( "float" );
    entt::registry registry;
    const auto     pawn     = MakePawn( registry, 900u, glm::vec3( 100.0f, 0.0f, -250.0f ) );
    const auto     document = CaptureSaveGame( registry, pawn, kArena, LookupWith( type ) );

    // Play spawns a NEW pawn (fresh UUID) next session: the pawn record is not keyed by UUID.
    entt::registry next;
    const auto     respawned = MakePawn( next, 901u, glm::vec3( 0.0f ) );
    const auto     applied   = ApplySaveGame( next, respawned, kArena, document, LookupWith( type ) );
    ASSERT_TRUE( applied ) << applied.GetError();
    EXPECT_TRUE( applied.GetValue().PawnRestored );
    EXPECT_EQ( next.get<ECS::TransformComponent>( respawned ).Translation, glm::vec3( 100.0f, 0.0f, -250.0f ) );
}

TEST( SaveGame, MissingEntityIsReportedByNameAndTheRestStillLoads )
{
    const auto     type  = PointLightType( "float" );
    const auto     types = LookupWith( type );
    entt::registry registry;
    const auto     kept     = MakeLamp( registry, 101u, "KeptLamp", 4.0f );
    const auto     gone     = MakeLamp( registry, 102u, "BrokenLamp", 6.0f );
    const auto     document = CaptureSaveGame( registry, entt::null, kArena, types );
    registry.destroy( gone );
    registry.get<ECS::PointLightComponent>( kept ).Data.Intensity = 0.0f;

    const auto applied = ApplySaveGame( registry, entt::null, kArena, document, types );
    ASSERT_TRUE( applied ) << applied.GetError();
    EXPECT_TRUE( AnyProblemMentions( applied.GetValue(), "'BrokenLamp'" ) );
    EXPECT_FLOAT_EQ( registry.get<ECS::PointLightComponent>( kept ).Data.Intensity, 4.0f );
}

TEST( SaveGame, FieldWhoseTypeChangedIsReportedAndSkipped )
{
    const auto     saved = PointLightType( "float" );
    entt::registry registry;
    const auto     lamp     = MakeLamp( registry, 101u, "Lamp", 9.0f );
    const auto     document = CaptureSaveGame( registry, entt::null, kArena, LookupWith( saved ) );
    registry.get<ECS::PointLightComponent>( lamp ).Data.Intensity = 2.0f;

    const auto now     = PointLightType( "double" );
    const auto applied = ApplySaveGame( registry, entt::null, kArena, document, LookupWith( now ) );
    ASSERT_TRUE( applied ) << applied.GetError();
    EXPECT_TRUE( AnyProblemMentions( applied.GetValue(), "changed type from 'float' to 'double'" ) );
    EXPECT_FLOAT_EQ( registry.get<ECS::PointLightComponent>( lamp ).Data.Intensity, 2.0f );
}

TEST( SaveGame, SlotOfAnotherSceneIsRefusedNamingBoth )
{
    const auto                  type = PointLightType( "float" );
    entt::registry              registry;
    const auto                  document = CaptureSaveGame( registry, entt::null, kArena, LookupWith( type ) );
    const SaveGameSceneIdentity cave{ "other-guid", "Cave" };
    const auto applied = ApplySaveGame( registry, entt::null, cave, document, LookupWith( type ) );
    ASSERT_FALSE( applied );
    EXPECT_NE( applied.GetError().find( "'Arena'" ), std::string::npos ) << applied.GetError();
    EXPECT_NE( applied.GetError().find( "'Cave'" ), std::string::npos ) << applied.GetError();
}

TEST( SaveGame, HalfWrittenTempNeverReplacesTheSlot )
{
    TestSupport::ScratchDir scratch( "savegame-atomic" );
    const auto              type = PointLightType( "float" );
    entt::registry          registry;
    MakeLamp( registry, 101u, "Lamp", 5.0f );
    const auto document = CaptureSaveGame( registry, entt::null, kArena, LookupWith( type ) );
    ASSERT_TRUE( WriteSaveGameSlot( scratch.Path(), "Slot1", 0, document ) );

    // A crash in the middle of the next save: the temporary holds half a document.
    const auto path = SaveGameSlotPath( scratch.Path(), "Slot1", 0 );
    ASSERT_TRUE( path );
    {
        std::ofstream half( path.GetValue().string() + ".tmp", std::ios::binary );
        half << "{\"Format\": \"DesertSaveGame\", \"Vers";
    }
    const auto read = ReadSaveGameSlot( scratch.Path(), "Slot1", 0 );
    ASSERT_TRUE( read ) << read.GetError();
    EXPECT_TRUE( Common::Json::Same( read.GetValue(), document ) );
    const auto slots = ListSaveGameSlots( scratch.Path(), 0 );
    ASSERT_TRUE( slots );
    EXPECT_EQ( slots.GetValue(), std::vector<std::string>{ "Slot1" } ) << "a .desave.tmp was listed as a slot";

    // The next save goes through the temporary again and replaces the slot whole.
    ASSERT_TRUE( WriteSaveGameSlot( scratch.Path(), "Slot1", 0, document ) );
    EXPECT_TRUE( ReadSaveGameSlot( scratch.Path(), "Slot1", 0 ) );
}

TEST( SaveGame, OtherVersionIsRefusedByName )
{
    TestSupport::ScratchDir scratch( "savegame-version" );
    const auto              path = SaveGameSlotPath( scratch.Path(), "Old", 0 );
    ASSERT_TRUE( path );
    {
        std::ofstream file( path.GetValue(), std::ios::binary );
        file << "{\"Format\": \"DesertSaveGame\", \"Version\": 999, \"Scene\": {\"Guid\": \"\", \"Name\": \"\"}}";
    }
    const auto read = ReadSaveGameSlot( scratch.Path(), "Old", 0 );
    ASSERT_FALSE( read );
    EXPECT_NE( read.GetError().find( "Old.desave" ), std::string::npos ) << read.GetError();
    EXPECT_NE( read.GetError().find( "version 999" ), std::string::npos ) << read.GetError();

    {
        std::ofstream file( path.GetValue(), std::ios::binary );
        file << "{\"Format\": \"SomethingElse\", \"Version\": 1}";
    }
    const auto foreign = ReadSaveGameSlot( scratch.Path(), "Old", 0 );
    ASSERT_FALSE( foreign );
    EXPECT_NE( foreign.GetError().find( "not a save game" ), std::string::npos ) << foreign.GetError();
}

TEST( SaveGame, SlotsAreListedPerUserAndDeleted )
{
    TestSupport::ScratchDir scratch( "savegame-slots" );
    entt::registry          registry;
    const auto              type     = PointLightType( "float" );
    const auto              document = CaptureSaveGame( registry, entt::null, kArena, LookupWith( type ) );
    ASSERT_TRUE( WriteSaveGameSlot( scratch.Path(), "b", 0, document ) );
    ASSERT_TRUE( WriteSaveGameSlot( scratch.Path(), "a", 0, document ) );
    ASSERT_TRUE( WriteSaveGameSlot( scratch.Path(), "c", 1, document ) );

    EXPECT_EQ( ListSaveGameSlots( scratch.Path(), 0 ).GetValue(), ( std::vector<std::string>{ "a", "b" } ) );
    EXPECT_EQ( ListSaveGameSlots( scratch.Path(), 1 ).GetValue(), std::vector<std::string>{ "c" } );
    EXPECT_TRUE( DoesSaveGameExist( scratch.Path(), "a", 0 ) );
    EXPECT_FALSE( DoesSaveGameExist( scratch.Path(), "c", 0 ) );

    ASSERT_TRUE( DeleteGameInSlot( scratch.Path(), "a", 0 ) );
    EXPECT_FALSE( DoesSaveGameExist( scratch.Path(), "a", 0 ) );
    const auto again = DeleteGameInSlot( scratch.Path(), "a", 0 );
    ASSERT_FALSE( again );
    EXPECT_NE( again.GetError().find( "'a'" ), std::string::npos ) << again.GetError();
    EXPECT_EQ( ListSaveGameSlots( scratch.Path(), 0 ).GetValue(), std::vector<std::string>{ "b" } );

    EXPECT_FALSE( SaveGameSlotPath( scratch.Path(), "../escape", 0 ) );
    EXPECT_FALSE( SaveGameSlotPath( scratch.Path(), "", 0 ) );
}
