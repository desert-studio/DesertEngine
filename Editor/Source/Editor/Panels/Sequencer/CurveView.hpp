#pragma once

/**
 * THE CURVE VIEW'S ARITHMETIC, WITH NO IMGUI IN IT.
 *
 * T4.3 is a VIEW over data three tasks already settled: a key sits on an integer tick (A5), it carries an
 * interpolation mode and tangents (A6), and a tangent is expressed in VALUE UNITS PER SECOND. A curve
 * editor is then almost entirely two mappings and their inverses, and putting them here rather than inside
 * the draw loop is not tidiness — it is the only way any of it can be asserted, because
 * `SequencerPanel.cpp` is compiled by no suite (scripts/CI/UnreachedSources.sh) and an ImGui frame is not
 * something a test can open.
 *
 * ── THE CONVERSION LIVES IN ONE PLACE, AND THIS IS THAT PLACE ────────────────────────────────────────
 *
 * A tangent is a slope in value-per-second; the screen is in pixels; and between them sit two independent
 * scales (pixels per second, pixels per value unit) plus a Y axis that points DOWN. Every one of those is
 * a sign or a factor that can be got wrong, and got wrong separately at each site that repeats it. So
 * there is exactly one function that turns a slope into a handle offset, exactly one that turns a handle
 * offset back into a slope, and they are inverses over the domain where the inverse exists.
 *
 * ── WHAT IS DELIBERATELY NOT HERE ────────────────────────────────────────────────────────────────────
 *
 * No px-per-second field. The time axis is a RANGE mapped onto a rectangle, because the thing a curve
 * editor must guarantee is that its horizontal position agrees with the dope sheet's — and the dope sheet
 * maps `duration` onto its lane width. A second zoom model beside that one is how the playhead and the
 * key under it end up two pixels apart at some zoom levels and nobody can say which is lying.
 */

#include <Engine/Animation/KeyInterpolation.hpp>
#include <Engine/Animation/TimeModel.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace Desert::Editor::Sequencer
{
    /**
     * @brief The rectangle a curve is drawn in, and the whole of curve-space <-> pixel-space.
     *
     * `Y0` is the TOP edge, as everywhere in ImGui, so a larger value maps to a SMALLER y. That inversion
     * is stated once here and nowhere else.
     */
    struct CurveViewport
    {
        float X0 = 0.0F; ///< left edge, pixels
        float X1 = 1.0F; ///< right edge, pixels
        float Y0 = 0.0F; ///< top edge, pixels
        float Y1 = 1.0F; ///< bottom edge, pixels

        double TimeStart = 0.0; ///< seconds at X0
        double TimeEnd   = 1.0; ///< seconds at X1

        float ValueMin = 0.0F; ///< value at Y1 (the BOTTOM)
        float ValueMax = 1.0F; ///< value at Y0 (the top)

        [[nodiscard]] double PixelsPerSecond() const;
        [[nodiscard]] float  PixelsPerValue() const;

        [[nodiscard]] float  TimeToX( double seconds ) const;
        [[nodiscard]] double XToTime( float x ) const;
        [[nodiscard]] float  ValueToY( float value ) const;
        [[nodiscard]] float  YToValue( float y ) const;

        /**
         * @brief A tangent, as the pixel offset from its key to the end of its handle.
         *
         * THE HANDLE HAS A FIXED PIXEL LENGTH and carries the slope in its DIRECTION only. UE draws them
         * this way and the reason is not cosmetic: a handle whose length was the tangent's magnitude is
         * off-screen for a steep key and invisible for a gentle one, at which point the animator is
         * dragging something they cannot see. Length is reserved for weights, which A6 shipped as zero.
         *
         * @param slope         value units per second — the unit tangents are stored in.
         * @param handlePixels  how long to draw it.
         * @param leaving       true for the handle that leaves the key (to its right), false for the one
         *                      that arrives at it. The two are mirror images, and drawing both from one
         *                      call is what keeps a "break" visible as an actual corner.
         */
        [[nodiscard]] glm::vec2 HandleOffset( float slope, float handlePixels, bool leaving ) const;

        /**
         * @brief The slope a dragged handle now means, or nothing when the drag says nothing.
         *
         * REFUSES A HANDLE DRAGGED PAST ITS OWN KEY. A leaving handle at or behind the key (dx <= 0) is
         * not a steeper slope — it is a slope with no sign, and the arithmetic answer is an infinity that
         * would be written into the file. Returning nothing leaves the tangent where the animator last
         * put it, which is what a mouse that slipped should do.
         */
        [[nodiscard]] std::optional<float> SlopeFromHandle( glm::vec2 offsetPixels, bool leaving ) const;
    };

    /**
     * @brief The value range to draw a set of scalar keys in, padded, never degenerate.
     *
     * A CHANNEL THAT NEVER MOVES IS THE CASE THAT BREAKS THE NAIVE FIT: min == max makes the value scale a
     * division by zero, and every key lands on a NaN. A flat channel is also the most common thing in a
     * clip (every bone's scale), so it is the default case rather than an edge one. It gets a unit window
     * around its value, which draws as a horizontal line through the middle — what it is.
     */
    [[nodiscard]] glm::vec2 FitValueRange( const std::vector<Animation::ScalarKey>& keys, float paddingFraction );

    /**
     * @brief How many DISPLAY FRAMES to leave between grid lines so they stay readable.
     *
     * ONE FUNCTION, TWO CONSUMERS: the ruler and the curve view. The brief for this task said the display
     * rate "already has a consumer — the grid the timeline snaps to", and that turned out to be half true
     * in a way worth knowing: the display rate is what a dragged key SNAPS to, but the ruler drew one line
     * per whole SECOND and labelled it with an integer second. So the grid the animator sees and the grid
     * their key lands on were different grids. Both read this now.
     *
     * Returns a step from the 1-2-5-10 ladder (so labels stay round) such that the gap is at least
     * @p minPixels wide. Never returns 0.
     */
    [[nodiscard]] int32_t ChooseFrameStep( double pixelsPerFrame, float minPixels );
} // namespace Desert::Editor::Sequencer
