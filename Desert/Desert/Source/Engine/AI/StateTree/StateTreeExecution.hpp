#pragma once

// AI-STATETREE CONTRACT (header only, owner review pending) - docs/contracts/AI-STATETREE.md.
//
// ONE RUNNING TREE (UE FStateTreeExecutionContext StateTreeExecutionContext.h:329 + FStateTreeInstanceData).
// The compiled tree AND its node objects are SHARED (lead decision 10-22 Q6, UE: the UStateTree asset holds the
// node structs, FStateTreeInstanceData holds the per-actor part): one StateTreeNodeSet per compiled tree, built
// once and used by every NPC running it. An execution owns only what is per-NPC: the nodes' INSTANCE DATA
// (INodeInstanceData, StateTreeNodes.hpp) and bound parameter values, the active path, the pending events, the
// delayed transitions.
//
// ONE TICK, in UE's order:
//   1. evaluators Tick, global tasks Tick;
//   2. tasks of the active path Tick, root to leaf (a task that is not WantsTick is skipped);
//   3. transitions: from the leaf up, triggers matched against the tick's completion and events, conditions
//      tested; the highest-priority passing transition is taken (ties: deepest state first);
//   4. taking it: ExitState on the states being left (leaf first), selection from the target down
//      (enter conditions, SelectionBehavior), EnterState on the states being entered (root first). A task
//      that completes in EnterState re-runs step 3 in the same tick, bounded by kMaxTransitionsPerTick (UE
//      MaxIterations) - exceeding it is an error the tree stops on, never a silent hang.
//   5. events are consumed.
// Scheduled tick (UE 5.4 bScheduledTick): ShouldTick() is false while no task wants a tick, no transition is
// OnTick or delayed, and no event is pending - the AI system skips the tree that frame.

#include <Engine/AI/StateTree/StateTree.hpp>
#include <Engine/AI/StateTree/StateTreeNodes.hpp>

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace Desert::AI::StateTree
{
    class IStateTreeScriptHost;

    inline constexpr int kMaxTransitionsPerTick = 8;

    /// A record of what the executor did (UE FStateTreeTraceEvent / the debugger's trace): the PIE debugger
    /// (AI-DEBUG) and the execution suite (AI-ST-EXEC: Enter/Exit log against a reference) read it.
    struct ExecutionTraceEntry
    {
        enum class Kind : std::uint8_t
        {
            TreeStart,
            TreeStop,
            EnterState,
            ExitState,
            TaskStatus,
            TransitionTaken,
            ConditionFailed,
            Event
        };
        Kind          What;
        StateIndex    State  = kInvalidState;
        NodeIndex     Node   = kInvalidNode;
        RunStatus     Status = RunStatus::Unset;
        std::uint32_t Tick   = 0;
    };

    /// The SHARED node objects of one compiled tree (lead decision 10-22 Q6): one IStateTreeNode per CompactNode,
    /// in node order, made once per (compiled tree, asset revision) and shared by every execution of that tree.
    /// Node methods are const, so the set is immutable after Build and safe to use from every NPC's tick. The
    /// AI system keeps one per tree asset (AISystem.hpp batching) and rebuilds it on asset revision change or on
    /// Luau hot reload of a script class the tree uses.
    class StateTreeNodeSet
    {
    public:
        /// Makes every node: native ones from `registry` (unknown name = refused), Luau ones through `scripts`
        /// (one shared node per script class, StateTreeLuau.hpp; required when the tree has any, refused
        /// otherwise). Refused with the first failing node's reason; a partial set is never returned.
        static Common::ResultStr<std::shared_ptr<const StateTreeNodeSet>>
        Build( std::shared_ptr<const CompiledStateTree> tree, const NodeRegistry& registry,
               IStateTreeScriptHost* scripts );

        [[nodiscard]] const CompiledStateTree& Tree() const;
        [[nodiscard]] const IStateTreeNode&    Node( NodeIndex node ) const;
        [[nodiscard]] std::size_t              NodeCount() const;

    private:
        StateTreeNodeSet() = default;

        std::shared_ptr<const CompiledStateTree>     m_Tree;
        std::vector<std::unique_ptr<IStateTreeNode>> m_Nodes; // index = NodeIndex
    };

    struct ExecutionSetup
    {
        /// The shared nodes (and through them the compiled tree) this NPC runs.
        std::shared_ptr<const StateTreeNodeSet> Nodes;
        /// Per-instance tree parameter overrides (StateTreeComponent::Parameters); unknown names are refused.
        std::span<const Scripting::ScriptProperty> Parameters;
        Core::Scene*                               Scene    = nullptr;
        entt::registry*                            Registry = nullptr;
        /// The AI controller entity that owns the tree (Q4) and the pawn it possesses (entt::null while it
        /// possesses nothing); copied into every NodeContext.
        entt::entity Controller = entt::null;
        entt::entity Pawn       = entt::null;
        bool         Trace      = false; // record ExecutionTraceEntry (editor PIE, suites)
    };

    class StateTreeExecution
    {
    public:
        StateTreeExecution() = default;
        ~StateTreeExecution();
        StateTreeExecution( const StateTreeExecution& )            = delete;
        StateTreeExecution& operator=( const StateTreeExecution& ) = delete;
        StateTreeExecution( StateTreeExecution&& ) noexcept;
        StateTreeExecution& operator=( StateTreeExecution&& ) noexcept;

        /// UE Start: makes this NPC's instance data of every node (IStateTreeNode::MakeInstanceData), TreeStart on
        /// evaluators and global tasks, selects from the roots and enters the selected path. Refused (with the
        /// reason) when an instance cannot be made, a parameter override names nothing, or no root can be
        /// selected. The node objects are never created here (StateTreeNodeSet::Build).
        Common::BoolResultStr Start( const ExecutionSetup& setup );

        /// UE Tick. Returns the tree's status: Running, or Succeeded / Failed once a transition finished it
        /// (the tree is then stopped and Start may run it again).
        Common::ResultStr<RunStatus> Tick( float deltaSeconds );

        /// UE SendEvent: queued, processed by the next Tick (and makes ShouldTick true).
        void SendEvent( StateTreeEvent event );

        /// UE Stop: ExitState on the active path, TreeStop on evaluators and global tasks, instance data released
        /// (the shared node set stays with the AI system).
        void Stop();

        [[nodiscard]] RunStatus GetStatus() const;
        [[nodiscard]] bool      ShouldTick() const;
        /// The active path, root first (UE GetActiveStateNames as indices).
        [[nodiscard]] std::span<const StateIndex>          ActiveStates() const;
        [[nodiscard]] const CompiledStateTree*             GetTree() const;
        [[nodiscard]] std::span<const ExecutionTraceEntry> Trace() const;
        void                                               ClearTrace();

    private:
        struct DelayedTransition;

        std::shared_ptr<const StateTreeNodeSet> m_Nodes;
        /// Per node (index = NodeIndex): this NPC's instance data; null where the node keeps none.
        std::vector<std::unique_ptr<INodeInstanceData>> m_Instances;
        /// Per node: its parameters with this NPC's overrides and bindings copied in (NodeContext::Parameters).
        std::vector<std::vector<Scripting::ScriptProperty>> m_BoundParameters;
        std::array<StateIndex, kMaxActiveDepth>  m_Active{};
        std::uint8_t                             m_ActiveDepth = 0;
        std::vector<RunStatus>                   m_TaskStatus; // per node, for the active tasks
        std::vector<StateTreeEvent>              m_Events;
        std::vector<DelayedTransition>           m_Delayed;
        std::vector<ExecutionTraceEntry>         m_Trace;
        RunStatus                                m_Status       = RunStatus::Stopped;
        std::uint32_t                            m_TickIndex    = 0;
        bool                                     m_TraceEnabled = false;
    };
} // namespace Desert::AI::StateTree
