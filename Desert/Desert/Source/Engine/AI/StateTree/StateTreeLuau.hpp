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
//     function InstanceData()                       return { elapsed = 0 } end   -- per-NPC state, optional
//     function EnterState(self, ctx, transition)    return Status.Running end
//     function Tick(self, ctx)                      self.elapsed += ctx.dt; return Status.Succeeded end
//     function ExitState(self, ctx, transition)     end
//     -- Condition:  function TestCondition(self, ctx) return true end
//     -- Evaluator:  function TreeStart(self, ctx) end; function Tick(self, ctx) end; function TreeStop(self, ctx)
//     end
//     --             Outputs = { Target = Property.Entity() }   -- read by bindings, written into self.outputs
//
// `self` is THIS NPC's instance table (what InstanceData() returned; an empty table when the module defines
// none). `ctx` is the call's NodeContext: ctx.controller and ctx.pawn (LuauEntityRef of the AI controller and
// its possessed pawn, lead decision 10-22 Q4 - `ctx.pawn:component("Transform")` works as in every entity
// script; ctx.pawn is nil while nothing is possessed), ctx.dt, ctx.params (the node's bound parameter values
// for this NPC), ctx.events, ctx.tree (sendEvent(tag, payload), activeStates()), ctx.perception (the
// controller's AIPerceptionComponent queries). The module's only globals are `Status` and `Property`; there is
// no `actor` global.
//
// ONE LUAU ENVIRONMENT PER SCRIPT CLASS (lead decision 10-22 Q6, UE: one Blueprint class, per-actor instance
// data): the module runs once in one LuauRuntime slot per script key, and that slot backs the ONE shared node
// of every tree node using the script. Per-NPC state lives only in `self`, never in module globals - a module
// global written from a node function is shared by every NPC, so ValidateScript refuses a module whose
// functions assign globals (Luau --!strict + the host's global-write check). Hot reload re-runs the class slot
// (LuauRuntime::Reload), the AI system rebuilds the node sets that use it and restarts their trees, since the
// instance tables of the old class are gone.
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
        bool                    DefinesInstanceData   = false; // `InstanceData()`; else `self` starts as {}
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

        /// The SHARED node of the script class `scriptKey` (StateTreeNodeSet::Build calls it once per tree
        /// node; the class environment itself is one per script key). Its MakeInstanceData calls the module's
        /// InstanceData() and wraps the returned table's reference in an INodeInstanceData; the slot is
        /// released when the last node of the class dies.
        virtual Common::ResultStr<std::unique_ptr<IStateTreeTask>> MakeTask( const std::string& scriptKey ) = 0;
        virtual Common::ResultStr<std::unique_ptr<IStateTreeCondition>>
        MakeCondition( const std::string& scriptKey ) = 0;
        virtual Common::ResultStr<std::unique_ptr<IStateTreeEvaluator>>
        MakeEvaluator( const std::string& scriptKey ) = 0;
    };
} // namespace Desert::AI::StateTree
