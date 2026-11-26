#pragma once
// THE WHOLE GRADE OF A VIEW AS THE GPU READS IT, declared once: UE's colour grading per range
// (FColorGradePerRangeSettings, FilmColorRange) and everything else CombineLUTs is parameterised by
// (FilmGradeParams). Tools/ShaderProtocolGen generates Generated/Post/FilmGradeParams.glslh (both structs) from
// it, included by Common/FilmToneMap.glslh; the bake uploads FilmGradeParams as that struct (std430), and the
// editor's HDR thumbnail compiles FilmToneMap.glslh as C++ against this very type. Layout only: FilmGradeOf
// (FilmGrade.hpp) is the one packing.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>


// X( C++ type, member, "GLSL name", initialiser )
#define DESERT_FILM_COLOR_RANGE_FIELDS( X )                                                                          \
    X( glm::vec4, Saturation, "Saturation", glm::vec4( 0.0f ) )                                                      \
    X( glm::vec4, Contrast, "Contrast", glm::vec4( 0.0f ) )                                                          \
    X( glm::vec4, Gamma, "Gamma", glm::vec4( 0.0f ) )                                                                \
    X( glm::vec4, Gain, "Gain", glm::vec4( 0.0f ) )                                                                  \
    X( glm::vec4, Offset, "Offset", glm::vec4( 0.0f ) )

#define DESERT_FILM_GRADE_PARAMS_FIELDS( X )                                                                         \
    X( glm::vec4, Curve, "Curve", glm::vec4( 0.0f ) ) /* FilmSlope, FilmToe, FilmShoulder, FilmBlackClip */          \
    X( glm::vec4, Look, "Look", glm::vec4( 0.0f ) ) /* FilmWhiteClip, BlueCorrection, ExpandGamut, ToneCurveAmount */ \
    X( glm::vec4, WhiteBalance, "WhiteBalance", glm::vec4( 0.0f ) ) /* WhiteTemp, WhiteTint, z = 1 when not the identity (6500 / 0) */ \
    X( glm::vec4, Output, "Output", glm::vec4( 0.0f ) ) /* headroom (1 = SDR), Core::TonemapOperator, WhitePoint (Reinhard) */ \
    X( glm::vec4, Ranges, "Ranges", glm::vec4( 0.0f ) ) /* ColorCorrectionShadowsMax, HighlightsMin, HighlightsMax */ \
    X( FilmColorRange, Global, "Global", {} )                                                                        \
    X( FilmColorRange, Shadows, "Shadows", {} )                                                                      \
    X( FilmColorRange, Midtones, "Midtones", {} )                                                                    \
    X( FilmColorRange, Highlights, "Highlights", {} )

namespace Desert::Graphic
{
    DESERT_SHADER_STRUCT( FilmColorRange, "FilmColorRange", Std430, DESERT_FILM_COLOR_RANGE_FIELDS,
                          bool operator==( const FilmColorRange& ) const = default; );
    DESERT_SHADER_STRUCT( FilmGradeParams, "FilmGradeParams", Std430, DESERT_FILM_GRADE_PARAMS_FIELDS,
                          bool operator==( const FilmGradeParams& ) const = default; );

    inline constexpr const ShaderProtocols::ShaderStructDecl* kFilmGradeStructs[] = { &kFilmColorRangeStruct,
                                                                                    &kFilmGradeParamsStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kFilmGradeShaderInclude{
         .Path         = "Generated/Post/FilmGradeParams.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/PostProcessing/FilmGradeLayout.hpp",
         .Structs      = kFilmGradeStructs };
} // namespace Desert::Graphic
