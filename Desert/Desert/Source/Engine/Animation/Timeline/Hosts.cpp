#include "Hosts.hpp"

#include <Engine/UI/Args/UIEasing.hpp>

namespace Desert::Animation::Timeline
{
    // `EasingPreset` mirrors `UIEasing` value for value (Channel.hpp); the table below is therefore the
    // identity, and these pins are what makes it one — a reorder on either side stops the build here.
    static_assert( static_cast<int>( UI::UIEasing::Linear ) == static_cast<int>( EasingPreset::Linear ) );
    static_assert( static_cast<int>( UI::UIEasing::QuadIn ) == static_cast<int>( EasingPreset::QuadIn ) );
    static_assert( static_cast<int>( UI::UIEasing::QuadOut ) == static_cast<int>( EasingPreset::QuadOut ) );
    static_assert( static_cast<int>( UI::UIEasing::QuadInOut ) == static_cast<int>( EasingPreset::QuadInOut ) );
    static_assert( static_cast<int>( UI::UIEasing::CubicIn ) == static_cast<int>( EasingPreset::CubicIn ) );
    static_assert( static_cast<int>( UI::UIEasing::CubicOut ) == static_cast<int>( EasingPreset::CubicOut ) );
    static_assert( static_cast<int>( UI::UIEasing::CubicInOut ) == static_cast<int>( EasingPreset::CubicInOut ) );
    static_assert( static_cast<int>( UI::UIEasing::BackOut ) == static_cast<int>( EasingPreset::BackOut ) );
    static_assert( static_cast<int>( UI::UIEasing::ElasticOut ) == static_cast<int>( EasingPreset::ElasticOut ) );
    static_assert( static_cast<int>( UI::UIEasing::BounceOut ) == static_cast<int>( EasingPreset::BounceOut ) );

    EasingPreset PresetOf( const UI::UIEasing easing )
    {
        return static_cast<EasingPreset>( static_cast<int>( easing ) );
    }
} // namespace Desert::Animation::Timeline
