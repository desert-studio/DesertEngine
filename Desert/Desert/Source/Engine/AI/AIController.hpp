#pragma once

// DEPENDENCY DECLARED BY THE AI-STATETREE CONTRACT (header only, implemented by the AI-CONTROLLER card) -
// docs/contracts/AI-STATETREE.md section 2.
//
// THE AI CONTROLLER (UE AAIController AIController.h + APawn::AutoPossessAI / AIControllerClass Pawn.h). The
// brain and the body are two entities: the CONTROLLER entity carries AIControllerComponent, StateTreeComponent
// and AIPerceptionComponent (lead decision 10-22 Q4, UE UStateTreeAIComponent / UAIPerceptionComponent on the
// AAIController); the PAWN is the body it possesses (transform, CharacterComponent, collision, animation). The
// controller outlives its pawn: a dead or unpossessed pawn leaves the tree running with Pawn = entt::null.
//
// WHO SPAWNS THE CONTROLLER (UE AutoPossessAI + AIControllerClass): the PAWN is what the level authors. Its
// AIPawnComponent names the controller PREFAB (a .dprefab holding the controller's components - the tree asset,
// the senses, the team; UE: the AIController blueprint class) and when to spawn it. At Play (PlacedInWorld) or
// when a pawn is spawned at run time (Spawned), AIControllerSystem instantiates the prefab into the SAME scene
// and calls Possess. One prefab, many NPCs: the controller's authored data is not copied into every pawn.
//
// POSSESSION (UE AController::Possess / UnPossess, OnPossess / OnUnPossess). Possess sets Pawn and the pawn's
// AIPawnComponent::Controller, unpossessing whatever either held before (a pawn has at most one controller, a
// controller at most one pawn), then sends the StateTree event "AI.Controller.Possessed"; Unpossess clears both
// links and sends "AI.Controller.Unpossessed". Destroying a pawn unpossesses it first (UE APawn::Destroyed ->
// DetachFromControllerPendingDestroy). The player's possession (Core::GameMode, Scene's player pawn) is a
// separate path: a pawn possessed by the player is refused by Possess, naming both entities.
//
// SERVER ONLY (lead decision 10-22 Q7): controllers exist only where AISystem runs; a client never spawns one
// and sees the pawn's replicated result.
//
// Until AI-CONTROLLER lands nothing that includes this header links a call to it; the COMPONENT markers are
// gated as the other AI components' are (ReflectedComponentBlocks.gen.hpp must not include this header before
// AIControllerSystem exists).

#include <Engine/Assets/Common.hpp>
#include <Common/Core/ResultStr.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

#include <entt/entt.hpp>

#include <cstdint>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::ECS
{
    /// UE EAutoPossessAI.
    enum class AutoPossessAI : std::uint8_t
    {
        Disabled,               ///< No controller is spawned; a script or the GameMode possesses the pawn.
        PlacedInWorld,          ///< Spawned at Play for a pawn the level placed (UE default).
        Spawned,                ///< Spawned for a pawn created at run time (prefab instantiation, GameMode).
        PlacedInWorldOrSpawned, ///< Both.
    };

    struct AIPawnData
    {
        REFLECT()

        PROPERTY( DisplayName( "AI Controller" ), Category( "AI" ), Summary, Asset<PrefabAsset>,
                  Tooltip( "The controller prefab spawned to possess this pawn (UE AIControllerClass): it holds "
                           "the State Tree, the perception senses and the team." ) )
        Assets::AssetHandle AIController;

        PROPERTY( DisplayName( "Auto Possess AI" ), Category( "AI" ),
                  Tooltip( "UE AutoPossessAI: when the controller prefab is spawned and possesses this pawn." ) )
        AutoPossessAI AutoPossess = AutoPossessAI::PlacedInWorld;
    };

    /// On the PAWN: what the level authors.
    struct AIPawnComponent
    {
        // GATED until AI-CONTROLLER: COMPONENT( Key( "AIPawn" ), Block( Data ), Run( ActorsAndUI ) )
        AIPawnData Data;

        // ---- transient (not serialized): written by Possess / Unpossess only ----
        entt::entity Controller = entt::null; ///< UE APawn::Controller
    };

    /// On the CONTROLLER entity (spawned from AIPawnData::AIController): never authored by hand in a level.
    struct AIControllerComponent
    {
        // GATED until AI-CONTROLLER: COMPONENT( Key( "AIController" ), Run( ActorsAndUI ) )

        // ---- transient (not serialized): written by Possess / Unpossess only ----
        entt::entity Pawn = entt::null; ///< UE AController::GetPawn
    };
} // namespace Desert::ECS

namespace Desert::AI
{
    /// UE AController::Possess. Refused by name: @p controller without AIControllerComponent, @p pawn without
    /// AIPawnComponent, a pawn the player possesses, a destroyed entity.
    Common::BoolResultStr Possess( Core::Scene& scene, entt::entity controller, entt::entity pawn );

    /// UE AController::UnPossess. A controller with no pawn is left as it is.
    void Unpossess( Core::Scene& scene, entt::entity controller );

    /// The controller possessing @p pawn, or entt::null.
    [[nodiscard]] entt::entity ControllerOf( const Core::Scene& scene, entt::entity pawn );
} // namespace Desert::AI
