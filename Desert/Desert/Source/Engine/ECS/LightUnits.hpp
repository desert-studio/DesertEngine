#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Desert::ECS
{
    // Distance falloff model for a point / spot light. Reflected enum - drives a combo in the editor and
    // round-trips through reflected serialization (as its number). Matches LightFalloffFactor in
    // Shaders/Mesh/BRDF.glslh.
    enum class LightFalloff
    {
        Linear,
        Quadratic,
        InverseSquare
    };

    // UE ELightUnits: what a point / spot light's Intensity is measured in. Reflected enum, stored as its number.
    //   Unitless  - the renderer's own multiplier: the radiance at the falloff's reference distance (Min Radius,
    //               or kInverseSquareReferenceFloorCm) is Color x Intensity. Every light authored before light
    //               units existed is Unitless (scene v56 states it), so its picture did not move.
    //   Candelas  - luminous intensity, cd. 1 cd lights a surface 1 m away with 1 lux.
    //   Lumens    - luminous flux, lm, spread over the light's solid angle: 4 pi for a point light,
    //               2 pi (1 - cos OuterCone) for a spot light (UE USpotLightComponent).
    //   EV100     - exposure value at ISO 100 of the candela value: cd = 2^(EV - 3) (K = 12.5, the reflected-light
    //               meter constant; the same law Graphic::EV100ToLuminance applies to the exposure range).
    // The physical units are only defined for InverseSquare falloff - Linear and Quadratic are artistic curves
    // without a distance law, so there the value is Unitless whatever the field says (UE greys the field when
    // bUseInverseSquaredFalloff is off).
    enum class LightUnits
    {
        Unitless,
        Candelas,
        Lumens,
        EV100
    };

    namespace LightPhotometry
    {
        // BRDF.glslh LightFalloffFactor normalises InverseSquare to 1 at max(minRadius, 0.05 cm) - this is that
        // 0.05. The shader's literal and this constant are one fact; change both or neither.
        inline constexpr float kInverseSquareReferenceFloorCm = 0.05f;

        // World units are centimetres: E = I / d^2 with d in cm gives lux when I is cd x (100 cm/m)^2.
        inline constexpr float kSquareCentimetresPerSquareMetre = 100.0f * 100.0f;

        // A point light radiates over the whole sphere: the cone whose half-angle cosine is -1.
        inline constexpr float kPointLightCosHalfCone = -1.0f;

        // The short label a readout prints after an Intensity ("" for Unitless).
        inline const char* UnitsLabel( LightUnits units )
        {
            switch ( units )
            {
                case LightUnits::Candelas:
                    return "cd";
                case LightUnits::Lumens:
                    return "lm";
                case LightUnits::EV100:
                    return "EV100";
                case LightUnits::Unitless:
                    break;
            }
            return "";
        }

        inline float InverseSquareReferenceCm( float minRadiusCm )
        {
            return std::max( minRadiusCm, kInverseSquareReferenceFloorCm );
        }

        // Solid angle of a cone of half-angle acos(cosHalfCone), steradians (4 pi for a point light).
        inline float ConeSolidAngle( float cosHalfCone )
        {
            // UE clamps the same way: a zero-width cone has no solid angle to spread lumens over.
            const float c = std::clamp( cosHalfCone, -1.0f, 1.0f - 1e-4f );
            return 2.0f * std::numbers::pi_v<float> * ( 1.0f - c );
        }

        // A physical value (Candelas / Lumens / EV100) as candelas. Unitless has no candela value on its own.
        inline float ToCandelas( float value, LightUnits units, float cosHalfCone )
        {
            switch ( units )
            {
                case LightUnits::Lumens:
                    return value / ConeSolidAngle( cosHalfCone );
                case LightUnits::EV100:
                    return std::exp2( value - 3.0f );
                case LightUnits::Candelas:
                case LightUnits::Unitless:
                    break;
            }
            return value;
        }

        inline float FromCandelas( float candelas, LightUnits units, float cosHalfCone )
        {
            switch ( units )
            {
                case LightUnits::Lumens:
                    return candelas * ConeSolidAngle( cosHalfCone );
                case LightUnits::EV100:
                    return std::log2( std::max( candelas, 1e-30f ) ) + 3.0f;
                case LightUnits::Candelas:
                case LightUnits::Unitless:
                    break;
            }
            return candelas;
        }

        // THE ONE CONVERSION the renderer applies: the authored Intensity in its units as the multiplier the light
        // payload carries (radiance at the reference distance, the number LightFalloffFactor scales). For
        // InverseSquare, E(d) = cd x 1e4 / d^2 lux beyond the reference distance and flat inside it - the
        // physical law with the emitter's size as the clamp, as UE's SourceRadius-bounded inverse square.
        inline float ShaderIntensity( float intensity, LightUnits units, LightFalloff falloff, float minRadiusCm,
                                      float cosHalfCone )
        {
            if ( units == LightUnits::Unitless || falloff != LightFalloff::InverseSquare )
                return intensity;
            const float reference = InverseSquareReferenceCm( minRadiusCm );
            return ToCandelas( intensity, units, cosHalfCone ) * kSquareCentimetresPerSquareMetre /
                   ( reference * reference );
        }

        // UE ULocalLightComponent::PostEditChangeProperty on IntensityUnits: the value re-stated in the new units
        // so the light keeps its brightness. Outside InverseSquare the units do not apply and the value stays.
        inline float ConvertIntensity( float intensity, LightUnits from, LightUnits to, LightFalloff falloff,
                                       float minRadiusCm, float cosHalfCone )
        {
            if ( from == to || falloff != LightFalloff::InverseSquare )
                return intensity;
            const float shader = ShaderIntensity( intensity, from, falloff, minRadiusCm, cosHalfCone );
            if ( to == LightUnits::Unitless )
                return shader;
            const float reference = InverseSquareReferenceCm( minRadiusCm );
            return FromCandelas( shader * reference * reference / kSquareCentimetresPerSquareMetre, to,
                                 cosHalfCone );
        }
    } // namespace LightPhotometry
} // namespace Desert::ECS
