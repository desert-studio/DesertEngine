#pragma once

// AI-STATETREE CONTRACT (header only, owner review pending) - docs/contracts/AI-STATETREE.md.
//
// The vocabulary every StateTree header shares (UE: Plugins/Runtime/StateTree/Source/StateTreeModule/Public/
// StateTreeTypes.h). Indices, not pointers: the compiled tree is a flat array (UE FCompactStateTreeState /
// FCompactStateTransition) and an instance names its active states by index into it.

#include <cstdint>
#include <limits>
#include <string>

namespace Desert::AI::StateTree
{
    /// Index of a state in the COMPILED tree (CompiledStateTree::States). UE FStateTreeStateHandle.
    using StateIndex = std::uint16_t;
    /// Index of a node (task / condition / evaluator) in CompiledStateTree::Nodes. UE FStateTreeIndex16.
    using NodeIndex = std::uint16_t;

    inline constexpr StateIndex kInvalidState = std::numeric_limits<StateIndex>::max();
    inline constexpr NodeIndex  kInvalidNode  = std::numeric_limits<NodeIndex>::max();

    /// The deepest active path the executor holds (UE FStateTreeActiveStates::MaxStates = 8).
    inline constexpr std::size_t kMaxActiveDepth = 8;

    /// What a task reports, and what a state, and the whole tree, are in (UE EStateTreeRunStatus).
    enum class RunStatus : std::uint8_t
    {
        Running,
        Stopped,
        Succeeded,
        Failed,
        Unset
    };

    /// UE EStateTreeStateType. Linked = run another state of THIS tree as a subtree (UE Linked);
    /// LinkedAsset = run another StateTree asset (UE LinkedAsset). Subtree = a root only reachable by a link.
    enum class StateType : std::uint8_t
    {
        State,
        Group,
        Linked,
        LinkedAsset,
        Subtree
    };

    /// How a state picks among its children when it is selected (UE EStateTreeStateSelectionBehavior; the
    /// utility / weighted variants of UE 5.5 are not in this contract - see the doc's REMAINDER).
    enum class SelectionBehavior : std::uint8_t
    {
        TryEnterState,             // this state only (a leaf)
        TrySelectChildrenInOrder,  // first child whose enter conditions hold
        TrySelectChildrenAtRandom, // a random child among those whose enter conditions hold
        TryFollowTransitions       // evaluate this state's transitions instead of entering it
    };

    /// When a transition is evaluated (UE EStateTreeTransitionTrigger).
    enum class TransitionTrigger : std::uint8_t
    {
        OnStateCompleted, // any task of the state finished (succeeded or failed)
        OnStateSucceeded,
        OnStateFailed,
        OnTick, // every tick, conditions decide
        OnEvent // an event with the transition's tag arrived this tick
    };

    /// Where a transition goes (UE EStateTreeTransitionType).
    enum class TransitionTarget : std::uint8_t
    {
        GotoState,
        NextState, // next sibling of the source
        Succeeded, // finish the tree as succeeded
        Failed,    // finish the tree as failed
        None       // consume the trigger, change nothing (UE "None": blocks lower-priority transitions)
    };

    /// UE EStateTreeTransitionPriority: on the same tick a higher priority transition wins; equal priorities
    /// are taken from the deepest active state up (UE order).
    enum class TransitionPriority : std::uint8_t
    {
        Low,
        Normal,
        Medium,
        High,
        Critical
    };

    /// How a condition combines with the one before it (UE EStateTreeConditionOperand + indent = brackets).
    enum class ConditionOperand : std::uint8_t
    {
        And,
        Or
    };

    /// An event the tree reacts to (UE FStateTreeEvent: Tag + Payload + Origin). The tag is a dotted name
    /// ("Perception.Sight.Gained"); a transition listening for "Perception.Sight" matches every tag below it
    /// (UE FGameplayTag::MatchesTag). The engine has no GameplayTag type yet (TAGS line) - when it lands, the
    /// string is replaced by it in ONE place, here.
    struct StateTreeEvent
    {
        std::string Tag;
        /// Optional payload: the entity the event is about (the seen actor, the noise instigator); entt::null
        /// encoded as the max value so this header stays free of entt.
        std::uint32_t Subject = std::numeric_limits<std::uint32_t>::max();
        /// Free payload a Luau sender attached (a table serialized by the binding); empty = none.
        std::string Payload;
    };
} // namespace Desert::AI::StateTree
