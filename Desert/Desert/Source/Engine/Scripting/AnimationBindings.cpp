#include "Internal/ScriptRuntime.hpp"

#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Assets/AnimGraphAsset.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/ECS/System/AnimationECSSystem.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace Desert::Scripting
{
    namespace
    {
        namespace G = Animation::Graph;

        std::string EntityName( const LuauEntityRef& ref )
        {
            const auto* tag = ref.Registry->try_get<ECS::TagComponent>( ref.Entity );
            return tag != nullptr ? tag->Tag : std::string();
        }

        /// The entity's AnimationComponent, or nullptr (the entity itself is checked live: a Luau error when
        /// gone).
        ECS::AnimationComponent* Animation( lua_State* L, LuauEntityRef& ref )
        {
            ref = LuauBinder::CheckEntity( L, 1 );
            return ref.Registry->try_get<ECS::AnimationComponent>( ref.Entity );
        }

        // self:getAnimCurve(name) -> number | nil. UE's GetCurveValue, on the clip the entity plays now
        // (blended through a crossfade). nil, logged, when there is no animator or no such curve — 0 is a
        // value a curve can have, so it cannot also mean "not there".
        int GetAnimCurve( lua_State* L )
        {
            LuauEntityRef     ref;
            const auto*       anim = Animation( L, ref );
            const std::string name = luaL_checkstring( L, 2 );
            if ( anim == nullptr )
            {
                LOG_ERROR( "[Anim] getAnimCurve('{}'): the entity has no AnimationComponent, so no clip plays.",
                           name );
                lua_pushnil( L );
                return 1;
            }
            const std::optional<float> value =
                 anim->Animator ? anim->Animator->GetCurveValue( name ) : std::optional<float>{};
            if ( !value )
            {
                LOG_ERROR( "[Anim] getAnimCurve('{}'): entity '{}' plays no clip carrying a keyed curve of that "
                           "name.",
                           name, EntityName( ref ) );
                lua_pushnil( L );
                return 1;
            }
            lua_pushnumber( L, *value );
            return 1;
        }

        // self:isAnimNotifyStateActive(name) -> bool. Whether a notify state of that name spans the
        // playhead of the clip playing now — the polling side of OnAnimationNotifyBegin / End.
        int IsAnimNotifyStateActive( lua_State* L )
        {
            LuauEntityRef     ref;
            const auto*       anim   = Animation( L, ref );
            const std::string name   = luaL_checkstring( L, 2 );
            bool              active = false;
            if ( anim != nullptr && anim->Animator )
            {
                const auto& states = anim->Animator->GetActiveNotifyStates();
                active             = std::any_of( states.begin(), states.end(),
                                                  [&name]( const Animation::ActiveNotifyState& state )
                                                  { return state.Name == name; } );
            }
            lua_pushboolean( L, active );
            return 1;
        }

        // self:linkAnimLayers(path) / self:unlinkAnimLayers(path) -> bool. UE LinkAnimClassLayers /
        // UnlinkAnimClassLayers: the `.danimgraph` at `path` (identified by its GUID) joins or leaves the
        // entity's AnimationComponent::LinkedLayerGraphs, the one list the scene and Details write too; the
        // animation system relinks next tick and reports a refused link by name. False, logged, on an
        // entity with no AnimationComponent, a path naming no graph, or a graph implementing no layer.
        std::optional<Assets::AssetHandle> LayerGraph( lua_State* L, const ECS::AnimationComponent* anim,
                                                       const std::string& path, const char* verb )
        {
            if ( anim == nullptr )
            {
                LOG_ERROR( "[Anim] {}('{}'): the entity has no AnimationComponent to link layers on.", verb,
                           path );
                return std::nullopt;
            }
            Assets::AssetManager* assets = ScriptEngine::Impl::Of( L ).Assets;
            if ( assets == nullptr )
            {
                LOG_ERROR( "[Anim] {}('{}'): no AssetManager bound to resolve the graph through.", verb, path );
                return std::nullopt;
            }
            auto asset = assets->FindByPath<Assets::AnimGraphAsset>( path );
            if ( !asset )
                asset = assets->CreateAsset<Assets::AnimGraphAsset>( path );
            if ( !asset || !asset->GetGraph() )
            {
                LOG_ERROR( "[Anim] {}('{}'): no anim graph at that path.", verb, path );
                return std::nullopt;
            }
            const auto& graph = *asset->GetGraph();
            if ( !graph.Layers || graph.Layers->Implemented.empty() )
            {
                LOG_ERROR( "[Anim] {}('{}'): AnimGraph '{}' implements no layer interface, so it has no layer "
                           "to link.",
                           verb, path, graph.Name );
                return std::nullopt;
            }
            return Assets::AssetHandle( static_cast<uint64_t>( asset->GetMetadata().Handle ) );
        }

        int LinkAnimLayers( lua_State* L )
        {
            LuauEntityRef     ref;
            auto*             anim   = Animation( L, ref );
            const std::string path   = luaL_checkstring( L, 2 );
            const auto        handle = LayerGraph( L, anim, path, "linkAnimLayers" );
            bool              linked = false;
            if ( handle )
            {
                if ( const auto result = ECS::AnimationECSSystem::LinkAnimLayers( *anim, *handle ); !result )
                {
                    LOG_ERROR( "[Anim] linkAnimLayers('{}'): {}", path, result.GetError() );
                }
                else
                {
                    linked = true;
                }
            }
            lua_pushboolean( L, linked );
            return 1;
        }

        int UnlinkAnimLayers( lua_State* L )
        {
            LuauEntityRef     ref;
            auto*             anim     = Animation( L, ref );
            const std::string path     = luaL_checkstring( L, 2 );
            const auto        handle   = LayerGraph( L, anim, path, "unlinkAnimLayers" );
            bool              unlinked = false;
            if ( handle )
            {
                unlinked = ECS::AnimationECSSystem::UnlinkAnimLayers( *anim, *handle );
                if ( !unlinked )
                    LOG_ERROR( "[Anim] unlinkAnimLayers('{}'): the entity has not linked that graph.", path );
            }
            lua_pushboolean( L, unlinked );
            return 1;
        }

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
        int SetAnimParam( lua_State* L )
        {
            LuauEntityRef     ref;
            auto*             component = Animation( L, ref );
            const std::string name      = luaL_checkstring( L, 2 );
            const auto        refuse    = [L]
            {
                lua_pushboolean( L, 0 );
                return 1;
            };

            if ( component == nullptr )
            {
                LOG_ERROR( "[Anim] setAnimParam('{}'): entity '{}' has no AnimationComponent, so there is no "
                           "state machine to drive. Add one in Details, or name a different entity.",
                           name, EntityName( ref ) );
                return refuse();
            }

            auto& anim = *component;
            if ( !anim.Graph )
            {
                LOG_ERROR( "[Anim] setAnimParam('{}'): entity '{}' has an AnimationComponent but no AnimGraph "
                           "on it — it plays a single clip, and a parameter has nothing to reach.",
                           name, EntityName( ref ) );
                return refuse();
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
                           name, anim.Graph->Name, EntityName( ref ), G::DeclaredParameterList( *anim.Graph ) );
                return refuse();
            }

            const auto type = static_cast<G::ParamType>( declared->Type );

            float stored = 0.0F;
            if ( type == G::ParamType::Bool )
            {
                // A NUMBER IS NOT ACCEPTED HERE, and that is the decision. `if x then` in Lua treats 0 as
                // true, so "a number means a bool" has no reading a script author and this engine would
                // agree on — and the one everybody guesses (0 = false) is the C reading, not Lua's.
                if ( lua_type( L, 3 ) != LUA_TBOOLEAN )
                {
                    LOG_ERROR( "[Anim] setAnimParam('{}'): AnimGraph '{}' declares it Bool and the script "
                               "passed {}. Pass true or false — a number is refused because Lua's own "
                               "truthiness would make 0 mean true.",
                               name, anim.Graph->Name, luaL_typename( L, 3 ) );
                    return refuse();
                }
                stored = lua_toboolean( L, 3 ) != 0 ? 1.0F : 0.0F;
            }
            else
            {
                if ( lua_type( L, 3 ) != LUA_TNUMBER )
                {
                    LOG_ERROR( "[Anim] setAnimParam('{}'): AnimGraph '{}' declares it {} and the script "
                               "passed {}.",
                               name, anim.Graph->Name, G::TypeName( type ), luaL_typename( L, 3 ) );
                    return refuse();
                }

                const double number = lua_tonumber( L, 3 );

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
                    return refuse();
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
                        return refuse();
                    }
                }
                stored = static_cast<float>( number );
            }

            // QUEUED, NOT WRITTEN. The evaluator does not exist until AnimationECSSystem has seen this
            // entity with a loaded skinned mesh, so a set from OnStart (or from any frame before an async
            // mesh load lands) would otherwise be swallowed. See AnimationComponent::PendingGraphParams for
            // the argument, and AnimationECSSystem::DrainGraphParams for when it is consumed.
            anim.PendingGraphParams.push_back( { name, stored } );
            lua_pushboolean( L, 1 );
            return 1;
        }
    } // namespace

    void RegisterAnimationBindings( lua_State* L )
    {
        for ( const luaL_Reg& method :
              { luaL_Reg{ "getAnimCurve", &GetAnimCurve },
                luaL_Reg{ "isAnimNotifyStateActive", &IsAnimNotifyStateActive },
                luaL_Reg{ "linkAnimLayers", &LinkAnimLayers }, luaL_Reg{ "unlinkAnimLayers", &UnlinkAnimLayers },
                luaL_Reg{ "setAnimParam", &SetAnimParam } } )
            LuauBinder::SetEntityMethod( L, method.name, method.func );
    }
} // namespace Desert::Scripting
