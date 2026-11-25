#pragma once

// The landscape's sample encoding and a tile's placement (LandscapeFrame): what a consumer that places,
// raycasts or lays out tiles names WITHOUT the tile data (LandscapeData.hpp owns LandscapeTileData).

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>

namespace Desert::World::Landscape
{
    // ── The encoding ──────────────────────────────────────────────────────────────────────────────────
    //
    // UE:Engine/Source/Runtime/Landscape/Public/LandscapeDataAccess.h:13-14
    //     #define LANDSCAPE_ZSCALE (1.0f/128.0f)      #define LANDSCAPE_INV_ZSCALE 128.0f
    // UE:Engine/Source/Runtime/Landscape/Public/LandscapeDataAccess.h:26-27
    //     MaxValue = 65535;  MidValue = 32768.f;
    // UE:Engine/Source/Runtime/Landscape/Public/LandscapeDataAccess.h:29-37
    //     GetLocalHeight(h)  = (h - MidValue) * LANDSCAPE_ZSCALE
    //     GetTexHeight(z)    = uint16(RoundToInt(Clamp(z * LANDSCAPE_INV_ZSCALE + MidValue, 0, MaxValue)))
    //
    // "Local" height is in the landscape's own space; the actor's Z scale multiplies it into the world.
    // UE's world unit is the centimetre and so is ours, so the conversion to centimetres is the Z scale
    // itself and nothing else: heightCm = (sample - 32768) / 128 * ZScale. With UE's default Z scale of
    // 100 that is a range of -256 m .. +255.99 m in steps of 100/128 = 0.78125 cm.

    inline constexpr uint16_t kLandscapeMaxSample = 65535u;
    inline constexpr uint16_t kLandscapeMidSample = 32768u;

    /// Local units per sample step. UE:LandscapeDataAccess.h:13 (LANDSCAPE_ZSCALE).
    inline constexpr float kLandscapeLocalPerStep = 1.0f / 128.0f;

    /// Samples per local unit. UE:LandscapeDataAccess.h:14 (LANDSCAPE_INV_ZSCALE).
    inline constexpr float kLandscapeStepsPerLocal = 128.0f;

    /// The vertical scale a new landscape gets: centimetres per local unit. UE's default actor scale is
    /// (100, 100, 100); the Z component of it is this number.
    inline constexpr float kLandscapeDefaultZScale = 100.0f;

    /// The horizontal spacing a new landscape gets: centimetres between two neighbouring samples. UE's
    /// default actor X/Y scale of 100 on a one-unit quad grid is one metre.
    inline constexpr float kLandscapeDefaultSpacingCm = 100.0f;

    /// Height in the landscape's local space. UE:LandscapeDataAccess.h:29-32 (GetLocalHeight).
    /// Defined by Shaders/Common/LandscapeHeight.glslh — the text the terrain shader decodes its R16 copy
    /// with — so it is not constexpr: the formula has one home, and it is the one the GPU compiles.
    float LandscapeLocalHeight( uint16_t sample );

    /**
     * @brief The sample for a local height. UE:LandscapeDataAccess.h:34-37 (GetTexHeight).
     *
     * Clamps THEN rounds half-up, in that order, as UE does. The clamp is spelled as UE's FMath::Clamp
     * spells it — `x < lo ? lo : (x < hi ? x : hi)` — rather than std::clamp, because the two differ on
     * NaN: std::clamp hands NaN through to the integer cast, which is undefined behaviour; UE's form maps
     * NaN to the maximum, which is defined and is what a heightmap exported from UE would contain.
     */
    uint16_t LandscapeSampleFromLocal( float localHeight );

    /// World-space height offset in centimetres of a sample, for a landscape of vertical scale @p zScale.
    /// Same home as LandscapeLocalHeight.
    float LandscapeHeightCm( uint16_t sample, float zScale );

    /// The sample for a height offset in centimetres. @p zScale must be positive; the caller that owns
    /// the scale (the landscape root) is the one that validates it, see ValidateLandscapeFrame.
    uint16_t LandscapeSampleFromHeightCm( float heightCm, float zScale );

    // ── Placement ─────────────────────────────────────────────────────────────────────────────────────

    /**
     * @brief Where one tile's sample grid sits in the world. Y is up; samples lie on the XZ plane.
     *
     * Sample (0, 0) is at (OriginX, BaseY + height, OriginZ); sample (x, z) is SpacingCm * (x, z) from it.
     * Supplied by the owner of the tile rather than stored in it — see the file comment.
     */
    struct LandscapeFrame
    {
        float OriginX   = 0.0f;
        float OriginZ   = 0.0f;
        float BaseY     = 0.0f;
        float SpacingCm = kLandscapeDefaultSpacingCm;
        float ZScale    = kLandscapeDefaultZScale;
    };

    /// Refuses a frame the sampling maths cannot honour (non-positive or non-finite spacing or scale),
    /// naming the offending number.
    Common::BoolResultStr ValidateLandscapeFrame( const LandscapeFrame& frame );
} // namespace Desert::World::Landscape
