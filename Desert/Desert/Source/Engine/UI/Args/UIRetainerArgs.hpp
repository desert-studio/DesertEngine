#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>

#include <string>

// An element whose subtree is drawn once into an offscreen target and re-composited.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // UE Retainer Box: the element and its whole subtree are drawn into their own offscreen RGBA target
    // (sized to what the subtree covers on screen, so DPI and canvas scale are already in it), and that
    // picture is composited back through ONE effect pass. Every effect of the layer goes through that pass
    // — a mask by another element's shape, a heat haze — rather than each being a special case of one
    // primitive. Render2D::AddRetainedPasses renders the targets (graph passes);
    // Engine/Graphic/Render2D/RetainerEffect.hpp is the effect's math, mirrored by UIRetainer.shader.
    struct UIRetainerData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Retainer;

        PROPERTY( DisplayName( "Opacity" ), Category( "UI Retainer" ), Range( 0.0f, 1.0f ) )
        float Opacity = 1.0f;

        PROPERTY( DisplayName( "Mask" ), Category( "UI Retainer Mask" ),
                  Tooltip( "Show the layer only where the Mask Element covers it (its alpha, its shape)." ) )
        bool Mask = false;

        PROPERTY( DisplayName( "Mask Element" ), Category( "UI Retainer Mask" ), EditCondition( "Mask" ),
                  Tooltip( "Name of the element on this canvas whose drawn subtree is the mask. It is "
                           "captured even when hidden, so a hidden element is a pure mask." ) )
        std::string MaskElement;

        PROPERTY( DisplayName( "Invert Mask" ), Category( "UI Retainer Mask" ), EditCondition( "Mask" ),
                  Tooltip( "Show the layer where the mask is NOT — a sun hidden behind a dune." ) )
        bool InvertMask = false;

        PROPERTY( DisplayName( "Heat Haze" ), Category( "UI Retainer Haze" ) )
        bool Haze = false;

        PROPERTY( DisplayName( "Haze Amplitude" ), Category( "UI Retainer Haze" ), Range( 0.0f, 64.0f ),
                  Units( "px" ), EditCondition( "Haze" ),
                  Tooltip( "Largest UV displacement. Animate as 'HazeAmplitude'." ) )
        float HazeAmplitude = 3.0f; // design px, scaled by the canvas scale

        PROPERTY( DisplayName( "Haze Scale" ), Category( "UI Retainer Haze" ), Range( 1.0f, 512.0f ),
                  Units( "px" ), EditCondition( "Haze" ), Tooltip( "Size of one shimmer cell." ) )
        float HazeScale = 24.0f; // design px

        PROPERTY( DisplayName( "Haze Speed" ), Category( "UI Retainer Haze" ), Range( 0.0f, 16.0f ),
                  EditCondition( "Haze" ), Tooltip( "Cells per second the shimmer rises, on the view's clock." ) )
        float HazeSpeed = 1.5f;
    };
} // namespace Desert::UI
