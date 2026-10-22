#pragma once

// AI-STATETREE CONTRACT (header only, owner review pending) - docs/contracts/AI-STATETREE.md.
//
// THE AUTHORED TREE AND ITS COMPILED FORM (UE UStateTree + UStateTreeEditorData + FStateTreeCompiler).
//
// Two shapes, one source of truth: `StateTreeDesc` is what the editor edits and the `.dstatetree` file stores
// (a tree of states, nested); `CompiledStateTree` is what the executor walks (flat arrays, children as index
// ranges, transitions and nodes as index ranges - UE FCompactStateTreeState). The compiled form is DERIVED on
// load and on every edit, never stored, so the two cannot disagree (UE stores the compact form in the asset and
// keeps the editor data beside it; here the file holds only the authored tree).
//
// Node parameters are ScriptValues: one value type for a native node and a Luau node alike, the same one the
// Details panel already edits for a ScriptSlot (Scripting/ScriptProperty.hpp).

#include <Engine/AI/StateTree/StateTreeTypes.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::AI::StateTree
{
    inline constexpr std::string_view kStateTreeExtension = ".dstatetree";

    /// Which implementation a node has (see StateTreeNodes.hpp). UE has only C++ (and Blueprint) nodes; the
    /// Luau kind is this engine's gameplay-script path (decision 10-09: game logic in scripts).
    enum class NodeSource : std::uint8_t
    {
        Native, // a C++ type registered in NodeRegistry under `Type`
        Luau    // a Luau module; `Type` is its script key ("assets:AI/Tasks/Patrol.luau")
    };

    /// One node as authored: its implementation, its parameter values, and the bindings that feed parameters
    /// from elsewhere (UE FStateTreeEditorNode + FStateTreePropertyPathBinding).
    struct NodeDesc
    {
        NodeKind      Kind   = NodeKind::Task;
        NodeSource    Source = NodeSource::Native;
        std::string   Type;   // registered native type name, or the Luau script key
        std::string   Name;   // display name in the tree; empty = the type's own
        std::uint64_t Id = 0; // stable id (bindings and the PIE debugger name a node by it, not by position)

        /// Overrides of the parameters the node declares (native: its reflected fields; Luau: its
        /// `Properties = { ... }` table, read by Scripting::ReadScriptSchema). A parameter not listed keeps
        /// its declared default.
        std::vector<Scripting::ScriptProperty> Parameters;

        /// Condition only: how it combines with the previous condition of the same list, and its bracket depth
        /// (UE ExpressionIndent). Condition only: invert the result.
        ConditionOperand Operand = ConditionOperand::And;
        std::uint8_t     Indent  = 0;
        bool             Invert  = false;
    };

    /// A parameter of a node fed from another value instead of its literal (UE FStateTreePropertyPathBinding).
    /// Sources: a tree parameter ("Tree.<Name>"), an evaluator output ("<evaluatorId>.<Output>"), or the
    /// context ("Context.Controller", "Context.Pawn", "Context.Perception" - the AI controller that owns the
    /// tree, the pawn it possesses, the controller's perception; lead decision 10-22 Q4). Resolved by the compiler
    /// to indices; a binding whose source or target does not exist, or whose types differ, is a compile ERROR (not
    /// a warning).
    struct PropertyBinding
    {
        std::uint64_t TargetNode = 0;
        std::string   TargetParameter;
        std::string   SourcePath;
    };

    struct TransitionDesc
    {
        TransitionTrigger Trigger = TransitionTrigger::OnStateCompleted;
        /// OnEvent only: the registered tag listened for (parent-aware, GameplayTag::MatchesTag). The file stores
        /// the tag's name; ReadStateTree resolves it through GameplayTagRegistry::RequestTag and refuses a name
        /// nobody registered (lead decision 10-22 Q5: never a free string).
        GameplayTags::GameplayTag EventTag;

        TransitionTarget      Target       = TransitionTarget::GotoState;
        std::uint64_t         TargetState  = 0; // GotoState only: StateDesc::Id
        TransitionPriority    Priority     = TransitionPriority::Normal;
        float                 DelaySeconds = 0.0f; // UE bDelayTransition / DelayDuration
        std::vector<NodeDesc> Conditions;          // all NodeKind::Condition
    };

    struct StateDesc
    {
        std::uint64_t     Id = 0; // stable across edits and saves
        std::string       Name;
        StateType         Type      = StateType::State;
        SelectionBehavior Selection = SelectionBehavior::TrySelectChildrenInOrder;
        bool              Enabled   = true;

        std::uint64_t LinkedState = 0; // Type == Linked: the Subtree state run here
        std::string   LinkedAsset;     // Type == LinkedAsset: the tree asset's key

        std::vector<NodeDesc>       EnterConditions; // all NodeKind::Condition
        std::vector<NodeDesc>       Tasks;           // all NodeKind::Task; run together, UE "all tasks"
        std::vector<TransitionDesc> Transitions;
        std::vector<StateDesc>      Children;
    };

    /// The whole authored tree (the `.dstatetree` file).
    struct StateTreeDesc
    {
        std::string Name;
        /// The context this tree runs in (UE Schema, UStateTreeAIComponentSchema): "AI" = an AI controller that
        /// possesses a pawn, with the controller's perception and the pawn's movement. Only one
        /// schema exists in this contract; the field is there so a node can refuse a tree it cannot run in.
        std::string Schema = "AI";
        /// Tree parameters, overridable per instance by StateTreeComponent (UE FStateTreeParameters).
        std::vector<Scripting::ScriptProperty> Parameters;
        std::vector<NodeDesc>        Evaluators;  // all NodeKind::Evaluator; tick before transitions, tree-global
        std::vector<NodeDesc>        GlobalTasks; // UE 5.2 global tasks: run for the tree's whole life
        std::vector<StateDesc>       Roots;
        std::vector<PropertyBinding> Bindings;
    };

    // ------------------------------------------------------------------------------------------------------
    // Compiled form - what the executor walks. Index ranges are [Begin, Begin + Count).

    struct CompactNode
    {
        NodeKind         Kind;
        NodeSource       Source;
        std::uint64_t    Id;
        std::uint16_t    TypeIndex;      // into CompiledStateTree::NodeTypes (native name / script key)
        std::uint16_t    ParameterBegin; // into CompiledStateTree::Parameters
        std::uint16_t    ParameterCount;
        ConditionOperand Operand;
        std::uint8_t     Indent;
        bool             Invert;
    };

    struct CompactTransition
    {
        TransitionTrigger  Trigger;
        TransitionTarget   Target;
        TransitionPriority Priority;
        StateIndex         TargetState;   // GotoState / NextState resolved
        GameplayTags::GameplayTag EventTag;      // OnEvent only; the empty tag otherwise
        float              DelaySeconds;
        NodeIndex          ConditionBegin;
        std::uint8_t       ConditionCount;
    };

    struct CompactState
    {
        std::uint64_t     Id;
        StateIndex        Parent; // kInvalidState for a root
        StateIndex        ChildBegin;
        std::uint16_t     ChildCount;
        StateType         Type;
        SelectionBehavior Selection;
        StateIndex        LinkedState;      // Linked
        std::uint16_t     LinkedAssetIndex; // LinkedAsset: into CompiledStateTree::LinkedAssets
        NodeIndex         EnterConditionBegin;
        std::uint8_t      EnterConditionCount;
        NodeIndex         TaskBegin;
        std::uint8_t      TaskCount;
        std::uint16_t     TransitionBegin;
        std::uint8_t      TransitionCount;
    };

    /// A resolved PropertyBinding: copy `Source` into parameter `TargetParameter` of `TargetNode` before the
    /// node runs (UE FStateTreePropertyCopy batches).
    struct CompactBinding
    {
        NodeIndex     TargetNode;
        std::uint16_t TargetParameter; // into CompiledStateTree::Parameters
        std::uint16_t SourceIndex;     // into CompiledStateTree::BindingSources
    };

    struct CompiledStateTree
    {
        std::vector<CompactState>              States;
        std::vector<CompactTransition>         Transitions;
        std::vector<CompactNode>               Nodes;
        std::vector<CompactBinding>            Bindings;
        std::vector<Scripting::ScriptProperty> Parameters; // node parameter values, defaults applied
        std::vector<Scripting::ScriptProperty> TreeParameters;
        std::vector<std::string>               NodeTypes;
        std::vector<std::string>               LinkedAssets;
        std::vector<std::string>               BindingSources;
        NodeIndex                              EvaluatorBegin  = 0;
        std::uint16_t                          EvaluatorCount  = 0;
        NodeIndex                              GlobalTaskBegin = 0;
        std::uint16_t                          GlobalTaskCount = 0;
        StateIndex                             RootBegin       = 0;
        std::uint16_t                          RootCount       = 0;
    };

    /// A problem the compiler found (cf. Animation::Graph::GraphWarning). An Error makes Compile fail; a
    /// Warning compiles (an unreachable state, a disabled state with transitions into it).
    struct CompileMessage
    {
        enum class Severity : std::uint8_t
        {
            Warning,
            Error
        };
        Severity      Level = Severity::Error;
        std::uint64_t State = 0; // the state it is about; 0 = the tree
        std::uint64_t Node  = 0; // the node it is about; 0 = the state
        std::string   Text;
    };

    struct CompileResult
    {
        CompiledStateTree           Tree;
        std::vector<CompileMessage> Messages;
    };

    /// UE FStateTreeCompiler::Compile. Errors (any is fatal, all are collected): a transition into a state
    /// that does not exist, a Linked state naming a non-Subtree, an unknown native type, a Luau script that
    /// does not load or does not define the functions its kind needs (StateTreeLuau.hpp), a binding that does
    /// not resolve or whose types differ, more than kMaxActiveDepth levels, a node kind in the wrong list,
    /// more states / nodes than the index types hold. The error result carries every message joined; the
    /// success result carries the warnings.
    [[nodiscard]] Common::ResultStr<CompileResult> Compile( const StateTreeDesc& tree );

    /// The `.dstatetree` text format (YAML, as every authored asset). Read refuses a malformed file with the
    /// reason; Write is the inverse; Read(Write(x)) == x.
    [[nodiscard]] Common::ResultStr<StateTreeDesc> ReadStateTree( std::string_view text );
    [[nodiscard]] std::string                      WriteStateTree( const StateTreeDesc& tree );
} // namespace Desert::AI::StateTree
