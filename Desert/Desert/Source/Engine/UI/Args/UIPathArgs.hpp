#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>

#include <glm/glm.hpp>

// A stroked/filled vector path.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // How UIPath joins its control points.
    enum class UIPathCurve
    {
        Linear, // straight segments: a polyline through the points
        Smooth  // centripetal Catmull-Rom THROUGH every point (no cusps on unevenly spaced points)
    };

    // A stroked line drawn as UI — the pen line of a logo, an underline that draws itself, a dune contour.
    // An element of the canvas like a panel: it lives in its UILayout rect, and its points are FRACTIONS of
    // that rect ((0,0) top-left, (1,1) bottom-right), so it follows anchors and resolution like everything
    // else; stretch the element over the canvas and the points are fractions of the canvas.
    //
    // Reveal draws the line by ARC LENGTH (0 = nothing, 0.5 = the first half of its length, 1 = all of it),
    // from P0 towards the last point — the "drawn by hand" reveal; a UI animation drives it with the
    // "Reveal" property. The stroke is antialiased by geometry (Feather px of fringe), its ends are round
    // half-discs, and Glow is the same stroke widened by Glow Radius and fading to nothing.
    //
    // WHY EIGHT FIXED POINTS rather than a list: reflection has no array field (FieldType has no sequence),
    // so a list would be invisible to Details, to the timeline and to the serializer alike. Point Count
    // says how many of P0..P7 the path uses.
    struct UIPathData
    {
        REFLECT()

        static constexpr ArgKind Kind = ArgKind::Path;

        PROPERTY( DisplayName( "Curve" ), Category( "UI Path" ) )
        UIPathCurve Curve = UIPathCurve::Smooth;

        PROPERTY( DisplayName( "Point Count" ), Category( "UI Path" ), Range( 2.0f, 8.0f ) )
        int PointCount = 3;

        PROPERTY( DisplayName( "Reveal" ), Category( "UI Path" ), Range( 0.0f, 1.0f ),
                  Tooltip( "Fraction of the line's LENGTH drawn, from the first point on. Animate as 'Reveal'." ) )
        float Reveal = 1.0f;

        PROPERTY( DisplayName( "Thickness" ), Category( "UI Path" ), Range( 0.5f, 64.0f ), Units( "px" ) )
        float Thickness = 4.0f; // design px, scaled by the canvas scale

        PROPERTY( DisplayName( "Color" ), Category( "UI Path" ), Color )
        glm::vec3 Color = glm::vec3( 0.95f, 0.78f, 0.45f );

        PROPERTY( DisplayName( "Opacity" ), Category( "UI Path" ), Range( 0.0f, 1.0f ) )
        float Opacity = 1.0f;

        PROPERTY( DisplayName( "Round Caps" ), Category( "UI Path" ) )
        bool RoundCaps = true;

        PROPERTY( DisplayName( "Antialias Width" ), Category( "UI Path" ), Range( 0.0f, 4.0f ), Units( "px" ),
                  Tooltip( "Soft fringe on each side of the stroke, in screen px. 0 = hard edge." ) )
        float Feather = 1.0f;

        PROPERTY( DisplayName( "Point 0" ), Category( "UI Path Points" ) )
        glm::vec2 P0 = glm::vec2( 0.1f, 0.5f );
        PROPERTY( DisplayName( "Point 1" ), Category( "UI Path Points" ) )
        glm::vec2 P1 = glm::vec2( 0.5f, 0.3f );
        PROPERTY( DisplayName( "Point 2" ), Category( "UI Path Points" ) )
        glm::vec2 P2 = glm::vec2( 0.9f, 0.5f );
        PROPERTY( DisplayName( "Point 3" ), Category( "UI Path Points" ) )
        glm::vec2 P3 = glm::vec2( 1.0f, 0.5f );
        PROPERTY( DisplayName( "Point 4" ), Category( "UI Path Points" ) )
        glm::vec2 P4 = glm::vec2( 1.0f, 0.5f );
        PROPERTY( DisplayName( "Point 5" ), Category( "UI Path Points" ) )
        glm::vec2 P5 = glm::vec2( 1.0f, 0.5f );
        PROPERTY( DisplayName( "Point 6" ), Category( "UI Path Points" ) )
        glm::vec2 P6 = glm::vec2( 1.0f, 0.5f );
        PROPERTY( DisplayName( "Point 7" ), Category( "UI Path Points" ) )
        glm::vec2 P7 = glm::vec2( 1.0f, 0.5f );

        PROPERTY( DisplayName( "Glow" ), Category( "Effects" ) )
        bool Glow = false;
        PROPERTY( DisplayName( "Glow Color" ), Category( "Effects" ), Color, EditCondition( "Glow" ) )
        glm::vec3 GlowColor = glm::vec3( 1.0f, 0.70f, 0.35f );
        PROPERTY( DisplayName( "Glow Radius" ), Category( "Effects" ), Range( 0.0f, 64.0f ), Units( "px" ),
                  EditCondition( "Glow" ) )
        float GlowRadius = 12.0f; // design px past the stroke's edge over which the glow fades out
        PROPERTY( DisplayName( "Glow Strength" ), Category( "Effects" ), Range( 0.0f, 1.0f ),
                  EditCondition( "Glow" ) )
        float GlowStrength = 0.6f; // glow opacity at the stroke's edge
    };
} // namespace Desert::UI
