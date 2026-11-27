#include <Engine/ECS/AnimationComponent.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/ECS/RelationshipComponent.hpp>

namespace Desert::ECS
{
    AnimationComponent::AnimationComponent() = default;
    AnimationComponent::AnimationComponent( std::unique_ptr<Animation::Animator>&& animator )
         : Animator( std::move( animator ) )
    {
    }
    AnimationComponent::AnimationComponent( AnimationComponent&& ) noexcept            = default;
    AnimationComponent& AnimationComponent::operator=( AnimationComponent&& ) noexcept = default;
    AnimationComponent::~AnimationComponent()                                           = default;

    void MarkPoseTeleported( entt::registry& registry, const entt::entity root )
    {
        if ( !registry.valid( root ) )
            return;
        if ( auto* anim = registry.try_get<AnimationComponent>( root ) )
            anim->PendingTeleport = true;
        if ( const auto* family = registry.try_get<RelationshipComponent>( root ) )
            for ( const entt::entity child : family->Children )
                MarkPoseTeleported( registry, child );
    }
} // namespace Desert::ECS
