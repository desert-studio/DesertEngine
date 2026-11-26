#pragma once

#include <Engine/Core/PostProcessSettings.hpp>
#include <Engine/Graphic/PostProcessing/FilmGradeLayout.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // The packing of a view's grade (Core::PostProcessSettings) into FilmGradeParams.
    // @p headroom: the output's peak in paper-white units the table is baked for (1 = SDR, the HDR output's
    // Graphic::CurrentDisplayOutput headroom otherwise).
    inline FilmGradeParams FilmGradeOf( const Core::PostProcessSettings& grade, float headroom )
    {
        const bool  identityWhite = Core::IsIdentityWhiteBalance( grade.WhiteTemp, grade.WhiteTint );
        const float tonemapper    = static_cast<float>( static_cast<int>( grade.Tonemapper ) );
        return FilmGradeParams{
             .Curve = { grade.FilmSlope, grade.FilmToe, grade.FilmShoulder, grade.FilmBlackClip },
             .Look  = { grade.FilmWhiteClip, grade.BlueCorrection, grade.ExpandGamut, grade.ToneCurveAmount },
             .WhiteBalance = { grade.WhiteTemp, grade.WhiteTint, identityWhite ? 0.0f : 1.0f, 0.0f },
             .Output       = { headroom, tonemapper, grade.WhitePoint, 0.0f },
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
