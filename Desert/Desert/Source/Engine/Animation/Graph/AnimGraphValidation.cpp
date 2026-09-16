#include "AnimGraphValidation.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <unordered_map>

namespace Desert::Animation::Graph
{
    namespace
    {
        constexpr float kInf = std::numeric_limits<float>::infinity();

        // ── THE VALUES ONE PARAMETER MAY TAKE FOR A CONDITION SET TO HOLD ─────────────────────────────
        //
        // An interval with open/closed ends, plus the points a `!=` (or an `is true`) carved out of it.
        // That is the exact expressive power of `CompareOp`: there is no condition in this model that can
        // produce two disjoint intervals, so nothing here has to approximate.
        struct Domain
        {
            float              Lo     = -kInf;
            float              Hi     = kInf;
            bool               LoOpen = true; // -inf is never attained, so "open" is the honest default
            bool               HiOpen = true;
            std::vector<float> Excluded;
        };

        void Narrow( Domain& domain, const Condition& condition )
        {
            const float value = condition.Value;
            switch ( static_cast<CompareOp>( condition.Op ) )
            {
                case CompareOp::Greater:
                    if ( value >= domain.Lo )
                    {
                        domain.Lo     = value;
                        domain.LoOpen = true;
                    }
                    break;
                case CompareOp::GreaterEqual:
                    if ( value > domain.Lo )
                    {
                        domain.Lo     = value;
                        domain.LoOpen = false;
                    }
                    break;
                case CompareOp::Less:
                    if ( value <= domain.Hi )
                    {
                        domain.Hi     = value;
                        domain.HiOpen = true;
                    }
                    break;
                case CompareOp::LessEqual:
                    if ( value < domain.Hi )
                    {
                        domain.Hi     = value;
                        domain.HiOpen = false;
                    }
                    break;
                case CompareOp::Equals:
                    if ( value > domain.Lo )
                    {
                        domain.Lo     = value;
                        domain.LoOpen = false;
                    }
                    if ( value < domain.Hi )
                    {
                        domain.Hi     = value;
                        domain.HiOpen = false;
                    }
                    // `x == v` against an interval that already excludes v is empty, and `Satisfiable`
                    // below is what notices: the exclusion list is not cleared here.
                    break;
                case CompareOp::NotEquals:
                    domain.Excluded.push_back( value );
                    break;
                case CompareOp::IsTrue:
                    // The evaluator spells this `v != 0.0f`, so this is a `!=` against zero and not a
                    // separate notion of truth. See `Evaluator::EvaluateCondition`.
                    domain.Excluded.push_back( 0.0f );
                    break;
                case CompareOp::IsFalse:
                    if ( 0.0f > domain.Lo )
                    {
                        domain.Lo     = 0.0f;
                        domain.LoOpen = false;
                    }
                    if ( 0.0f < domain.Hi )
                    {
                        domain.Hi     = 0.0f;
                        domain.HiOpen = false;
                    }
                    break;
            }
        }

        [[nodiscard]] bool Admits( const Domain& domain, float value )
        {
            if ( value < domain.Lo || ( value == domain.Lo && domain.LoOpen ) )
                return false;
            if ( value > domain.Hi || ( value == domain.Hi && domain.HiOpen ) )
                return false;
            return std::find( domain.Excluded.begin(), domain.Excluded.end(), value ) == domain.Excluded.end();
        }

        [[nodiscard]] bool Satisfiable( const Domain& domain )
        {
            if ( domain.Lo > domain.Hi )
                return false;
            if ( domain.Lo == domain.Hi )
                return !domain.LoOpen && !domain.HiOpen && Admits( domain, domain.Lo );
            // A non-degenerate interval holds uncountably many values and `Excluded` is finite, so it can
            // never be emptied by exclusions. (An interval narrower than one float step would be the
            // exception; there is no way to author one, because both ends come from literals the user
            // typed.)
            return true;
        }

        /// Every value `inner` admits is admitted by `outer`.
        [[nodiscard]] bool Contains( const Domain& outer, const Domain& inner )
        {
            if ( outer.Lo > inner.Lo )
                return false;
            if ( outer.Lo == inner.Lo && outer.LoOpen && !inner.LoOpen )
                return false;
            if ( outer.Hi < inner.Hi )
                return false;
            if ( outer.Hi == inner.Hi && outer.HiOpen && !inner.HiOpen )
                return false;
            // A point `outer` forbids but `inner` allows is a case where `inner` fires and `outer` does
            // not, which is exactly what containment denies.
            for ( const float point : outer.Excluded )
            {
                if ( Admits( inner, point ) )
                    return false;
            }
            return true;
        }

        using DomainMap = std::unordered_map<std::string, Domain>;

        [[nodiscard]] DomainMap DomainsOf( const Transition& transition )
        {
            DomainMap domains;
            for ( const auto& condition : transition.Conditions )
                Narrow( domains[condition.Parameter], condition );
            return domains;
        }

        [[nodiscard]] bool AllSatisfiable( const DomainMap& domains )
        {
            return std::all_of( domains.begin(), domains.end(),
                                []( const auto& entry ) { return Satisfiable( entry.second ); } );
        }

        /// Whether every parameter assignment that makes @p later eligible also makes @p earlier
        /// eligible. A parameter @p earlier does not mention is unconstrained for it, which contains
        /// anything — so this is checked over the parameters EARLIER constrains and no others.
        [[nodiscard]] bool Shadows( const DomainMap& earlier, const DomainMap& later )
        {
            const Domain unconstrained;
            for ( const auto& [name, outer] : earlier )
            {
                const auto  found = later.find( name );
                const auto& inner = found == later.end() ? unconstrained : found->second;
                if ( !Contains( outer, inner ) )
                    return false;
            }
            return true;
        }

        /// Whether @p earlier's exit-time gate opens no later than @p later's. An earlier transition held
        /// back until 80 % of the clip does NOT shadow one that may fire at once — `Evaluator::Update`
        /// skips it while `normalizedTime` is below the gate, and the later one gets its turn.
        [[nodiscard]] bool ExitGateIsNoStricter( const Transition& earlier, const Transition& later )
        {
            if ( !earlier.HasExitTime )
                return true;
            return later.HasExitTime && later.ExitTime >= earlier.ExitTime;
        }

        /// Whether `Evaluator::Update` would ever ACT on this transition. It skips a transition whose
        /// target does not resolve and one that points at the state it leaves — so such a transition
        /// shadows nothing, and saying otherwise would blame the wrong line.
        [[nodiscard]] bool CanBeTaken( const AnimGraph& graph, const State& from, const Transition& transition )
        {
            if ( transition.To == from.Name )
                return false;
            return std::any_of( graph.States.begin(), graph.States.end(),
                                [&]( const State& state ) { return state.Name == transition.To; } );
        }

        [[nodiscard]] const Parameter* FindParameter( const AnimGraph& graph, const std::string& name )
        {
            const auto found = std::find_if( graph.Parameters.begin(), graph.Parameters.end(),
                                             [&name]( const Parameter& p ) { return p.Name == name; } );
            return found == graph.Parameters.end() ? nullptr : &*found;
        }
    } // namespace

    std::string UndeclaredConditionParameters( const AnimGraph& graph )
    {
        // EVERY CONDITION IS CHECKED ONCE, because the place it is READ cannot refuse: it runs for every
        // condition of every candidate transition, every frame. See `Evaluator::GetStructureError`.
        std::string missing;
        std::size_t count = 0;
        for ( const auto& state : graph.States )
        {
            for ( const auto& transition : state.Transitions )
            {
                for ( const auto& condition : transition.Conditions )
                {
                    if ( FindParameter( graph, condition.Parameter ) != nullptr )
                    {
                        continue;
                    }
                    ++count;
                    missing += missing.empty() ? "" : ", ";
                    missing += fmt::format( "{} -> {} on '{}'", state.Name, transition.To, condition.Parameter );
                }
            }
        }

        if ( count == 0 )
        {
            return {};
        }
        return fmt::format( "{} condition(s) name a parameter this graph does not declare, and each reads 0 and "
                            "compares against it rather than failing: [{}].",
                            count, missing );
    }

    std::vector<GraphWarning> Validate( const AnimGraph& graph, const ClipSet& clips )
    {
        std::vector<GraphWarning> warnings;

        for ( std::size_t si = 0; si < graph.States.size(); ++si )
        {
            const State& state = graph.States[si];

            // ── W1 ────────────────────────────────────────────────────────────────────────────────────
            if ( state.Clip.empty() )
            {
                warnings.push_back(
                     { WarningKind::StateHasNoClip, state.Name, -1,
                       fmt::format( "'{}' names no clip - entering it plays nothing.", state.Name ) } );
            }
            else if ( clips.Known &&
                      std::find( clips.Names.begin(), clips.Names.end(), state.Clip ) == clips.Names.end() )
            {
                warnings.push_back(
                     { WarningKind::StateClipNotAvailable, state.Name, -1,
                       fmt::format( "'{}' names clip '{}', which this skeleton has no animation for.", state.Name,
                                    state.Clip ) } );
            }

            // ── W2 ────────────────────────────────────────────────────────────────────────────────────
            //
            // SHARPENED FROM "THEIR CONDITIONS OVERLAP", AND DELIBERATELY. Mere overlap is the normal
            // shape of an authored graph: `Speed > 0.1` and `Jump is true` overlap at every jump, and the
            // author meant the order to be a priority. A strip that fired there would light up on every
            // real graph and be learned as noise. What is never intended is a transition that can NEVER
            // fire, and that is decidable here: an earlier transition whose eligible set CONTAINS this
            // one's. The doc's own example is this case — the later `Speed > 3.0` sits behind an earlier
            // `Speed > 0.1` and is dead.
            std::vector<DomainMap> domains;
            domains.reserve( state.Transitions.size() );
            for ( const auto& transition : state.Transitions )
                domains.push_back( DomainsOf( transition ) );

            for ( std::size_t ti = 0; ti < state.Transitions.size(); ++ti )
            {
                const Transition& later = state.Transitions[ti];
                if ( !CanBeTaken( graph, state, later ) )
                    continue;

                if ( !AllSatisfiable( domains[ti] ) )
                {
                    warnings.push_back(
                         { WarningKind::TransitionNeverFires, state.Name, static_cast<int>( ti ),
                           fmt::format( "'{}' -> '{}' can never fire: its own conditions contradict each other.",
                                        state.Name, later.To ) } );
                    continue;
                }

                for ( std::size_t ei = 0; ei < ti; ++ei )
                {
                    const Transition& earlier = state.Transitions[ei];
                    if ( !CanBeTaken( graph, state, earlier ) || !AllSatisfiable( domains[ei] ) )
                        continue;
                    if ( !ExitGateIsNoStricter( earlier, later ) )
                        continue;
                    if ( !Shadows( domains[ei], domains[ti] ) )
                        continue;

                    warnings.push_back(
                         { WarningKind::TransitionNeverFires, state.Name, static_cast<int>( ti ),
                           fmt::format( "'{}' -> '{}' can never fire: '{}' -> '{}' is earlier in the list and "
                                        "holds in every case this one does.",
                                        state.Name, later.To, state.Name, earlier.To ) } );
                    break; // one culprit is enough to act on; naming all of them is a wall of text
                }
            }

            // ── W3, per condition, so the strip can name the state that carries it ────────────────────
            for ( std::size_t ti = 0; ti < state.Transitions.size(); ++ti )
            {
                for ( const auto& condition : state.Transitions[ti].Conditions )
                {
                    if ( FindParameter( graph, condition.Parameter ) != nullptr )
                        continue;
                    warnings.push_back(
                         { WarningKind::UndeclaredConditionParam, state.Name, static_cast<int>( ti ),
                           fmt::format( "'{}' -> '{}' tests '{}', which this graph does not declare; it reads "
                                        "0 and compares against that.",
                                        state.Name, state.Transitions[ti].To,
                                        condition.Parameter.empty() ? "<unnamed>" : condition.Parameter ) } );
                }
            }
        }

        return warnings;
    }
} // namespace Desert::Animation::Graph
