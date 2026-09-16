#pragma once

#include <Common/Core/ResultStr.hpp>

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

    struct AnimGraph
    {
        std::string            Name = "AnimGraph";
        std::string            Entry; // entry state name (defaults to the first state if empty)
        std::vector<Parameter> Parameters;
        std::vector<State>     States;
    };

    /// The graph's declared parameters as a readable list ("'Speed' (Float), 'Armed' (Bool)"), or
    /// "none at all". ONE spelling, because both refusals that need it — the evaluator's and the Lua
    /// binding's — are the same sentence to the same reader, and two copies of a message drift.
    [[nodiscard]] std::string DeclaredParameterList( const AnimGraph& graph );

    // JSON round-trip (reflect-cpp). Serialize never fails; Deserialize returns an error string on bad JSON.
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

        // Advances one tick. normalizedTime = the current clip's playback fraction [0,1] (for exit-time gating;
        // pass 0 if unknown). Fires at most one transition per tick (the first eligible one, in list order).
        Result Update( float normalizedTime );

        [[nodiscard]] const AnimGraph& Graph() const
        {
            return m_Graph;
        }
        [[nodiscard]] const State* CurrentState() const;

    private:
        [[nodiscard]] int  FindState( const std::string& name ) const;
        [[nodiscard]] bool EvaluateCondition( const Condition& c ) const;

        /// Fills m_StructureError: every condition whose parameter the graph does not declare.
        void CheckStructure();

        /// The declared parameter, or nullptr. The one place that answers "does the graph have this".
        [[nodiscard]] const Parameter* FindParameter( const std::string& name ) const;

        /// The refusal shared by all three setters: names the parameter and lists what the graph declares.
        [[nodiscard]] Common::BoolResultStr RefuseUnknown( const std::string& name ) const;

        AnimGraph                              m_Graph;
        std::unordered_map<std::string, float> m_Params;
        int                                    m_Current = -1;
        std::string                            m_StructureError;
    };
} // namespace Desert::Animation::Graph
