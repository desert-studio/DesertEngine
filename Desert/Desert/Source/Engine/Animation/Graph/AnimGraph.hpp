#pragma once

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Animation/Graph/LayeredBlendPerBone.hpp>

#include <optional>
#include <span>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Data-driven animation state machine ("AnimGraph"): a set of STATES (each playing a named clip) connected by
// TRANSITIONS whose CONDITIONS compare live PARAMETERS. The runtime Evaluator walks it each frame and reports
// which clip should be playing (+ blend duration on a change); the ECS then drives the existing Animator with
// that result. Plain structs so reflect-cpp round-trips the graph to/from JSON with no boilerplate (see
// AnimGraphSerialization.cpp); enums are stored as int for stable, tolerant serialization.
namespace Desert::Animation::Graph
{
    /// The extension a saved graph is named by (`.danimgraph`). BESIDE THE DATA AND NOT BESIDE THE ASSET
    /// WRAPPER, on the terms `kControlRigExtension` states: the scene migrator has to spell this to name
    /// the files it writes and links no asset layer at all, and a second spelling of an extension is a
    /// slot that silently refuses a valid file.
    ///
    /// WHY NOT `.degraph`, which is the engine's usual `de` prefix: the shader graph is `.dgraph`, and
    /// `.degraph` differs from it by ONE LETTER in the middle of a word — a file list nobody can read at a
    /// glance and a `switch` whose wrong arm looks right. This spelling carries the word `anim`, so the two
    /// can never be confused by a human or by a grep. Constants.hpp makes the same argument in a
    /// static_assert about `.stmesh` / `.skmesh`.
    inline constexpr std::string_view kAnimGraphExtension = ".danimgraph";

    enum class ParamType : int
    {
        Bool  = 0,
        Int   = 1,
        Float = 2,
    };

    /// The declared type's name, for the refusals that have to say which two types disagreed. A message
    /// that says "the graph declares it as 2" is a message nobody can read.
    [[nodiscard]] const char* TypeName( ParamType type );

    // Comparison of a parameter against a constant. IsTrue/IsFalse treat the parameter as a bool (!= 0).
    enum class CompareOp : int
    {
        Greater      = 0,
        Less         = 1,
        GreaterEqual = 2,
        LessEqual    = 3,
        Equals       = 4,
        NotEquals    = 5,
        IsTrue       = 6,
        IsFalse      = 7,
    };

    struct Parameter
    {
        std::string Name;
        int         Type    = static_cast<int>( ParamType::Float );
        float       Default = 0.0f; // bool encoded as 0/1, int as a whole number
    };

    struct Condition
    {
        std::string Parameter;
        int         Op    = static_cast<int>( CompareOp::Greater );
        float       Value = 0.0f;
    };

    struct Transition
    {
        std::string            To;                 // target state name
        float                  Blend       = 0.2f; // cross-fade seconds when this transition fires
        bool                   HasExitTime = false;
        float                  ExitTime    = 1.0f; // require the source clip to reach this fraction [0,1] first
        std::vector<Condition> Conditions;         // ALL must hold (logical AND); empty + exit-time = auto-advance
    };

    struct State
    {
        std::string             Name;
        std::string             Clip; // clip name, resolved against the AnimationLibrary at runtime
        bool                    Loop  = true;
        float                   Speed = 1.0f;
        float                   X     = 0.0f; // editor canvas position (persisted, unused at runtime)
        float                   Y     = 0.0f;
        std::vector<Transition> Transitions;
    };

    /// A state machine: the payload of a StateMachine pose node (UE: FAnimNode_StateMachine). Its states
    /// play clips; its output pose is the running state's.
    struct StateMachine
    {
        std::string        Entry; // entry state name (defaults to the first state if empty)
        std::vector<State> States;
    };

    /// What a pose node IS. Append only (stored as int). Each kind states its pins in `PinsOf`.
    enum class PoseNodeKind : int
    {
        StateMachine        = 0,
        LayeredBlendPerBone = 1, ///< UE FAnimNode_LayeredBoneBlend: a base pose and one pose per layer
        SequencePlayer      = 2, ///< UE FAnimNode_SequencePlayer: one clip on its own clock, no Pose input
        ApplyAdditive       = 3, ///< UE FAnimNode_ApplyAdditive: Base + Alpha x (Additive - reference pose)
    };

    /// The Apply Additive node's pins: pin 0 Base, pin 1 Additive, and the `Alpha` parameter pin (unbound: 1).
    inline constexpr std::string_view kApplyAdditiveAlphaPin = "Alpha";

    /// The payload of a SequencePlayer node: which clip it plays (by name, resolved against the
    /// AnimationLibrary at runtime, as a State's clip is) and how.
    struct SequencePlayerNode
    {
        std::string Clip;
        bool        Loop = true;
    };

    [[nodiscard]] const char* KindName( PoseNodeKind kind );

    /// A kind's pins: how many Pose inputs it takes, and the names of its parameter pins (inputs a graph
    /// parameter drives, UE's exposed pins such as a blend's weight).
    struct PoseNodePins
    {
        int                          PoseInputs = 0;
        std::span<const char* const> ParameterPins;
    };
    [[nodiscard]] PoseNodePins PinsOf( PoseNodeKind kind );

    struct PoseNode;

    /// The weight pin of a Layered Blend Per Bone node's layer `layer` (UE's `BlendWeights_N`). Unbound, the
    /// layer's weight is 1 — UE's default — so a layer is on until a parameter says otherwise.
    [[nodiscard]] std::string LayerWeightPin( size_t layer );

    /// How many Pose pins `node` has: its kind's fixed count, and for a Layered Blend Per Bone the base pin
    /// plus one per layer of its payload — the pins grow with the layers, as UE's do.
    [[nodiscard]] int PoseInputCountOf( const PoseNode& node );

    /// Whether `node` has the parameter pin `pin`: its kind's fixed pins, and a Layered Blend Per Bone's
    /// weight pin of each of its layers.
    [[nodiscard]] bool HasParameterPin( const PoseNode& node, std::string_view pin );

    /// One parameter pin of a node bound to a declared graph parameter.
    struct ParameterPin
    {
        std::string Pin;       // one of PinsOf(kind).ParameterPins
        std::string Parameter; // a name in AnimGraph::Parameters
    };

    /// A node of the pose graph (UE: an FAnimNode_Base in an AnimGraph).
    struct PoseNode
    {
        std::string                 Name; // unique in the graph; wires name nodes by it
        int                         Kind = static_cast<int>( PoseNodeKind::StateMachine );
        std::vector<std::string>    PoseInputs;      // the node wired into each Pose pin, in pin order
        std::vector<ParameterPin>   ParameterInputs; // bound parameter pins
        std::optional<StateMachine> Machine;         // the payload, present exactly when Kind == StateMachine
        /// The payload, present exactly when Kind == LayeredBlendPerBone. Pose pin 0 is the base, pin i+1
        /// layer i's pose; LayerWeightPin(i) its weight.
        std::optional<LayeredBlendPerBoneNode> LayeredBlend;
        std::optional<SequencePlayerNode>      Sequence; // the payload, present exactly when Kind == SequencePlayer
        float                       X = 0.0f;        // node editor canvas position (persisted, unused at runtime)
        float                       Y = 0.0f;
    };

    struct AnimGraph
    {
        /// The text asset header (T7d, ANGR 1), FIRST so the registry reads it without parsing the rest:
        /// Kind "AnimGraph", the GUID that IS the graph's identity and its handle (AnimGraphAsset's
        /// constructor), and the format under `ANGR`. Carried on the graph so an editor save keeps the GUID
        /// it was loaded with; absent only on a graph never written - Serialize mints it then.
        std::optional<Common::Content::TextAssetHeaderSerialized> Header;
        std::string                                               Name = "AnimGraph";
        std::vector<Parameter> Parameters;
        /// The pose graph (UE's AnimGraph of an AnimBlueprint): nodes whose Pose pins are wired to other
        /// nodes by NAME, evaluated from `OutputPose` back through the wires. A state machine is ONE node
        /// of it, never the whole graph (ANGR 2; an ANGR 1 file was a lone machine and became
        /// `MakeStateMachineGraph`'s shape in the files).
        std::vector<PoseNode> Nodes;
        /// The node wired into the Output Pose sink. Every graph has one: a graph with nothing at its
        /// output is refused by `PlanPoseGraph`, which is what the loader runs.
        std::string OutputPose;
    };

    /// The state machine node's name in a graph `MakeStateMachineGraph` built — and the name every ANGR 1
    /// file's machine was given by the migration.
    inline constexpr std::string_view kDefaultStateMachineNode = "StateMachine";

    /// A graph of ONE state machine node wired to Output Pose, with no states yet: the shape a new graph
    /// starts from and the shape the ANGR 1 files were migrated to.
    [[nodiscard]] AnimGraph MakeStateMachineGraph( std::string name = "AnimGraph" );

    /// The node called `name`, or nullptr.
    [[nodiscard]] const PoseNode* FindNode( const AnimGraph& graph, std::string_view name );
    [[nodiscard]] PoseNode*       FindNode( AnimGraph& graph, std::string_view name );

    /// The SOURCE node (a StateMachine or a SequencePlayer) at the bottom of Output Pose's base chain: the
    /// output node when it is a source, else down pin 0 (the base pin of a Layered Blend Per Bone and of an
    /// Apply Additive) until one is reached. nullptr when the chain ends in no source. Its pose is the one
    /// the Animator's Source stage plays — with the crossfade, notifies, curves and root motion — and the
    /// pose graph reads it as that node's pose.
    [[nodiscard]] const PoseNode* BaseSourceNode( const AnimGraph& graph );

    /// `BaseSourceNode`'s machine, or nullptr when that node is not a state machine. The machine the state
    /// machine editor edits until the node editor lets one pick a node.
    [[nodiscard]] const StateMachine* OutputMachine( const AnimGraph& graph );
    [[nodiscard]] StateMachine*       OutputMachine( AnimGraph& graph );

    /// Whether `kind` produces a pose from clips with no Pose input (UE: a leaf of the AnimGraph).
    [[nodiscard]] bool IsSourceKind( PoseNodeKind kind );

    /**
     * @brief The evaluation order of the pose graph: node indices, every node AFTER the nodes wired into
     *        its Pose pins, the output node last (UE: the output pose's Update/Evaluate recursion, laid
     *        flat). Only nodes that reach Output Pose are in it — an unwired node is not evaluated.
     *
     * Refuses, naming what is wrong: a graph with no Output Pose or one naming a missing node; two nodes
     * of one name (wires are by name, so a duplicate makes a wire mean two things); a Pose pin wired to a
     * missing node; a node wired to more or fewer Pose pins than its kind has; a parameter pin its kind
     * does not have or bound to an undeclared parameter; a kind's payload missing; and a CYCLE, spelled
     * as the loop of node names ("A -> B -> A"), because a pose graph with a loop has no first node.
     */
    [[nodiscard]] Common::ResultStr<std::vector<int>> PlanPoseGraph( const AnimGraph& graph );

    /// The graph's declared parameters as a readable list ("'Speed' (Float), 'Armed' (Bool)"), or
    /// "none at all". ONE spelling, because both refusals that need it — the evaluator's and the Lua
    /// binding's — are the same sentence to the same reader, and two copies of a message drift.
    [[nodiscard]] std::string DeclaredParameterList( const AnimGraph& graph );

    // JSON round-trip (reflect-cpp). Serialize never fails and stamps the header (the GUID kept, or minted for
    // a new graph). Deserialize refuses a file with no header (generation 0, before T7d) by name, pointing at
    // Tools/SceneMigrator, and a header of another kind or version; otherwise an error string on bad JSON.
    std::string                  Serialize( const AnimGraph& graph );
    Common::ResultStr<AnimGraph> Deserialize( const std::string& json );

    // Runtime state-machine evaluator: owns a copy of the graph + live parameter values + the current state.
    // Parameters are all held as float (bool = 0/1, int as a whole number) for a single uniform store.
    class Evaluator
    {
    public:
        explicit Evaluator( AnimGraph graph );

        void Reset(); // jump to the entry state and seed parameters to their defaults

        // Replaces the graph in place while PRESERVING the running state (matched by name) and live parameter
        // values (new parameters get their defaults). Lets the editor edit a live graph without the state
        // machine snapping back to entry. Falls back to entry only if the active state was removed/renamed.
        void SyncGraph( AnimGraph graph );

        /**
         * @brief Set a DECLARED parameter, refusing a name the graph does not have and a type it disagrees
         *        with. Three functions rather than one, because the CALLER knows what it is holding and the
         *        graph knows what it declared — and the whole point is to make the two disagree out loud.
         *
         * THEY USED TO BE `void` AND `m_Params[name] = value`, which created the parameter on the spot. A
         * typo therefore produced a parameter that existed, held the value, was read by nothing, and
         * reported nothing: the empty successful answer the contract forbids (§1.4), in the subsystem that
         * already had one — a crossfade that played nothing. It could not be reached from outside the
         * editor panel before T3.1, which is the only reason it never cost anybody a day.
         *
         * The refusal NAMES the parameter and lists the ones that exist, because "no such parameter" with
         * no list is a message an artist cannot act on without opening the graph.
         */
        [[nodiscard]] Common::BoolResultStr SetBool( const std::string& name, bool value );
        [[nodiscard]] Common::BoolResultStr SetInt( const std::string& name, int value );
        [[nodiscard]] Common::BoolResultStr SetFloat( const std::string& name, float value );

        /// The live value of a declared parameter, or its declared default, or 0 for a name that does not
        /// exist. STILL TOLERANT, and deliberately so: its caller is `EvaluateCondition`, which runs for
        /// every condition of every candidate transition every frame and has no channel to refuse on. What
        /// closes that hole instead is `GetStructureError()` — the graph is checked ONCE, where a report
        /// can be read, rather than sixty times a second where it cannot.
        [[nodiscard]] float GetFloat( const std::string& name ) const;

        /**
         * @brief Empty when every condition in the graph names a parameter the graph declares; otherwise
         *        what is wrong, once, in one string.
         *
         * THE MIRROR OF THE SETTERS ABOVE, and it was the half nobody could see. A condition on a
         * misspelled parameter reads 0.0 through the tolerant `GetFloat` and compares against it happily:
         * `Speed > 0.5` on a parameter called `Sped` is permanently false, the transition never fires, the
         * character stands in its entry state, and there is not one line anywhere to say why. Recorded at
         * construction and after `SyncGraph` rather than discovered at use — the same shape
         * `Skeleton::GetStructureError` uses for malformed parent links.
         */
        [[nodiscard]] const std::string& GetStructureError() const
        {
            return m_StructureError;
        }

        struct Result
        {
            const State* Current = nullptr; // current state after this tick (null only if the graph has no states)
            bool         Changed = false;   // a transition fired this tick
            float        Blend   = 0.0f;    // cross-fade seconds for the change (valid when Changed)
        };

        /**
         * @brief Advances one tick: walks the pose graph in `PlanPoseGraph` order, each node's Update
         *        after the nodes wired into it (UE: FAnimNode_Base::Update_AnyThread, from the output
         *        back). A state machine node fires at most one transition per tick (the first eligible
         *        one, in list order). An unplannable graph updates nothing: its refusal is in
         *        `GetStructureError()`.
         * @param normalizedTime The playback fraction [0,1] of the clip Output Pose shows (exit-time
         *        gating; 0 if unknown).
         * @return What the state machine wired into Output Pose did this tick.
         */
        Result Update( float normalizedTime );

        [[nodiscard]] const AnimGraph& Graph() const
        {
            return m_Graph;
        }
        /// The running state of the machine wired into Output Pose (nullptr when it has no states).
        [[nodiscard]] const State* CurrentState() const;
        /// The running state of the state machine node `node`, or nullptr (no such machine, no states).
        [[nodiscard]] const State* CurrentState( std::string_view node ) const;

        /// Состояние, из которого пришёл последний сработавший переход машины на выходе, или nullptr
        /// до первого. Нужно, чтобы показать переход как «откуда → куда», а не только «куда».
        [[nodiscard]] const State* PreviousState() const;

    private:
        /// A state machine node's running state: indices into its machine's States.
        struct MachineRun
        {
            int Current  = -1;
            int Previous = -1; ///< см. PreviousState(): пишется там же, где срабатывает переход
        };

        [[nodiscard]] bool EvaluateCondition( const Condition& c ) const;

        /// Plans the graph and fills m_StructureError: the plan's refusal, or every condition whose
        /// parameter the graph does not declare.
        void CheckStructure();

        /// Enters every state machine node's entry state (entry, or the first state if it names none).
        void EnterMachines();

        /// One tick of the state machine node at `node`.
        Result UpdateMachine( int node, float normalizedTime );

        [[nodiscard]] const State* StateOf( int node, int state ) const;

        /// The declared parameter, or nullptr. The one place that answers "does the graph have this".
        [[nodiscard]] const Parameter* FindParameter( const std::string& name ) const;

        /// The refusal shared by all three setters: names the parameter and lists what the graph declares.
        [[nodiscard]] Common::BoolResultStr RefuseUnknown( const std::string& name ) const;

        AnimGraph                              m_Graph;
        std::unordered_map<std::string, float> m_Params;
        std::vector<int>                       m_Plan; ///< PlanPoseGraph's order; empty when refused
        std::vector<MachineRun>                m_Runs; ///< one per node, parallel to m_Graph.Nodes
        int m_Output = -1; ///< the machine whose pose is Output Pose's base (OutputMachine), when planned
        std::string                            m_StructureError;
    };
} // namespace Desert::Animation::Graph
