#pragma once

#include <Editor/Panels/Sequencer/CurveView.hpp>

#include <Engine/Animation/TimeModel.hpp>

#include <ImGui/imgui.h>

namespace Desert::Editor::Sequencer
{
    /// The time axis of a lane area, as the ONE mapping the ruler, the dope sheet, the curve view and the
    /// Animation Editor's notify tracks all read. Three copies of `laneX0 + (t/duration)*laneW` is how a
    /// playhead and the key under it end up two pixels apart at some zoom and nobody can say which is lying.
    [[nodiscard]] CurveViewport TimeAxis( float laneX0, float laneW, float durationSeconds );

    /// The DISPLAY-RATE frame grid, drawn between @p yTop and @p yBottom; @p labels writes the frame number
    /// beside each line. ONE FUNCTION, EVERY TIMELINE — the Sequencer's ruler and curve view, and the
    /// Animation Editor's ruler — so the grid an animator sees is the grid a dragged key lands on.
    void DrawFrameGrid( ImDrawList* dl, const CurveViewport& axis, float yTop, float yBottom,
                        Animation::FrameNumber durationTicks, Animation::FrameRate tickRate,
                        Animation::FrameRate displayRate, bool labels, ImU32 lineColour );

    /// The playhead line at @p seconds across [@p yTop, @p yBottom], with UE's marker on the ruler when
    /// @p rulerBottom is below @p yTop.
    void DrawPlayhead( ImDrawList* dl, const CurveViewport& axis, double seconds, float yTop, float yBottom,
                       float rulerBottom );
} // namespace Desert::Editor::Sequencer
