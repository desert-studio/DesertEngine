#pragma once

#include <Engine/Core/PostProcessSettings.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // One range of UE's colour grading (FColorGradePerRangeSettings), as Common/FilmToneMap.glslh's
    // FilmColorRange.
    struct FilmColorRange
    {
        glm::vec4 Saturation;
        glm::vec4 Contrast;
        glm::vec4 Gamma;
        glm::vec4 Gain;
        glm::vec4 Offset;

        bool operator==( const FilmColorRange& ) const = default;
    };

    // THE WHOLE GRADE OF A VIEW AS THE GPU READS IT: the C++ side of FilmToneMap.glslh's FilmGradeParams, member
    // for member. The CombineLUTs bake uploads it as that struct (std430 of vec4 members only, so the two layouts
    // are 25 packed vec4s and nothing else), and the editor's HDR thumbnail compiles FilmToneMap.glslh against
    // this very type, so FilmCombine is fed by one packing, FilmGradeOf, and nowhere else.
    struct FilmGradeParams
    {
        glm::vec4      Curve;        // FilmSlope, FilmToe, FilmShoulder, FilmBlackClip
        glm::vec4      Look;         // FilmWhiteClip, BlueCorrection, ExpandGamut, ToneCurveAmount
        glm::vec4      WhiteBalance; // WhiteTemp, WhiteTint, z = 1 when not the identity (6500 / 0)
        glm::vec4      Output;       // Gamma (UE DisplayGamma), operator (0 Filmic, 1 Reinhard), WhitePoint
        glm::vec4      Ranges;       // ColorCorrectionShadowsMax, HighlightsMin, HighlightsMax
        FilmColorRange Global;
        FilmColorRange Shadows;
        FilmColorRange Midtones;
        FilmColorRange Highlights;

        bool operator==( const FilmGradeParams& ) const = default;
    };
    static_assert( sizeof( FilmGradeParams ) == 25 * sizeof( glm::vec4 ),
                   "FilmGradeParams must stay 25 packed vec4s: FilmToneMap.glslh reads it as std430" );

    // The packing of a view's grade (Core::PostProcessSettings) into FilmGradeParams.
    inline FilmGradeParams FilmGradeOf( const Core::PostProcessSettings& grade )
    {
        const bool  identityWhite = Core::IsIdentityWhiteBalance( grade.WhiteTemp, grade.WhiteTint );
        const float tonemapper    = static_cast<float>( static_cast<int>( grade.Tonemapper ) );
        return FilmGradeParams{
             .Curve = { grade.FilmSlope, grade.FilmToe, grade.FilmShoulder, grade.FilmBlackClip },
             .Look  = { grade.FilmWhiteClip, grade.BlueCorrection, grade.ExpandGamut, grade.ToneCurveAmount },
             .WhiteBalance = { grade.WhiteTemp, grade.WhiteTint, identityWhite ? 0.0f : 1.0f, 0.0f },
             .Output       = { grade.Gamma, tonemapper, grade.WhitePoint, 0.0f },
             .Ranges       = { grade.ColorCorrectionShadowsMax, grade.ColorCorrectionHighlightsMin,
                               grade.ColorCorrectionHighlightsMax, 0.0f },
             .Global       = { .Saturation = grade.ColorSaturation,
                               .Contrast   = grade.ColorContrast,
                               .Gamma      = grade.ColorGamma,
                               .Gain       = grade.ColorGain,
                               .Offset     = grade.ColorOffset },
             .Shadows      = { .Saturation = grade.ColorSaturationShadows,
                               .Contrast   = grade.ColorContrastShadows,
                               .Gamma      = grade.ColorGammaShadows,
                               .Gain       = grade.ColorGainShadows,
                               .Offset     = grade.ColorOffsetShadows },
             .Midtones     = { .Saturation = grade.ColorSaturationMidtones,
                               .Contrast   = grade.ColorContrastMidtones,
                               .Gamma      = grade.ColorGammaMidtones,
                               .Gain       = grade.ColorGainMidtones,
                               .Offset     = grade.ColorOffsetMidtones },
             .Highlights   = { .Saturation = grade.ColorSaturationHighlights,
                               .Contrast   = grade.ColorContrastHighlights,
                               .Gamma      = grade.ColorGammaHighlights,
                               .Gain       = grade.ColorGainHighlights,
                               .Offset     = grade.ColorOffsetHighlights } };
    }
} // namespace Desert::Graphic
