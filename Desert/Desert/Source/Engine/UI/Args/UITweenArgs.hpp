#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>
#include <Engine/UI/Args/UIEasing.hpp>

#include <glm/glm.hpp>

// A one-property tween authored on an element.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // What a tween drives. From/To are read per property: Offset/Size use xy (design px), Opacity uses x,
    // Color uses rgb — one vec4 keeps the component flat instead of four half-used fields.
    enum class UITweenProperty
    {
        Offset,  // slide: shifts the resolved rect
        Size,    // grow/shrink: adds to the rect's width/height
        Opacity, // fade: multiplies the element's alpha
        Color    // tint: multiplies the element's colour
    };

    enum class UITweenLoop
    {
        Once,
        Loop,
        PingPong
    };

    // A generic from->to animation on any UI element, evaluated while the canvas is drawn. It never
    // writes back into the authored fields — the value is applied on the way to the screen — so a tween
    // running in the editor cannot corrupt the scene, and Design mode previews it live.
    struct UITweenData
    {
        REFLECT()

        static constexpr ArgKind Kind = ArgKind::Tween;

        PROPERTY( DisplayName( "Property" ), Category( "UI Tween" ) )
        UITweenProperty Property = UITweenProperty::Offset;

        PROPERTY( DisplayName( "From" ), Category( "UI Tween" ),
                  Tooltip( "Offset/Size: xy in design px. Opacity: x. Color: rgb." ) )
        glm::vec4 From = glm::vec4( 0.0f );

        PROPERTY( DisplayName( "To" ), Category( "UI Tween" ) )
        glm::vec4 To = glm::vec4( 0.0f );

        PROPERTY( DisplayName( "Duration" ), Category( "UI Tween" ), Range( 0.01f, 20.0f ) )
        float Duration = 0.4f;

        PROPERTY( DisplayName( "Delay" ), Category( "UI Tween" ), Range( 0.0f, 20.0f ) )
        float Delay = 0.0f;

        PROPERTY( DisplayName( "Easing" ), Category( "UI Tween" ) )
        UIEasing Easing = UIEasing::CubicOut;

        PROPERTY( DisplayName( "Loop" ), Category( "UI Tween" ) )
        UITweenLoop Loop = UITweenLoop::Once;

        PROPERTY( DisplayName( "Playing" ), Category( "UI Tween" ) )
        bool Playing = true; // clear to freeze at the current value; set to (re)start from the delay

        PROPERTY( DisplayName( "Rewind On Hide" ), Category( "UI Tween" ) )
        bool RewindOnHide = true; // a canvas that goes invisible replays from the top when it returns
    };
} // namespace Desert::UI
