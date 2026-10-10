#pragma once

#include <glm/glm.hpp>

#include <span>
#include <vector>

// The CPU half of UIPath: control points -> a polyline measured by ARC LENGTH, and the prefix of that
// polyline a Reveal fraction keeps. Kept free of the draw list and of ECS so a suite can pin "Reveal 0.5
// draws half the length" on the geometry itself, without a GPU and without a canvas.
//
// WHY ARC LENGTH AND NOT THE CURVE PARAMETER. A logo's line is "drawn by a pen": the visible length must
// grow evenly with Reveal. The spline parameter does not — a span between two close points would be drawn
// as slowly as a span between two far ones — so Reveal is measured along the tessellated polyline.
namespace Desert::UI
{
    // A tessellated path. `Distance[i]` is the arc length from Points[0] to Points[i]; Length is the last.
    struct UIPathPolyline
    {
        std::vector<glm::vec2> Points;
        std::vector<float>     Distance;
        float                  Length = 0.0f;
    };

    // Tessellate @p control. `smooth` = centripetal Catmull-Rom THROUGH every control point (no cusps or
    // self-loops on unevenly spaced points, which uniform Catmull-Rom makes); otherwise straight segments.
    // @p segmentsPerSpan subdivides each smooth span. Fewer than two control points -> an empty polyline.
    [[nodiscard]] UIPathPolyline TessellateUIPath( std::span<const glm::vec2> control, bool smooth,
                                                   int segmentsPerSpan = 24 );

    // The prefix of @p path whose arc length is `clamp(reveal, 0, 1) * Length`, ending EXACTLY at that
    // distance (the last segment is cut, not rounded to a vertex). Reveal <= 0 -> empty (nothing is drawn,
    // not even a cap); a zero-length path -> empty.
    [[nodiscard]] std::vector<glm::vec2> RevealUIPath( const UIPathPolyline& path, float reveal );

    // Sum of segment lengths.
    [[nodiscard]] float PolylineLength( std::span<const glm::vec2> points );
} // namespace Desert::UI
