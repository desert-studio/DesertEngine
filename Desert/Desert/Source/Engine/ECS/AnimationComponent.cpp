#include <Engine/ECS/AnimationComponent.hpp>

#include <Engine/Animation/Animator.hpp>

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
} // namespace Desert::ECS
