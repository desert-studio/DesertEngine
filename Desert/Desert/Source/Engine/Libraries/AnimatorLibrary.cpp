#include <Engine/Libraries/AnimatorLibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/Assets/AnimGraphAsset.hpp>
#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Core/WorldContext.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/AnimationECSSystem.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace Desert::Libraries
{
    namespace
    {
        namespace G = Animation::Graph;

        std::string EntityName( const ECS::Entity& entity )
        {
            const auto* tag = entity.GetRegistry()->try_get<ECS::TagComponent>( entity.GetHandle() );
            return tag != nullptr ? tag->Tag : std::string();
        }

        ECS::AnimationComponent* AnimationOf( const ECS::Entity& entity )
        {
            return entity ? entity.GetRegistry()->try_get<ECS::AnimationComponent>( entity.GetHandle() ) : nullptr;
        }

        std::optional<Assets::AssetHandle> LayerGraph( const ECS::AnimationComponent* anim,
                                                       const std::string& path, const char* verb )
        {
            if ( anim == nullptr )
            {
                LOG_ERROR( "[Anim] {}('{}'): the entity has no AnimationComponent to link layers on.", verb,
                           path );
                return std::nullopt;
            }
            const Core::WorldContext* context = Core::WorldContext::Current();
            Assets::AssetManager*     assets  = context != nullptr ? context->Assets : nullptr;
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

        // A script number arrives as whichever numeric kind its language chose.
        std::optional<double> Number( const Reflection::Value& value )
        {
            if ( const auto* d = value.Get<double>() )
                return *d;
            if ( const auto* f = value.Get<float>() )
                return *f;
            if ( const auto* i = value.Get<std::int64_t>() )
                return static_cast<double>( *i );
            if ( const auto* u = value.Get<std::uint64_t>() )
                return static_cast<double>( *u );
            return std::nullopt;
        }
    } // namespace

    Reflection::Value AnimatorLibrary::GetAnimCurve( ECS::Entity entity, const std::string& name )
    {
        const auto* anim = AnimationOf( entity );
        if ( anim == nullptr )
        {
            LOG_ERROR( "[Anim] getAnimCurve('{}'): the entity has no AnimationComponent, so no clip plays.",
                       name );
            return {};
        }
        const std::optional<float> value =
             anim->Animator ? anim->Animator->GetCurveValue( name ) : std::optional<float>{};
        if ( !value )
        {
            LOG_ERROR( "[Anim] getAnimCurve('{}'): entity '{}' plays no clip carrying a keyed curve of that name.",
                       name, EntityName( entity ) );
            return {};
        }
        return Reflection::Value::Float( *value );
    }

    bool AnimatorLibrary::IsAnimNotifyStateActive( ECS::Entity entity, const std::string& name )
    {
        const auto* anim = AnimationOf( entity );
        if ( anim == nullptr || !anim->Animator )
            return false;
        const auto& states = anim->Animator->GetActiveNotifyStates();
        return std::any_of( states.begin(), states.end(),
                            [&name]( const Animation::ActiveNotifyState& state ) { return state.Name == name; } );
    }

    bool AnimatorLibrary::LinkAnimLayers( ECS::Entity entity, const std::string& path )
    {
        auto*      anim   = AnimationOf( entity );
        const auto handle = LayerGraph( anim, path, "linkAnimLayers" );
        if ( !handle )
            return false;
        if ( const auto result = ECS::AnimationECSSystem::LinkAnimLayers( *anim, *handle ); !result )
        {
            LOG_ERROR( "[Anim] linkAnimLayers('{}'): {}", path, result.GetError() );
            return false;
        }
        return true;
    }

    bool AnimatorLibrary::UnlinkAnimLayers( ECS::Entity entity, const std::string& path )
    {
        auto*      anim   = AnimationOf( entity );
        const auto handle = LayerGraph( anim, path, "unlinkAnimLayers" );
        if ( !handle )
            return false;
        const bool unlinked = ECS::AnimationECSSystem::UnlinkAnimLayers( *anim, *handle );
        if ( !unlinked )
            LOG_ERROR( "[Anim] unlinkAnimLayers('{}'): the entity has not linked that graph.", path );
        return unlinked;
    }

    bool AnimatorLibrary::SetAnimParam( ECS::Entity entity, const std::string& name,
                                        const Reflection::Value& value )
    {
        auto* component = AnimationOf( entity );
        if ( component == nullptr )
        {
            LOG_ERROR( "[Anim] setAnimParam('{}'): entity '{}' has no AnimationComponent, so there is no state "
                       "machine to drive. Add one in Details, or name a different entity.",
                       name, entity ? EntityName( entity ) : std::string() );
            return false;
        }
        auto& anim = *component;
        if ( !anim.Graph )
        {
            LOG_ERROR( "[Anim] setAnimParam('{}'): entity '{}' has an AnimationComponent but no AnimGraph on it — "
                       "it plays a single clip, and a parameter has nothing to reach.",
                       name, EntityName( entity ) );
            return false;
        }
        const auto declared = std::find_if( anim.Graph->Parameters.begin(), anim.Graph->Parameters.end(),
                                            [&name]( const G::Parameter& p ) { return p.Name == name; } );
        if ( declared == anim.Graph->Parameters.end() )
        {
            LOG_ERROR( "[Anim] setAnimParam('{}'): AnimGraph '{}' on entity '{}' has no such parameter. It "
                       "declares: {}.",
                       name, anim.Graph->Name, EntityName( entity ), G::DeclaredParameterList( *anim.Graph ) );
            return false;
        }
        const auto type   = static_cast<G::ParamType>( declared->Type );
        float      stored = 0.0F;
        if ( type == G::ParamType::Bool )
        {
            const bool* flag = value.Get<bool>();
            if ( flag == nullptr )
            {
                LOG_ERROR( "[Anim] setAnimParam('{}'): AnimGraph '{}' declares it Bool and the script passed {}. "
                           "Pass true or false — a number is refused because Lua's own truthiness would make 0 "
                           "mean true.",
                           name, anim.Graph->Name, Reflection::FieldTypeName( value.Type() ) );
                return false;
            }
            stored = *flag ? 1.0F : 0.0F;
        }
        else
        {
            const std::optional<double> number = Number( value );
            if ( !number )
            {
                LOG_ERROR( "[Anim] setAnimParam('{}'): AnimGraph '{}' declares it {} and the script passed {}.",
                           name, anim.Graph->Name, G::TypeName( type ),
                           Reflection::FieldTypeName( value.Type() ) );
                return false;
            }
            if ( !std::isfinite( *number ) )
            {
                LOG_ERROR( "[Anim] setAnimParam('{}'): the script passed {}, which is not a finite number. "
                           "Stored, it would make every condition that reads this parameter compare false (NaN) "
                           "or true (infinity) with no diagnostic anywhere.",
                           name, *number );
                return false;
            }
            if ( type == G::ParamType::Int && *number != std::floor( *number ) )
            {
                LOG_ERROR( "[Anim] setAnimParam('{}'): AnimGraph '{}' declares it Int and the script passed {}, "
                           "which is not whole. Rounding it here would be a value the script never wrote.",
                           name, anim.Graph->Name, *number );
                return false;
            }
            stored = static_cast<float>( *number );
        }
        anim.PendingGraphParams.push_back( { name, stored } );
        return true;
    }
} // namespace Desert::Libraries
