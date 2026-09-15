#include "Internal/ScriptRuntime.hpp"

#include <Engine/Animation/Graph/AnimGraph.hpp>

#include <cmath>
#include <string>

namespace Desert::Scripting
{
    namespace
    {
        namespace G = Animation::Graph;

        /// The Lua type of a value, for a refusal that has to say what arrived. sol's own `type_name`
        /// needs the state; this needs only the tag, and it is the tag the message quotes.
        const char* LuaTypeName( const sol::object& value )
        {
            switch ( value.get_type() )
            {
                case sol::type::boolean:
                    return "boolean";
                case sol::type::number:
                    return "number";
                case sol::type::string:
                    return "string";
                case sol::type::lua_nil:
                    // `lua_nil` rather than `nil`: the shorter spelling is a conditional alias in sol2
                    // (SOL_NIL), because `nil` is an Objective-C keyword present in the macOS SDK.
                    return "nil";
                case sol::type::table:
                    return "table";
                case sol::type::function:
                    return "function";
                default:
                    return "a value of another type";
            }
        }

    } // namespace

    /**
     * ANIMATION DOMAIN — the one thing the AnimGraph was missing to be a runtime feature.
     *
     * Before this file, `Evaluator::SetFloat`/`SetBool` were called from EXACTLY TWO PLACES in the whole
     * repository, both of them the editor panel's live-value sliders (`AnimGraphPanel.cpp:362`, `:366`).
     * So in Play mode, and in a packaged game, NOTHING could change a parameter: the graph could only sit
     * in its entry state or auto-advance on exit time. A complete authoring feature with no runtime.
     * `Docs/Animation/06_gap_analysis.md` §6 calls T3.1 the highest value per line in the document for
     * exactly that reason, and this is the whole of it.
     *
     * ONE FUNCTION, NOT THREE, AND THE GRAPH DECIDES THE TYPE. `setAnimBool` / `setAnimInt` /
     * `setAnimFloat` would let a script assert a type the graph disagrees with, which is a second answer
     * to "what is this parameter" — and the graph already answers it, in the `Parameter::Type` an artist
     * chose in the panel. So the script passes a VALUE and the declared type says how to read it; a
     * mismatch is named, not coerced. Coercion was the tempting version and it is the one that hides the
     * bug: `setAnimParam("IsRunning", 0.4)` on a Bool would become `true` and read as working.
     */
    void RegisterAnimationBindings( ScriptEngine::Impl& impl )
    {
        sol::table entity = impl.Lua["Entity"];

        // self:setAnimParam(name, value) -> bool. Returns false AND logs on every refusal: the boolean is
        // for the script that wants to branch, the log is for the developer who does not know yet that
        // there is something to branch on.
        entity["setAnimParam"] =
             []( ScriptEntity& self, const std::string& name, const sol::object& value ) -> bool
        {
            if ( !self.Valid() )
            {
                LOG_ERROR( "[Anim] setAnimParam('{}') called on an entity that no longer exists.", name );
                return false;
            }

            if ( !self.Reg().has<ECS::AnimationComponent>( self.handle ) )
            {
                LOG_ERROR( "[Anim] setAnimParam('{}'): entity '{}' has no AnimationComponent, so there is no "
                           "state machine to drive. Add one in Details, or name a different entity.",
                           name, self.Name() );
                return false;
            }

            auto& anim = self.Reg().get<ECS::AnimationComponent>( self.handle );
            if ( !anim.Graph )
            {
                LOG_ERROR( "[Anim] setAnimParam('{}'): entity '{}' has an AnimationComponent but no AnimGraph "
                           "on it — it plays a single clip, and a parameter has nothing to reach.",
                           name, self.Name() );
                return false;
            }

            const auto declared = std::find_if( anim.Graph->Parameters.begin(), anim.Graph->Parameters.end(),
                                                [&name]( const G::Parameter& p ) { return p.Name == name; } );
            if ( declared == anim.Graph->Parameters.end() )
            {
                // THE TYPO, NAMED, WITH THE LIST. This is the refusal the task exists to make impossible to
                // miss: the write used to create the parameter, hold the value, be read by no condition and
                // report nothing at all.
                LOG_ERROR( "[Anim] setAnimParam('{}'): AnimGraph '{}' on entity '{}' has no such parameter. "
                           "It declares: {}.",
                           name, anim.Graph->Name, self.Name(), G::DeclaredParameterList( *anim.Graph ) );
                return false;
            }

            const auto type = static_cast<G::ParamType>( declared->Type );

            float stored = 0.0F;
            if ( type == G::ParamType::Bool )
            {
                // A NUMBER IS NOT ACCEPTED HERE, and that is the decision. `if x then` in Lua treats 0 as
                // true, so "a number means a bool" has no reading a script author and this engine would
                // agree on — and the one everybody guesses (0 = false) is the C reading, not Lua's.
                if ( value.get_type() != sol::type::boolean )
                {
                    LOG_ERROR( "[Anim] setAnimParam('{}'): AnimGraph '{}' declares it Bool and the script "
                               "passed {}. Pass true or false — a number is refused because Lua's own "
                               "truthiness would make 0 mean true.",
                               name, anim.Graph->Name, LuaTypeName( value ) );
                    return false;
                }
                stored = value.as<bool>() ? 1.0F : 0.0F;
            }
            else
            {
                if ( value.get_type() != sol::type::number )
                {
                    LOG_ERROR( "[Anim] setAnimParam('{}'): AnimGraph '{}' declares it {} and the script "
                               "passed {}.",
                               name, anim.Graph->Name, G::TypeName( type ), LuaTypeName( value ) );
                    return false;
                }

                const double number = value.as<double>();

                // NaN AND INFINITY ARE REFUSED, and this is the quietest of the three. Lua has no integer
                // division guard: `0/0` is NaN and it propagates through every arithmetic step without a
                // word. Stored in a parameter it makes EVERY comparison in every condition false — `>`,
                // `<`, `==` alike — so the machine freezes in its current state and the graph looks
                // correct, the script looks correct, and nothing anywhere has anything to say. An
                // infinity is the mirror: every `>` becomes true at once.
                if ( !std::isfinite( number ) )
                {
                    LOG_ERROR( "[Anim] setAnimParam('{}'): the script passed {}, which is not a finite "
                               "number. Stored, it would make every condition that reads this parameter "
                               "compare false (NaN) or true (infinity) with no diagnostic anywhere.",
                               name, number );
                    return false;
                }

                if ( type == G::ParamType::Int )
                {
                    // REFUSED RATHER THAN ROUNDED. A silent round turns `count / 2` into a number the
                    // script never wrote, and the condition it feeds then compares against the wrong one;
                    // the script author is the only person who can decide whether that should floor,
                    // round or be a Float parameter instead.
                    if ( number != std::floor( number ) )
                    {
                        LOG_ERROR( "[Anim] setAnimParam('{}'): AnimGraph '{}' declares it Int and the script "
                                   "passed {}, which is not whole. Rounding it here would be a value the "
                                   "script never wrote.",
                                   name, anim.Graph->Name, number );
                        return false;
                    }
                }
                stored = static_cast<float>( number );
            }

            // QUEUED, NOT WRITTEN. The evaluator does not exist until AnimationECSSystem has seen this
            // entity with a loaded skinned mesh, so a set from OnStart (or from any frame before an async
            // mesh load lands) would otherwise be swallowed. See AnimationComponent::PendingGraphParams for
            // the argument, and AnimationECSSystem::DrainGraphParams for when it is consumed.
            anim.PendingGraphParams.push_back( { name, stored } );
            return true;
        };
    }
} // namespace Desert::Scripting
