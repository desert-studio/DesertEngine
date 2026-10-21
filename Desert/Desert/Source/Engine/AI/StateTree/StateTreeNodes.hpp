#pragma once

// AI-STATETREE CONTRACT (header only, owner review pending) - docs/contracts/AI-STATETREE.md.
//
// THE THREE NODE KINDS (UE FStateTreeTaskBase StateTreeTaskBase.h:19, FStateTreeConditionBase
// StateTreeConditionBase.h:21, FStateTreeEvaluatorBase StateTreeEvaluatorBase.h:18) as interfaces with two
// implementations each: a native C++ type (registered here) and a Luau module (StateTreeLuau.hpp). The executor
// sees only these interfaces, so a tree mixes both freely.
//
// INSTANCE STATE: a node object is created PER TREE INSTANCE (one per NPC), so its members are its instance
// data (UE keeps the node const and puts instance data in FStateTreeInstanceData - the split buys UE memory
// layout for Mass; this engine runs a tree per NPC entity and the per-instance object is the simpler, honest
// shape; see the doc's "where we differ").

#include <Engine/AI/StateTree/StateTreeTypes.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::AI::StateTree
{
    class StateTreeExecution;

    /// What a node may read and do while it runs (UE FStateTreeExecutionContext as a node sees it). Valid only
    /// for the duration of the call it is passed to.
    struct NodeContext
    {
        Core::Scene*        Scene        = nullptr;
        entt::registry*     Registry     = nullptr;
        entt::entity        Actor        = entt::null; // the NPC the tree runs on
        StateTreeExecution* Execution    = nullptr;
        float               DeltaSeconds = 0.0f;

        /// The node's parameters with bindings already copied in (UE: instance data after property copies).
        std::span<const Scripting::ScriptProperty> Parameters;

        /// The events that arrived since the previous tick (UE FStateTreeExecutionContext::GetEventsToProcess).
        std::span<const StateTreeEvent> Events;
    };

    /// Why a state is being entered or left (UE FStateTreeTransitionResult as a task sees it).
    struct TransitionInfo
    {
        RunStatus  CurrentRunStatus = RunStatus::Running;
        StateIndex SourceState      = kInvalidState;
        StateIndex TargetState      = kInvalidState;
        /// UE EStateTreeStateChangeType: Changed = this state is entered/left; Sustained = it stays active
        /// while a child changes (a task that only cares about its own state ignores Sustained calls; UE
        /// bShouldStateChangeOnReselect decides whether reselecting the same state is Changed).
        bool Sustained = false;
    };

    class IStateTreeTask
    {
    public:
        virtual ~IStateTreeTask() = default;
        /// UE EnterState. Running keeps the state active; Succeeded / Failed complete it immediately.
        virtual RunStatus EnterState( NodeContext& context, const TransitionInfo& transition ) = 0;
        /// UE Tick; called only while the state is active and the task returned Running.
        virtual RunStatus Tick( NodeContext& context ) = 0;
        /// UE ExitState: always called for every task that was entered, whatever it returned.
        virtual void ExitState( NodeContext& context, const TransitionInfo& transition ) = 0;
        /// UE StateCompleted: a task of this or a child state completed (before the transition is taken).
        virtual void StateCompleted( NodeContext& /*context*/, RunStatus /*completed*/ )
        {
        }
        /// UE bShouldCallTick = false: an event-driven task (waits for a callback) is not ticked.
        [[nodiscard]] virtual bool WantsTick() const
        {
            return true;
        }
    };

    class IStateTreeCondition
    {
    public:
        virtual ~IStateTreeCondition() = default;
        /// UE TestCondition. Pure: no side effects, may be evaluated any number of times per tick.
        [[nodiscard]] virtual bool TestCondition( NodeContext& context ) const = 0;
    };

    class IStateTreeEvaluator
    {
    public:
        virtual ~IStateTreeEvaluator()                 = default;
        virtual void TreeStart( NodeContext& context ) = 0;
        /// Before transitions are evaluated, every tick. Writes its outputs (UE Output properties), which
        /// bindings copy into the parameters of the nodes that read them.
        virtual void Tick( NodeContext& context )     = 0;
        virtual void TreeStop( NodeContext& context ) = 0;
        /// The outputs bindings may name ("<evaluatorId>.<Name>"), current values.
        [[nodiscard]] virtual std::span<const Scripting::ScriptProperty> Outputs() const = 0;
    };

    /// The native node types by name (UE: USTRUCT reflection finds them). A type declares its parameters
    /// (name, type, default) so the editor and the compiler see them without instantiating it.
    class NodeRegistry
    {
    public:
        struct Entry
        {
            std::string                                           Name; // "Wait", "MoveTo", "IsTargetVisible", ...
            NodeKind                                              Kind;
            std::vector<Scripting::ScriptProperty>                Parameters; // declared, with defaults
            std::string                                           Tooltip;
            std::function<std::unique_ptr<IStateTreeTask>()>      MakeTask;      // Kind == Task
            std::function<std::unique_ptr<IStateTreeCondition>()> MakeCondition; // Kind == Condition
            std::function<std::unique_ptr<IStateTreeEvaluator>()> MakeEvaluator; // Kind == Evaluator
        };

        static NodeRegistry& Get();

        /// Refused when the name is taken (a second registration would silently replace a node in every tree).
        Common::BoolResultStr                Register( Entry entry );
        [[nodiscard]] const Entry*           Find( std::string_view name ) const;
        [[nodiscard]] std::span<const Entry> All() const;

    private:
        std::vector<Entry> m_Entries;
    };

    /// The native nodes this engine ships with (AI-ST-EXEC: Wait, Succeed, Fail, SendEvent, RunTime /
    /// DebugText; AI-TASK-MOVE: MoveTo, LookAt, PlayMontage; AI-PERC-*: HasPerceivedActor, TargetDistance,
    /// PerceptionEvaluator). Called once at engine start; a duplicate name is a startup error.
    Common::BoolResultStr RegisterBuiltinNodes( NodeRegistry& registry );
} // namespace Desert::AI::StateTree
