#pragma once

// AI-STATETREE CONTRACT (header only, owner review pending) - docs/contracts/AI-STATETREE.md.
//
// THE TREE ON AN NPC (UE UStateTreeComponent StateTreeComponent.h:39 / UStateTreeAIComponent
// StateTreeAIComponent.h:16). The scene stores the asset and the per-instance parameter overrides; the running
// execution is transient (rebuilt on Play, on asset revision change, on Luau hot reload).
//
// UE puts the AI tree on the AIController, which possesses a pawn. This engine has no Controller / Pawn type
// yet (GameMode possesses the player's pawn only), so the tree lives on the NPC entity itself; when the
// Controller line lands, the component moves to the controller entity and NodeContext::Actor becomes the
// possessed pawn - one field, see the doc's open questions.

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

        PROPERTY( DisplayName( "State Tree" ), Category( "AI" ), Summary,
                  Tooltip( "The .dstatetree asset this entity runs (UE UStateTreeComponent StateTreeRef)." ) )
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
    };
} // namespace Desert::ECS
