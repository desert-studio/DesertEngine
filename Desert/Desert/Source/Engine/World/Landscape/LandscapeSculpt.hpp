// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEdModePaintTools.cpp:505-590 (Erase),
// :593-808 (Sculpt), :878-1000 (Smooth), :1049-1350 (Flatten), :1533-1650 (Noise), :29-46 (noise permutations),
// LandscapeEdModeTools.h:29-160 (FNoiseParameter), :168-280 (LowPassFilter), :474-516 (GetValue / GetNormal) and
// :1134-1142 (StrengthMultiplier), LandscapeEditorObject.h:85-99 (NoiseModeConversion),
// LandscapeEdModeRampTool.cpp:29-75 (FLandscapeRampToolHeightRasterPolicy) and :486-613 (ApplyRamp),
// LandscapeEditorObject.h:396-403 with LandscapeEditorObject.cpp:57-58 (RampWidth / RampSideFalloff),
// LandscapeEdModeErosionTools.cpp:61-257 (Erosion) and :265-523 (Hydraulic Erosion) with LandscapeEditorObject.h
// :423-474 and LandscapeEditorObject.cpp:64-77 (their settings), adapted: no paint-layer weight transfer or
// hardness, float-to-uint16 casts clamp instead of wrapping, the landscape's outermost ring sheds nothing,
// Runtime/Engine/ Public/Raster.h (FTriangleRasterizer), adapted: the ramp's two points come from palette
// commands, not a hit proxy; UObject/ULandscapeEditorObject settings become plain structs, the stroke writes
// through LandscapeHeightCache (L2) on the global sample lattice, kissfft is replaced by a separable DFT, the clay
// brush, tablet pressure, the flatten target and edit layers are not ported, and the stroke records its own
// before/after snapshot for undo.
// Mirror and Copy/Paste: see LandscapeComponentTools.cpp for their UE sources.

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

    /// UE's Ramp_bRaiseTerrain / Ramp_bLowerTerrain as one choice. UE 5.8 hard-codes both on (the settings are
    /// commented out in ApplyRamp); the raster policy still honours each flag, and Both is that default.
    enum class LandscapeRampMode : uint8_t
    {
        Both,
        Raise,
        Lower,
    };

    struct LandscapeRampSettings
    {
        /// UE: RampWidth, world units (cm), ClampMin 1, default 2000.
        float WidthCm = 2000.0f;
        /// UE: RampSideFalloff, the fraction of the half width that blends into the terrain, 0..1, default 0.4.
        float             SideFalloff = 0.4f;
        LandscapeRampMode Mode        = LandscapeRampMode::Both;
    };

    inline constexpr float kLandscapeMinRampWidthCm   = 1.0f;
    inline constexpr float kLandscapeMaxRampWidthUiCm = 8192.0f; // UE's UIMax

    Common::BoolResultStr ValidateLandscapeRamp( const LandscapeRampSettings& settings );

    /// UE's ELandscapeToolErosionMode; the stroke casts it to the noise mode as UE does (Raise = Add,
    /// Lower = Sub).
    enum class LandscapeErosionNoiseMode : uint8_t
    {
        Both,
        Raise,
        Lower,
    };

    /**
     * @brief UE's Erosion tool settings. Not ported: ErodeSurfaceThickness and bErosionUseLayerHardness — UE
     * reads them only to move paint-layer weights and to soften by layer hardness, and the heightmap target has
     * no paint layers; a setting that could not change a height would be a dead one.
     */
    struct LandscapeErosionSettings
    {
        /// UE: ErodeThresh, height steps, ClampMin 0, ClampMax 256, default 64.
        int32_t Threshold = 64;
        /// UE: ErodeIterationNum, ClampMin 1, ClampMax 300, default 28.
        int32_t Iterations = 28;
        /// UE: ErosionNoiseMode, default Lower (the constructor's value).
        LandscapeErosionNoiseMode NoiseMode = LandscapeErosionNoiseMode::Lower;
        /// UE: ErosionNoiseScale, samples per noise period, ClampMin 1, ClampMax 512, default 60.
        float NoiseScale = 60.0f;
    };

    inline constexpr int32_t kLandscapeMaxErosionThreshold  = 256;
    inline constexpr int32_t kLandscapeMaxErosionIterations = 300;

    Common::BoolResultStr ValidateLandscapeErosion( const LandscapeErosionSettings& settings );

    /// UE's ELandscapeToolHydroErosionMode: Both rains where the noise is positive, Positive everywhere.
    enum class LandscapeRainMode : uint8_t
    {
        Both,
        Positive,
    };

    struct LandscapeHydroErosionSettings
    {
        /// UE: RainAmount, water steps per rained sample, ClampMin 1, ClampMax 512, default 128.
        int32_t RainAmount = 128;
        /// UE: SedimentCapacity, ClampMin 0.1, ClampMax 1, default 0.3.
        float SedimentCapacity = 0.3f;
        /// UE: HErodeIterationNum, ClampMin 1, ClampMax 300, default 75.
        int32_t Iterations = 75;
        /// UE: RainDistMode, default Both.
        LandscapeRainMode RainMode = LandscapeRainMode::Both;
        /// UE: RainDistScale, samples per noise period, ClampMin 1, ClampMax 512, default 60.
        float RainScale = 60.0f;
        /// UE: bHErosionDetailSmooth, default on.
        bool DetailSmooth = true;
        /// UE: HErosionDetailScale, ClampMin 0, ClampMax 0.99, default 0.01.
        float DetailScale = 0.01f;
    };

    inline constexpr int32_t kLandscapeMaxRainAmount       = 512;
    inline constexpr float   kLandscapeMinSedimentCapacity = 0.1f;
    inline constexpr float   kLandscapeMaxHydroDetailScale = 0.99f;

    Common::BoolResultStr ValidateLandscapeHydroErosion( const LandscapeHydroErosionSettings& settings );

    /**
     * @brief The heights one erosion step works on: @p Heights over @p Rect (UE's brush bounds grown by one),
     * and UE's BrushValue over @p Inner, both row-major, X fastest. Inner lies inside Rect shrunk by one, so
     * every neighbour the simulation reads is cached (UE's cache reads 0 past the landscape; ours has no such
     * sample, so the landscape's outermost ring sheds nothing; it only receives, as UE's ring does).
     */
    struct LandscapeErosionField
    {
        LandscapeSampleBounds Rect;
        std::vector<uint16_t> Heights;
        LandscapeSampleBounds Inner;
        std::vector<float>    Brush;
    };

    /**
     * @brief FLandscapeToolStrokeErosion::Apply's thermal loop, without the noise pass: every sample steeper
     * than the threshold towards a lower 4-neighbour sheds height to it, until an iteration changes nothing.
     * @return the number of iterations run.
     */
    int32_t LandscapeThermalErosion( LandscapeErosionField& field, const LandscapeErosionSettings& settings,
                                     float strength );

    /// The noise pass that closes FLandscapeToolStrokeErosion::Apply (amplitude BrushValue · Threshold ·
    /// strength · BrushSizeAdjust).
    void LandscapeErosionNoise( LandscapeErosionField& field, const LandscapeErosionSettings& settings,
                                float strength, float radiusCm );

    /**
     * @brief FLandscapeToolStrokeHydraErosion::Apply: rain by noise where the brush weighs 1, then per iteration
     * dissolve, flow over the 8 neighbours by water-surface altitude, evaporate half and deposit what exceeds
     * the capacity, until no water is left; then the detail smooth. Position-seeded (the noise), scan order
     * Z then X as UE's Y then X — no random state.
     * @return the number of iterations run.
     */
    int32_t LandscapeHydraulicErosion( LandscapeErosionField& field, const LandscapeHydroErosionSettings& settings,
                                       float strength );

    /**
     * @brief UE's SculptStrength for one step, in height steps at brush weight 1:
     *        Strength · (RadiusCm · 128 / ZScale) · min(dt, 0.1) · 3, and at least 1 (UE's non-clay floor).
     *        0 when the product is not positive — UE returns without writing.
     */
    /// UE's ELandscapeMirrorOperation. UE's Y is our Z; the Rotate variants also flip the other axis.
    enum class LandscapeMirrorOp : uint8_t
    {
        MinusXToPlusX,
        PlusXToMinusX,
        MinusZToPlusZ,
        PlusZToMinusZ,
        RotateMinusXToPlusX,
        RotatePlusXToMinusX,
        RotateMinusZToPlusZ,
        RotatePlusZToMinusZ,
    };

    /// UE's MirrorSmoothingWidth: UIMax 20, clamped to 0..32768 when applied.
    inline constexpr int32_t kLandscapeMaxMirrorSmoothingUi = 20;
    inline constexpr int32_t kLandscapeMaxMirrorSmoothing   = 32768;

    /// UE's mirror settings (MirrorOp, MirrorSmoothingWidth) with UE's defaults.
    struct LandscapeMirrorSettings
    {
        LandscapeMirrorOp Op = LandscapeMirrorOp::MinusXToPlusX;
        /// Samples either side of the mirror line blended by a cosine.
        int32_t SmoothingWidth = 0;
    };

    /// UE's ELandscapeToolPasteMode: Raise only lifts samples, Lower only sinks them.
    enum class LandscapePasteMode : uint8_t
    {
        Both,
        Raise,
        Lower,
    };

    /**
     * UE's gizmo SelectedData for a rectangular selection: heights of SizeX x SizeZ samples (row-major, X fastest)
     * stored RELATIVE to the region's centre sample (SizeX / 2, SizeZ / 2), exactly, in sample steps.
     */
    struct LandscapeCopyBuffer
    {
        int32_t              SizeX = 0;
        int32_t              SizeZ = 0;
        std::vector<int32_t> Relative;

        bool Empty() const
        {
            return Relative.empty();
        }
    };

    /// UE's Copy tool over the rectangle spanned by two world points (both corners inclusive, clipped to @p
    /// bounds).
    Common::ResultStr<LandscapeCopyBuffer> CopyLandscapeHeights( const LandscapeRoot&         root,
                                                                 LandscapeTileLookup          lookup,
                                                                 const LandscapeSampleBounds& bounds,
                                                                 glm::vec3 cornerACm, glm::vec3 cornerBCm );

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

        /**
         * FLandscapeToolRamp::ApplyRamp: a ramp from @p startCm to @p endCm (world cm; Y is the height the ramp
         * takes at that end), @p ramp.WidthCm wide, its sides blended into the terrain by a cosine over the outer
         * SideFalloff of the half width. Refuses coincident points (UE's CanApplyRamp wants two distinct points
         * to have a direction at all).
         */
        Common::BoolResultStr ApplyRamp( glm::vec3 startCm, glm::vec3 endCm, const LandscapeRampSettings& ramp );

        /// FLandscapeToolStrokeErosion::Apply for the heightmap target: the thermal loop, then the noise pass.
        Common::BoolResultStr ApplyErosion( const LandscapeBrushWeights&    weights,
                                            const LandscapeBrushSettings&   brush,
                                            const LandscapeErosionSettings& erosion );

        /// FLandscapeToolStrokeHydraErosion::Apply for the heightmap target.
        Common::BoolResultStr ApplyHydroErosion( const LandscapeBrushWeights&         weights,
                                                 const LandscapeBrushSettings&        brush,
                                                 const LandscapeHydroErosionSettings& hydro );

        /**
         * FLandscapeToolMirror::ApplyMirror for the heightmap: one side of the line through @p pointCm (world
         * cm; only the op's axis is read) is copied mirrored onto the other, the SmoothingWidth samples either
         * side of the line blended by a cosine. No point: the landscape's centre (UE's CenterMirrorPoint).
         * Refuses a line on or outside the landscape's edge.
         */
        Common::BoolResultStr ApplyMirror( std::optional<glm::vec3>       pointCm,
                                           const LandscapeMirrorSettings& mirror );

        /**
         * FLandscapeToolStrokePaste for the heightmap with a gizmo dropped at @p atCm: the buffer's centre sample
         * lands on the lattice sample nearest @p atCm at that sample's height, the rest keep their relative
         * heights. Refuses an empty buffer and a paste centre outside the landscape.
         */
        Common::BoolResultStr ApplyPaste( const LandscapeCopyBuffer& buffer, glm::vec3 atCm,
                                          LandscapePasteMode mode, const LandscapeBrushSettings& brush );

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
        /// Caches and reads one erosion step's rectangle; an empty Brush means there is nothing to erode.
        Common::ResultStr<LandscapeErosionField> ErosionField( const LandscapeBrushWeights&  weights,
                                                               const LandscapeBrushSettings& brush );
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
