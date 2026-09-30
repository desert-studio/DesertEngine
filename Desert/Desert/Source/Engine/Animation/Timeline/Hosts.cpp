#include "Hosts.hpp"

#include <Engine/ECS/Components.hpp>

namespace Desert::Animation::Timeline
{
    // `EasingPreset` mirrors `UIEasing` value for value (Channel.hpp); the table below is therefore the
    // identity, and these pins are what makes it one — a reorder on either side stops the build here.
    static_assert( static_cast<int>( ECS::UIEasing::Linear ) == static_cast<int>( EasingPreset::Linear ) );
    static_assert( static_cast<int>( ECS::UIEasing::QuadIn ) == static_cast<int>( EasingPreset::QuadIn ) );
    static_assert( static_cast<int>( ECS::UIEasing::QuadOut ) == static_cast<int>( EasingPreset::QuadOut ) );
    static_assert( static_cast<int>( ECS::UIEasing::QuadInOut ) == static_cast<int>( EasingPreset::QuadInOut ) );
    static_assert( static_cast<int>( ECS::UIEasing::CubicIn ) == static_cast<int>( EasingPreset::CubicIn ) );
    static_assert( static_cast<int>( ECS::UIEasing::CubicOut ) == static_cast<int>( EasingPreset::CubicOut ) );
    static_assert( static_cast<int>( ECS::UIEasing::CubicInOut ) == static_cast<int>( EasingPreset::CubicInOut ) );
    static_assert( static_cast<int>( ECS::UIEasing::BackOut ) == static_cast<int>( EasingPreset::BackOut ) );
    static_assert( static_cast<int>( ECS::UIEasing::ElasticOut ) == static_cast<int>( EasingPreset::ElasticOut ) );
    static_assert( static_cast<int>( ECS::UIEasing::BounceOut ) == static_cast<int>( EasingPreset::BounceOut ) );

    EasingPreset PresetOf( const ECS::UIEasing easing )
    {
        return static_cast<EasingPreset>( static_cast<int>( easing ) );
    }
} // namespace Desert::Animation::Timeline
