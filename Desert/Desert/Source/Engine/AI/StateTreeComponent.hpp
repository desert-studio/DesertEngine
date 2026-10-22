#pragma once

// AI-STATETREE CONTRACT (header only, owner review pending) - docs/contracts/AI-STATETREE.md.
//
// THE TREE ON AN NPC (UE UStateTreeComponent StateTreeComponent.h:39 / UStateTreeAIComponent
// StateTreeAIComponent.h:16). The scene stores the asset and the per-instance parameter overrides; the running
// execution is transient (rebuilt on Play, on asset revision change, on Luau hot reload).
//
// ON THE AI CONTROLLER (lead decision 10-22 Q4, UE UStateTreeAIComponent on AAIController): this component
// sits on the controller entity, next to AIControllerComponent (Engine/AI/AIController.hpp, the AI-CONTROLLER
// card) and AIPerceptionComponent; the NPC's body is the pawn the controller possesses. Nodes read both
// (NodeContext::Controller / ::Pawn). A tree on an entity without AIControllerComponent is refused at Start
// (RuntimeError names it), so there is no second place a tree can live. The tree survives the pawn: when the
// pawn dies or is unpossessed the tree keeps running with Pawn = entt::null (UE: the controller outlives it)
// and gets the event "AI.Controller.Unpossessed"; possession of a new pawn sends "AI.Controller.Possessed".

// AI-CONTROLLER adds #include <Engine/AI/AIController.hpp> (AIControllerComponent); the header is not on disk yet
#include <Engine/AI/StateTree/StateTreeExecution.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Desert::ECS
{
    struct StateTreeData
    {
        REFLECT()

        PROPERTY(
             DisplayName( "State Tree" ), Category( "AI" ), Summary,
             Tooltip( "The .dstatetree asset this AI controller runs (UE UStateTreeComponent StateTreeRef)." ) )
        Assets::AssetHandle StateTree;

        PROPERTY( DisplayName( "Start On Play" ), Category( "AI" ),
                  Tooltip( "UE bStartLogicAutomatically: start the tree when Play begins. Off = a script or "
                           "the GameMode starts it (ai.startTree(entity))." ) )
        bool StartOnPlay = true;

        PROPERTY( DisplayName( "Restart On Finish" ), Category( "AI" ),
                  Tooltip( "When the tree finishes (Succeeded / Failed), start it again on the next tick. "
                           "Off = it stays stopped (UE default)." ) )
        bool RestartOnFinish = false;
    };

    struct StateTreeComponent
    {
        // GATED until AI-ST-ASSET/AI-ST-EXEC (the header tool skips commented markers; ReflectedComponentBlocks.gen.hpp would include this
        // header and the transient members need the execution's out-of-line destructor): COMPONENT( Key( "StateTree" ), Block( Data ), Run( ActorsAndUI ) )
        StateTreeData Data;

        /// Overrides of the tree's parameters (UE FStateTreeReference parameters), edited in Details like a
        /// ScriptSlot's Properties and stored with the entity (AI-ST-ASSET adds them to the block: a reflected
        /// vector of ScriptProperty is not yet a reflected field type).
        std::vector<Scripting::ScriptProperty> Parameters;

        // ---- transient (not serialized) ----
        std::unique_ptr<AI::StateTree::StateTreeExecution> Execution;
        std::uint32_t AssetRevision = 0; // the StateTreeAsset revision Execution was started at
        std::string   RuntimeError;      // the last Start / Tick failure, shown in Details while playing
        float SinceLastTick = 0.0f; // tick LOD (AISystem): accumulated delta handed to the next Tick
        float NextTickIn    = 0.0f; // seconds until this controller's LOD band ticks it again
    };
} // namespace Desert::ECS
