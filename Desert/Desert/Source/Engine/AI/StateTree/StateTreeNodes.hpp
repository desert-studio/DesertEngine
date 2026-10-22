#pragma once

// AI-STATETREE CONTRACT (header only, owner review pending) - docs/contracts/AI-STATETREE.md.
//
// THE THREE NODE KINDS (UE FStateTreeTaskBase StateTreeTaskBase.h:19, FStateTreeConditionBase
// StateTreeConditionBase.h:21, FStateTreeEvaluatorBase StateTreeEvaluatorBase.h:18) as interfaces with two
// implementations each: a native C++ type (registered here) and a Luau module (StateTreeLuau.hpp). The executor
// sees only these interfaces, so a tree mixes both freely.
//
// NODE vs INSTANCE DATA (UE's split, lead decision 10-22 Q6): a node object is SHARED - one per node of a
// compiled tree, used by every NPC running that tree (StateTreeNodeSet, StateTreeExecution.hpp) - and its
// methods are const. What changes per NPC lives in the node's INSTANCE DATA (UE FStateTreeInstanceData item),
// one object per (NPC, node), made by MakeInstanceData and handed back in NodeContext::InstanceData. A native
// node declares its instance struct; a Luau node's instance data is the table its class returns (one Luau
// environment per class, StateTreeLuau.hpp). So 1000 NPCs on one tree hold 1000 small instance objects, not
// 1000 copies of every node.

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

    /// The per-NPC state of one node (UE instance data). A native node derives its instance struct from it; the
    /// Luau host derives one that holds the instance table's reference. Destroyed when the NPC's execution stops.
    class INodeInstanceData
    {
    public:
        virtual ~INodeInstanceData() = default;
    };

    /// What a node may read and do while it runs (UE FStateTreeExecutionContext as a node sees it). Valid only
    /// for the duration of the call it is passed to.
    struct NodeContext
    {
        Core::Scene*    Scene    = nullptr;
        entt::registry* Registry = nullptr;
        /// The AI controller entity that owns the tree (lead decision 10-22 Q4, UE AAIController owning
        /// UStateTreeAIComponent) and the pawn it possesses (UE GetPawn()); Pawn is entt::null while the
        /// controller possesses nothing.
        entt::entity        Controller   = entt::null;
        entt::entity        Pawn         = entt::null;
        StateTreeExecution* Execution    = nullptr;
        float               DeltaSeconds = 0.0f; // the time since this tree's previous tick (tick LOD, AISystem)

        /// This node's instance data for this NPC; null when the node declares none.
        INodeInstanceData* InstanceData = nullptr;

        /// The node's parameters with bindings already copied in (UE: instance data after property copies).
        std::span<const Scripting::ScriptProperty> Parameters;

        /// The events that arrived since the previous tick (UE FStateTreeExecutionContext::GetEventsToProcess).
        std::span<const StateTreeEvent> Events;

        template <class T>
        [[nodiscard]] T& Instance() const
        {
            return static_cast<T&>( *InstanceData );
        }
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

    /// What every node kind shares: making its per-NPC instance data.
    class IStateTreeNode
    {
    public:
        virtual ~IStateTreeNode() = default;
        /// UE GetInstanceDataType + instantiation: a fresh instance for the NPC whose controller is given.
        /// Null = the node keeps no per-NPC state. Refused (Luau: the class's InstanceData() raised) with the
        /// reason, which fails the tree's Start.
        [[nodiscard]] virtual Common::ResultStr<std::unique_ptr<INodeInstanceData>>
        MakeInstanceData( entt::entity controller ) const = 0;
    };

    class IStateTreeTask : public IStateTreeNode
    {
    public:
        /// UE EnterState. Running keeps the state active; Succeeded / Failed complete it immediately.
        virtual RunStatus EnterState( NodeContext& context, const TransitionInfo& transition ) const = 0;
        /// UE Tick; called only while the state is active and the task returned Running.
        virtual RunStatus Tick( NodeContext& context ) const = 0;
        /// UE ExitState: always called for every task that was entered, whatever it returned.
        virtual void ExitState( NodeContext& context, const TransitionInfo& transition ) const = 0;
        /// UE StateCompleted: a task of this or a child state completed (before the transition is taken).
        virtual void StateCompleted( NodeContext& /*context*/, RunStatus /*completed*/ ) const
        {
        }
        /// UE bShouldCallTick = false: an event-driven task (waits for a callback) is not ticked.
        [[nodiscard]] virtual bool WantsTick() const
        {
            return true;
        }
    };

    class IStateTreeCondition : public IStateTreeNode
    {
    public:
        /// UE TestCondition. Pure: no side effects, may be evaluated any number of times per tick.
        [[nodiscard]] virtual bool TestCondition( NodeContext& context ) const = 0;
    };

    class IStateTreeEvaluator : public IStateTreeNode
    {
    public:
        virtual void TreeStart( NodeContext& context ) const = 0;
        /// Before transitions are evaluated, every tick. Writes its outputs (UE Output properties) into its
        /// instance data; bindings copy them into the parameters of the nodes that read them.
        virtual void Tick( NodeContext& context ) const     = 0;
        virtual void TreeStop( NodeContext& context ) const = 0;
        /// The outputs bindings may name ("<evaluatorId>.<Name>"), current values of one NPC's instance.
        [[nodiscard]] virtual std::span<const Scripting::ScriptProperty>
        Outputs( const INodeInstanceData* instance ) const = 0;
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
            /// Make the SHARED node object (one per node of a compiled tree, StateTreeNodeSet::Build).
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
    /// SmartObject nodes (find, claim, use, release a slot) are NOT registered here: SmartObjects are their own
    /// system (lead decision 10-22 Q3, UE SmartObjectsModule + GameplayStateTreeModule) and register their
    /// nodes into this registry from that module; the tree only queries and claims through them.
    Common::BoolResultStr RegisterBuiltinNodes( NodeRegistry& registry );
} // namespace Desert::AI::StateTree
