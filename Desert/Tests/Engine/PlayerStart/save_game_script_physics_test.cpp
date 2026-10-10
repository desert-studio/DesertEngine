// GP5b: save games reach the physics world (a restored pawn is teleported through its body / character, not
// only its TransformComponent), Lua script properties marked SaveGame are saved per entity UUID + script +
// property, and a test-only reflected struct carries PROPERTY(SaveGame)'s flag through capture and apply.
#include <Engine/Core/SaveGame.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>

#include "../../TestSupport/scratch_dir.hpp"
#include "../PhysicsFixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

using namespace Desert;
using namespace Desert::Core;

namespace
{
    const SaveGameSceneIdentity kArena{ "11111111-2222-3333-4444-555555555555", "Arena" };

    SaveGameSchema NoFlaggedFields( std::vector<std::string> scriptSaveGame )
    {
        return SaveGameSchema{
             []( const std::string& ) -> const Reflection::TypeInfo* { return nullptr; },
             [scriptSaveGame]( const std::string& ) -> Common::ResultStr<std::vector<std::string>>
             { return Common::MakeSuccess( scriptSaveGame ); } };
    }

    entt::entity MakePawn( entt::registry& registry, const glm::vec3& at )
    {
        const entt::entity pawn                                       = registry.create();
        registry.emplace<ECS::UUIDComponent>( pawn ).UUID             = Common::UUID( 7001 );
        registry.emplace<ECS::TransformComponent>( pawn ).Translation = at;
        return pawn;
    }

    entt::entity MakeScripted( entt::registry& registry, double score, bool door )
    {
        const entt::entity entity                           = registry.create();
        registry.emplace<ECS::UUIDComponent>( entity ).UUID = Common::UUID( 7002 );
        registry.emplace<ECS::TagComponent>( entity ).Tag   = "Chest";
        registry.emplace<ECS::TransformComponent>( entity );
        ECS::ScriptSlot slot;
        slot.ScriptKey = "Scripts/Chest.lua";
        Scripting::ScriptProperty scoreProperty;
        scoreProperty.Name   = "Score";
        scoreProperty.Number = score;
        Scripting::ScriptProperty doorProperty;
        doorProperty.Name = "DoorOpen";
        doorProperty.Type = Scripting::PropertyType::Bool;
        doorProperty.Bool = door;
        Scripting::ScriptProperty speedProperty; // tuning, not SaveGame
        speedProperty.Name   = "WalkSpeed";
        speedProperty.Number = 300.0;
        slot.Properties      = { scoreProperty, doorProperty, speedProperty };
        registry.emplace<ECS::ScriptComponent>( entity ).Scripts.push_back( slot );
        return entity;
    }

    const Scripting::ScriptProperty& PropertyOf( entt::registry& registry, entt::entity entity, const char* name )
    {
        for ( const auto& p : registry.get<ECS::ScriptComponent>( entity ).Scripts[0].Properties )
            if ( p.Name == name )
                return p;
        static const Scripting::ScriptProperty none;
        return none;
    }

    bool AnyProblemMentions( const SaveGameLoadReport& report, const std::string& text )
    {
        for ( const std::string& problem : report.Problems )
            if ( problem.find( text ) != std::string::npos )
                return true;
        return false;
    }

    // The test-only reflected struct: what a game's own gameplay struct looks like with PROPERTY(SaveGame) on
    // Health and Coins and none on Tuning. Its TypeInfo is the one the header tool generates for it (Metadata.tpl
    // writes `.SaveGame = true` into the field's PropertyMetadata), registered in the process registry under a
    // name no engine type has.
    struct SaveGameWitness
    {
        float Health = 100.0f;
        int   Coins  = 0;
        float Tuning = 1.0f;
    };

    const Reflection::TypeInfo* RegisterWitness()
    {
        Reflection::TypeInfo type;
        type.Name = "SaveGameWitness_GP5b";
        type.Size = sizeof( SaveGameWitness );
        Reflection::FieldInfo health;
        health.Name          = "Health";
        health.Type          = Reflection::FieldType::Float;
        health.Offset        = offsetof( SaveGameWitness, Health );
        health.Size          = sizeof( float );
        health.TypeName      = "float";
        health.Meta.SaveGame = true;
        Reflection::FieldInfo coins;
        coins.Name          = "Coins";
        coins.Type          = Reflection::FieldType::Int;
        coins.Offset        = offsetof( SaveGameWitness, Coins );
        coins.Size          = sizeof( int );
        coins.TypeName      = "int";
        coins.Meta.SaveGame = true;
        Reflection::FieldInfo tuning;
        tuning.Name     = "Tuning";
        tuning.Type     = Reflection::FieldType::Float;
        tuning.Offset   = offsetof( SaveGameWitness, Tuning );
        tuning.Size     = sizeof( float );
        tuning.TypeName = "float";
        type.Fields     = { health, coins, tuning };
        return Reflection::ReflectionRegistry::Get().Register( std::move( type ) );
    }
} // namespace

TEST( SaveGame, RestoredPawnBodyIsTeleportedAndStopped )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( 0.0f, TestSupport::PhysicsTestProfiles() ) ); // no gravity: only the teleport and the old velocity move the body
    Physics::BodyDesc desc;
    desc.Type        = Physics::BodyType::Dynamic;
    desc.HalfExtents = glm::vec3( 50.0f );
    desc.Position    = glm::vec3( 0.0f );
    desc.Profile     = TestSupport::ProfileId( world, "PhysicsActor" );
    const auto body  = world.CreateBody( desc );
    ASSERT_TRUE( body.IsSuccess() ) << body.GetError();

    entt::registry     registry;
    const entt::entity pawn = MakePawn( registry, glm::vec3( 1000.0f, 0, 0 ) );
    registry.emplace<ECS::RigidBodyComponent>( pawn ).RuntimeBody = body.GetValue();
    const auto document = CaptureSaveGame( registry, pawn, kArena, NoFlaggedFields( {} ) );

    registry.get<ECS::TransformComponent>( pawn ).Translation = glm::vec3( 0.0f );
    world.SetLinearVelocity( body.GetValue(), glm::vec3( 0.0f, 0.0f, 500.0f ) );

    const auto applied = ApplySaveGame( registry, pawn, kArena, document, NoFlaggedFields( {} ), &world );
    ASSERT_TRUE( applied.IsSuccess() ) << applied.GetError();
    EXPECT_TRUE( applied.GetValue().Problems.empty() );
    EXPECT_NEAR( world.GetPosition( body.GetValue() ).x, 1000.0f, 0.01f );
    EXPECT_NEAR( glm::length( world.GetLinearVelocity( body.GetValue() ) ), 0.0f, 1e-3f );

    // The next step leaves it where the save put it (not snapped back to the old pose, not carried on).
    world.Step( 1.0f / 60.0f );
    EXPECT_NEAR( glm::distance( world.GetPosition( body.GetValue() ), glm::vec3( 1000.0f, 0, 0 ) ), 0.0f, 0.01f );
    world.Shutdown();
}

TEST( SaveGame, RestoredPawnCharacterIsTeleported )
{
    Physics::PhysicsWorld world;
    ASSERT_TRUE( world.Init( 0.0f, TestSupport::PhysicsTestProfiles() ) );
    Physics::CharacterDesc desc;
    desc.Radius      = 30.0f;
    desc.HalfHeight  = 60.0f;
    desc.Position    = glm::vec3( 0.0f );
    desc.MaxSlopeDeg = 50.0f;
    desc.Profile     = TestSupport::ProfileId( world, "Pawn" );

    entt::registry     registry;
    const entt::entity pawn       = MakePawn( registry, glm::vec3( 0.0f, 0.0f, 2500.0f ) );
    auto&              controller = registry.emplace<ECS::CharacterControllerComponent>( pawn );
    const auto         character  = world.CreateCharacter( desc );
    ASSERT_TRUE( character.IsSuccess() ) << character.GetError();
    controller.RuntimeCharacter = character.GetValue();
    const auto document           = CaptureSaveGame( registry, pawn, kArena, NoFlaggedFields( {} ) );

    registry.get<ECS::TransformComponent>( pawn ).Translation        = glm::vec3( 0.0f );
    registry.get<ECS::CharacterControllerComponent>( pawn ).Velocity = glm::vec3( 0.0f, 0.0f, -900.0f );

    const auto applied = ApplySaveGame( registry, pawn, kArena, document, NoFlaggedFields( {} ), &world );
    ASSERT_TRUE( applied.IsSuccess() ) << applied.GetError();
    const auto& cc = registry.get<ECS::CharacterControllerComponent>( pawn );
    EXPECT_NEAR( glm::distance( world.GetCharacterPosition( cc.RuntimeCharacter ), glm::vec3( 0, 0, 2500.0f ) ),
                 0.0f, 0.01f );
    EXPECT_EQ( glm::length( cc.Velocity ), 0.0f );

    // Without the world, a live character is reported instead of silently left behind.
    registry.get<ECS::TransformComponent>( pawn ).Translation = glm::vec3( 0.0f );
    const auto blind = ApplySaveGame( registry, pawn, kArena, document, NoFlaggedFields( {} ), nullptr );
    ASSERT_TRUE( blind.IsSuccess() );
    EXPECT_TRUE( AnyProblemMentions( blind.GetValue(), "no physics world" ) );
    world.Shutdown();
}

TEST( SaveGame, ScriptSaveGamePropertiesRoundTrip )
{
    entt::registry     registry;
    const entt::entity chest    = MakeScripted( registry, 42.0, true );
    const auto         schema   = NoFlaggedFields( { "Score", "DoorOpen" } );
    const auto         document = CaptureSaveGame( registry, entt::null, kArena, schema );

    // The next session: same entity UUID, the script's defaults, a different tuning value.
    entt::registry     next;
    const entt::entity fresh                                                = MakeScripted( next, 0.0, false );
    next.get<ECS::ScriptComponent>( fresh ).Scripts[0].Properties[2].Number = 450.0;
    const auto applied = ApplySaveGame( next, entt::null, kArena, document, schema, nullptr );
    ASSERT_TRUE( applied.IsSuccess() ) << applied.GetError();
    EXPECT_TRUE( applied.GetValue().Problems.empty() );
    EXPECT_EQ( applied.GetValue().AppliedFields, 2u );
    EXPECT_EQ( PropertyOf( next, fresh, "Score" ).Number, 42.0 );
    EXPECT_TRUE( PropertyOf( next, fresh, "DoorOpen" ).Bool );
    EXPECT_EQ( PropertyOf( next, fresh, "WalkSpeed" ).Number, 450.0 )
         << "a property not marked SaveGame was loaded";
    (void)chest;
}

TEST( SaveGame, RemovedScriptPropertyIsReportedByName )
{
    entt::registry     registry;
    const entt::entity chest = MakeScripted( registry, 42.0, true );
    const auto         document =
         CaptureSaveGame( registry, entt::null, kArena, NoFlaggedFields( { "Score", "DoorOpen" } ) );

    // The script no longer declares DoorOpen SaveGame: Score still loads, DoorOpen is named and skipped.
    registry.get<ECS::ScriptComponent>( chest ).Scripts[0].Properties[0].Number = 0.0;
    const auto applied =
         ApplySaveGame( registry, entt::null, kArena, document, NoFlaggedFields( { "Score" } ), nullptr );
    ASSERT_TRUE( applied.IsSuccess() );
    EXPECT_TRUE( AnyProblemMentions( applied.GetValue(), "property 'DoorOpen'" ) );
    EXPECT_TRUE( AnyProblemMentions( applied.GetValue(), "Scripts/Chest.lua" ) );
    EXPECT_EQ( PropertyOf( registry, chest, "Score" ).Number, 42.0 );
}

TEST( SaveGame, ScriptDeclaresSaveGamePropertiesByName )
{
    TestSupport::ScratchDir scratch( "savegame-script-declaration" );
    const auto              good = scratch.Path() / "Good.lua";
    std::ofstream( good ) << "Properties = { Score = 0, Coins = 0, WalkSpeed = 300 }\n"
                             "SaveGameProperties = { \"Score\", \"Coins\" }\n";
    const auto names = Scripting::ReadScriptSaveGameProperties( good.string() );
    ASSERT_TRUE( names.IsSuccess() ) << names.GetError();
    EXPECT_EQ( names.GetValue(), ( std::vector<std::string>{ "Score", "Coins" } ) );

    const auto none = scratch.Path() / "None.lua";
    std::ofstream( none ) << "Properties = { WalkSpeed = 300 }\n";
    const auto noNames = Scripting::ReadScriptSaveGameProperties( none.string() );
    ASSERT_TRUE( noNames.IsSuccess() );
    EXPECT_TRUE( noNames.GetValue().empty() );

    const auto typo = scratch.Path() / "Typo.lua";
    std::ofstream( typo ) << "Properties = { Score = 0 }\nSaveGameProperties = { \"Scroe\" }\n";
    const auto refused = Scripting::ReadScriptSaveGameProperties( typo.string() );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "Scroe" ), std::string::npos );
}

TEST( SaveGame, TestOnlyReflectedStructCarriesTheSaveGameFlag )
{
    const Reflection::TypeInfo* registered = RegisterWitness();
    const Reflection::TypeInfo* type       = RegistryTypeLookup( "SaveGameWitness_GP5b" );
    ASSERT_EQ( type, registered ) << "the engine's lookup does not find a registered type";

    SaveGameWitness saved;
    saved.Health      = 35.5f;
    saved.Coins       = 12;
    saved.Tuning      = 9.0f;
    const auto fields = CaptureSaveGameFields( *type, &saved );
    EXPECT_EQ( fields.size(), 2u ) << "only Health and Coins carry SaveGame";
    const Common::Json::Value document( fields );

    SaveGameWitness    loaded;
    SaveGameLoadReport report;
    ApplySaveGameFields( *type, &loaded, Common::Json::Root( document ), "witness", report );
    EXPECT_TRUE( report.Problems.empty() );
    EXPECT_EQ( loaded.Health, 35.5f );
    EXPECT_EQ( loaded.Coins, 12 );
    EXPECT_EQ( loaded.Tuning, 1.0f ) << "a field without SaveGame was restored";
}
