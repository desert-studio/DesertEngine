// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModePaintTools.cpp:505-590 (Erase),
// :593-808 (Sculpt), :878-1000 (Smooth), :1049-1350 (Flatten), :1533-1650 (Noise), :29-46 (noise permutations),
// LandscapeEdModeTools.h:29-160 (FNoiseParameter), :168-280 (LowPassFilter), :474-516 (GetValue / GetNormal) and
// :1134-1142 (StrengthMultiplier), LandscapeEditorObject.h:85-99 (NoiseModeConversion), adapted:
// UObject/ULandscapeEditorObject settings become plain structs, the stroke writes through LandscapeHeightCache
// (L2) on the global sample lattice, kissfft is replaced by a separable DFT, the clay brush, tablet pressure, the
// flatten target and edit layers are not ported, and the stroke records its own before/after snapshot for undo.

#pragma once

#include <Engine/World/Landscape/LandscapeBrush.hpp>
#include <Engine/World/Landscape/LandscapeEditCache.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
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
     * ERASE. UE's Erase (FLandscapeToolStrokeErase) is NOT an edit-layer eraser: it is a heightmap stroke that
     * pulls every brushed sample towards local height 0 (GetTexHeight(0) = the mid sample), floor above and ceil
     * below so it never overshoots. That is exactly what it is here; with no edit layers there is nothing else to
     * return to, and a "base height" would be a second meaning UE does not have.
     *
     * NOT PORTED. The flatten target (bUseFlattenTarget / FlattenTarget) is set in UE by an eyedropper over the
     * viewport; without that widget it would be a number nobody can pick, so Flatten always takes its height from
     * the first point of the stroke, as UE does with the target off. Edit layers do not exist yet, so UE's
     * combined-layers read is the plain cache read. The clay brush (bUseClayBrush) needs per-vertex world normals
     * and a brush plane — a separate tool mode, not a variant of this maths. Tablet pressure is 1 (no pen input
     * here). Both are the stated reason, not an omission: the UE branches they sit in are skipped whole, the rest
     * is UE's line for line.
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

    /// UE's ELandscapeToolFlattenMode.
    enum class LandscapeFlattenMode : uint8_t
    {
        Both,
        Raise,
        Lower,
        Interval,
        Terrace,
    };

    /// UE's ULandscapeEditorObject flatten settings, with UE's defaults.
    struct LandscapeFlattenSettings
    {
        LandscapeFlattenMode Mode = LandscapeFlattenMode::Both;
        /// UE: bUseSlopeFlatten — flatten to the plane through the picked point instead of its height.
        bool UseSlopeFlatten = false;
        /// UE: bPickValuePerApply — re-pick the height at every step instead of once per stroke.
        bool PickValuePerApply = false;
        /// UE: TerraceInterval, world centimetres between terraces (Interval and Terrace modes), UI 1..32768.
        float TerraceIntervalCm = 1.0f;
        /// UE: TerraceSmooth, UI 0.0001..1.
        float TerraceSmooth = 0.0001f;
    };

    inline constexpr float kLandscapeMinTerraceIntervalCm = 1.0f;
    inline constexpr float kLandscapeMaxTerraceIntervalCm = 32768.0f;
    inline constexpr float kLandscapeMinTerraceSmooth     = 0.0001f;
    inline constexpr float kLandscapeMaxTerraceSmooth     = 1.0f;

    Common::BoolResultStr ValidateLandscapeFlatten( const LandscapeFlattenSettings& settings );

    /// UE's ELandscapeToolNoiseMode: Both leaves the noise centred on 0, Add / Sub shift it by its amplitude.
    enum class LandscapeNoiseMode : uint8_t
    {
        Both,
        Add,
        Sub,
    };

    struct LandscapeNoiseSettings
    {
        LandscapeNoiseMode Mode = LandscapeNoiseMode::Both;
        /// UE: NoiseScale in samples per noise period, ClampMin 1, ClampMax 512.
        float NoiseScale = 128.0f;
    };

    inline constexpr float kLandscapeMinNoiseScale = 1.0f;
    inline constexpr float kLandscapeMaxNoiseScale = 512.0f;
    /// UE: ULandscapeEditorObject::MaximumValueRadius — a smaller brush scales the noise amplitude down.
    inline constexpr float kLandscapeNoiseMaximumValueRadiusCm = 10000.0f;

    Common::BoolResultStr ValidateLandscapeNoise( const LandscapeNoiseSettings& settings );

    /**
     * @brief UE's FNoiseParameter(0, scale, 1).Sample(x, z): four octaves of Perlin noise over |x|, |z| with UE's
     *        fixed permutation table. UE has no seed: the field is a function of the GLOBAL sample coordinate, so
     *        a sample on a tile seam gets the same value from either tile and a repeated stroke is repeatable.
     */
    float LandscapeNoiseSample( int32_t x, int32_t z, float noiseScale );

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

        /**
         * FLandscapeToolStrokeFlatten::Apply for the heightmap target. @p pickCm is the stroke position's world
         * X/Z (UE: InteractorPositions[0]); the height (and, for slope flatten, the plane) is read there on the
         * first step, and on every step with PickValuePerApply.
         */
        Common::BoolResultStr ApplyFlatten( const LandscapeBrushWeights&    weights,
                                            const LandscapeBrushSettings&   brush,
                                            const LandscapeFlattenSettings& flatten, glm::vec2 pickCm );

        /// FLandscapeToolStrokeNoise::Apply for the heightmap target.
        Common::BoolResultStr ApplyNoise( const LandscapeBrushWeights&  weights,
                                          const LandscapeBrushSettings& brush,
                                          const LandscapeNoiseSettings& noise );

        /// FLandscapeToolStrokeErase::Apply: towards local height 0, strength clamped to 0..1 as in UE.
        Common::BoolResultStr ApplyErase( const LandscapeBrushWeights&  weights,
                                          const LandscapeBrushSettings& brush );

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
        /// UE's cache GetValue: bilinear at a lattice position, a missing corner taking its nearest neighbour.
        float PickValue( float gx, float gz );
        /// UE's cache GetNormal of the quad at (x, z), in lattice units (x, z, sample value).
        glm::dvec3 PickNormal( int32_t x, int32_t z );

        /// UE's FFlattenValues: the height the stroke flattens to, and the plane for slope flatten.
        struct FlattenValues
        {
            uint16_t   Value = 0;
            glm::dvec3 Normal{ 0.0 };
            float      PlaneDist = 0.0f;
        };
        std::optional<FlattenValues> m_Flatten;

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
