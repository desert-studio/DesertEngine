#pragma once

// AI-STATETREE CONTRACT (header only, owner review pending) - docs/contracts/AI-STATETREE.md.
//
// TASKS, CONDITIONS AND EVALUATORS WRITTEN IN LUAU (UE: Blueprint UStateTreeTaskBlueprintBase /
// UStateTreeConditionBlueprintBase / UStateTreeEvaluatorBlueprintBase; here a Luau module instead of a
// Blueprint class).
//
// A Luau node is a script file. Its kind is declared by the script, its parameters by the same
// `Properties = { ... }` table a ScriptSlot's script uses (Scripting::ReadScriptSchema), so the Details panel
// edits them with no new widget code:
//
//     --!strict
//     Node = "Task"                                   -- "Task" | "Condition" | "Evaluator"
//     Properties = { Radius = Property.Float(500, { Units = "cm" }) }
//
//     function EnterState(actor, transition)  return Status.Running end
//     function Tick(actor, dt)                return Status.Succeeded end
//     function ExitState(actor, transition)   end
//     -- Condition:  function TestCondition(actor) return true end
//     -- Evaluator:  function TreeStart(actor) end; function Tick(actor, dt) end; function TreeStop(actor) end
//     --             Outputs = { Target = Property.Entity() }   -- read by bindings, written by the script
//
// Globals the binding sets before the top level runs: `Status` (Running / Succeeded / Failed), `tree`
// (tree:sendEvent(tag, payload), tree:activeStates()), `params` (the node's bound parameter values, refreshed
// before every call), `perception` (the actor's AIPerceptionComponent queries). `actor` is a LuauEntityRef,
// so `actor:component("Transform")` works as in every entity script.
//
// ONE SANDBOX PER NODE INSTANCE: each NPC's copy of a Luau node is its own LuauRuntime slot (compile-once cache
// keyed by script and source, so a hundred NPCs compile the module once), so script globals ARE the node's
// instance data - the same rule a ScriptSlot has. Hot reload re-runs the slot (LuauRuntime::Reload); the
// execution then restarts the tree, since a reloaded task's state is gone.
//
// Errors: a missing function the kind needs is a COMPILE error (StateTree::Compile, via ValidateScript);
// a runtime script error or a Tick returning anything but a Status fails the task (RunStatus::Failed), is
// logged with the script's own message and recorded in the trace - never treated as Running.

#include <Engine/AI/StateTree/StateTree.hpp>
#include <Engine/AI/StateTree/StateTreeNodes.hpp>
#include <Engine/Scripting/ScriptSchema.hpp>

#include <Common/Core/ResultStr.hpp>

#include <memory>
#include <string>

namespace Desert::AI::StateTree
{
    /// The functions each kind must define (checked by ValidateScript; optional ones listed in the doc).
    inline constexpr const char* kLuauTaskRequired[]      = { "EnterState" };
    inline constexpr const char* kLuauConditionRequired[] = { "TestCondition" };
    inline constexpr const char* kLuauEvaluatorRequired[] = { "Tick" };

    /// What a Luau node module declares, read without running a tree (the editor's node picker, the compiler).
    struct LuauNodeSchema
    {
        NodeKind                Kind = NodeKind::Task;
        Scripting::ScriptSchema Parameters; // `Properties`
        Scripting::ScriptSchema Outputs;    // Evaluator only: `Outputs`
        bool                    DefinesTick           = false;
        bool                    DefinesExitState      = false;
        bool                    DefinesStateCompleted = false;
    };

    /// Creates Luau nodes for an execution. Implemented by Scripting::ScriptEngine (it owns the one
    /// LuauRuntime of the world, its watchdog, its debugger and hot reload), so AI scripts are debugged and
    /// reloaded exactly like entity scripts; a suite may implement it over a bare LuauRuntime.
    class IStateTreeScriptHost
    {
    public:
        virtual ~IStateTreeScriptHost() = default;

        /// Loads `scriptKey`, checks the `Node` kind against `kind` and the required functions, returns the
        /// schema. Refused with the script's own message.
        virtual Common::ResultStr<LuauNodeSchema> ValidateScript( const std::string& scriptKey,
                                                                  NodeKind           kind ) = 0;

        /// A new sandboxed instance of the module for one NPC. Released when the returned object dies.
        virtual Common::ResultStr<std::unique_ptr<IStateTreeTask>> MakeTask( const std::string& scriptKey,
                                                                             entt::entity       actor ) = 0;
        virtual Common::ResultStr<std::unique_ptr<IStateTreeCondition>>
        MakeCondition( const std::string& scriptKey, entt::entity actor ) = 0;
        virtual Common::ResultStr<std::unique_ptr<IStateTreeEvaluator>>
        MakeEvaluator( const std::string& scriptKey, entt::entity actor ) = 0;
    };
} // namespace Desert::AI::StateTree
