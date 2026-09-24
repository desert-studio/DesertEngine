// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModePaintTools.cpp:593-808 (Sculpt),
// :878-1000 (Smooth), LandscapeEdModeTools.h:168-280 (LowPassFilter) and :1134-1142 (StrengthMultiplier),
// adapted: UObject/ULandscapeEditorObject settings become plain structs, the stroke writes through
// LandscapeHeightCache (L2) on the global sample lattice, kissfft is replaced by a separable DFT, the clay brush
// and tablet pressure are not ported, and the stroke records its own before/after snapshot for undo.

#pragma once

#include <Engine/World/Landscape/LandscapeBrush.hpp>
#include <Engine/World/Landscape/LandscapeEditCache.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <vector>

namespace Desert::World::Landscape
{
    /**
     * @file
     * @brief UE's heightmap Sculpt and Smooth strokes: the maths of one stroke step over the edit cache.
     *
     * A STROKE is one press of the mouse (UE's FLandscapeToolStroke): it owns the edit cache for its whole life
     * and is applied once per frame while the button is held. Undo is one transaction per stroke, as in UE: the
     * stroke keeps a second cache that is extended BEFORE every write, so it holds each touched sample's value
     * from before the stroke touched it (an extension never re-reads a sample already cached). Finish() hands
     * both rectangles back; writing Before undoes the stroke byte for byte, writing After redoes it.
     *
     * NOT PORTED. The clay brush (bUseClayBrush) needs per-vertex world normals and a brush plane — a separate
     * tool mode, not a variant of this maths. Tablet pressure is 1 (no pen input here). Both are the stated
     * reason, not an omission: the UE branches they sit in are skipped whole, the rest is UE's line for line.
     */

    /// Inclusive rectangle on the root's global sample lattice (the edit cache's convention).
    struct LandscapeSampleBounds
    {
        int32_t X1 = 0;
        int32_t Z1 = 0;
        int32_t X2 = -1;
        int32_t Z2 = -1;

        bool Empty() const
        {
            return X2 < X1 || Z2 < Z1;
        }
    };

    /// UE caps one frame's step at 0.1 s ("under 10 fps slow down paint speed").
    inline constexpr float kLandscapeStrokeMaxDeltaSeconds = 0.1f;

    struct LandscapeSculptStep
    {
        /// UE: bInvert, the Shift modifier — lower instead of raise.
        bool Invert = false;
        /// The frame time this step stands for; clamped to kLandscapeStrokeMaxDeltaSeconds as in UE.
        float DeltaSeconds = kLandscapeStrokeMaxDeltaSeconds;
    };

    /// UE's ULandscapeEditorObject smooth settings, with UE's defaults and clamps.
    struct LandscapeSmoothSettings
    {
        /// UE: SmoothFilterKernelSize, ClampMin 1, ClampMax 31.
        int32_t FilterKernelRadius = 4;
        /// UE: bDetailSmooth — a frequency-domain low pass instead of the box filter.
        bool DetailSmooth = false;
        /// UE: DetailScale, ClampMin 0, ClampMax 0.99 — larger removes more detail.
        float DetailScale = 0.3f;
    };

    inline constexpr int32_t kLandscapeSmoothMinRadius = 1;
    inline constexpr int32_t kLandscapeSmoothMaxRadius = 31;
    inline constexpr float   kLandscapeMaxDetailScale  = 0.99f;

    Common::BoolResultStr ValidateLandscapeSmooth( const LandscapeSmoothSettings& settings );

    /**
     * @brief UE's SculptStrength for one step, in height steps at brush weight 1:
     *        Strength · (RadiusCm · 128 / ZScale) · min(dt, 0.1) · 3, and at least 1 (UE's non-clay floor).
     *        0 when the product is not positive — UE returns without writing.
     */
    float LandscapeSculptStrength( const LandscapeRoot& root, const LandscapeBrushSettings& brush,
                                   const LandscapeSculptStep& step );

    /// One stroke's undo record: the heights of @p Rect before the stroke and after it, row-major, X fastest.
    struct LandscapeStrokeRecord
    {
        LandscapeSampleBounds Rect;
        std::vector<uint16_t> Before;
        std::vector<uint16_t> After;
    };

    class LandscapeHeightStroke
    {
    public:
        /**
         * @param bounds the landscape's sample extent. UE's cache reads zeros outside the landscape; ours refuses
         *               such a sample, so every rectangle a step touches is clipped to @p bounds first.
         */
        LandscapeHeightStroke( const LandscapeRoot& root, LandscapeTileLookup lookup,
                               LandscapeSampleBounds bounds );

        /// FLandscapeToolStrokeSculpt::Apply (non-clay).
        Common::BoolResultStr ApplySculpt( const LandscapeBrushWeights&  weights,
                                           const LandscapeBrushSettings& brush, const LandscapeSculptStep& step );

        /// FLandscapeToolStrokeSmooth::Apply for the heightmap target, strength clamped to 0..1 as in UE.
        Common::BoolResultStr ApplySmooth( const LandscapeBrushWeights&   weights,
                                           const LandscapeBrushSettings&  brush,
                                           const LandscapeSmoothSettings& smooth );

        bool Touched() const
        {
            return m_Touched;
        }

        /// The stroke's undo record over every sample it cached. Refuses a stroke that wrote nothing.
        Common::ResultStr<LandscapeStrokeRecord> Finish() const;

    private:
        /// The step's rectangle: the brush's inclusive bounds grown by one (UE), clipped to the landscape.
        LandscapeSampleBounds StepRect( const LandscapeBrushWeights& weights ) const;
        Common::BoolResultStr Cache( const LandscapeSampleBounds& rect );

        LandscapeRoot         m_Root;
        LandscapeSampleBounds m_Bounds;
        LandscapeHeightCache  m_Cache;
        LandscapeHeightCache  m_Original;
        LandscapeSampleBounds m_Union;
        bool                  m_Touched = false;
    };

    /// Writes @p values over @p rect (undo writes Before, redo writes After). Refuses as SetCachedData does.
    Common::BoolResultStr WriteLandscapeHeights( const LandscapeRoot& root, LandscapeTileLookup lookup,
                                                 const LandscapeSampleBounds& rect,
                                                 const std::vector<uint16_t>& values );
} // namespace Desert::World::Landscape
