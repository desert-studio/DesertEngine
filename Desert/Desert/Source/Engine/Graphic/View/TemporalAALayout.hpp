#pragma once
// THE TAA RESOLVE'S PARAMETERS, declared once: Tools/ShaderProtocolGen generates
// Generated/Post/TemporalAAParams.glslh (`struct TemporalAAParams` + DESERT_TemporalAAParams_MEMBERS) from it,
// and Programs/TemporalAA/TemporalAA.shader declares its TemporalAABuffer (std430) with those members. A buffer
// and not a push constant: two matrices are already the 128 bytes Vulkan guarantees for push constants. Layout
// only: MakeTemporalAAParams (TemporalAA.hpp) fills it.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>

// X( C++ type, member, "GLSL name", initialiser )
namespace Desert::Graphic
{
    // A field type with a template comma cannot pass through X( ... ): named here.
    using TemporalAAPad = std::array<float, 3>;
} // namespace Desert::Graphic

#define DESERT_TEMPORAL_AA_PARAMS_FIELDS( X )                                                                        \
    X( glm::mat4, InvViewProjection, "InverseViewProjection", glm::mat4( 1.0f ) ) /* this frame, unjittered: output pixel uv (+ device depth) -> world */ \
    X( glm::mat4, PrevViewProjection, "PreviousViewProjection", glm::mat4( 1.0f ) ) /* previous frame, unjittered: world -> previous clip */ \
    X( glm::vec2, RenderSize, "RenderSize", glm::vec2( 0.0f ) ) /* pixels */                                         \
    X( glm::vec2, OutputSize, "OutputSize", glm::vec2( 0.0f ) ) /* pixels */                                         \
    X( glm::vec2, JitterUv, "JitterUv", glm::vec2( 0.0f ) ) /* this frame's jitter in uv: JitterNdc * (0.5, -0.5) */ \
    X( float, HistoryValid, "HistoryValid", 0.0f ) /* 1 when the history holds this view's previous frame */         \
    X( float, HistoryPreExposureCorrection, "HistoryPreExposureCorrection", 1.0f ) /* ViewFrame::PreExposure / PrevPreExposure (UE HistoryPreExposureCorrection) */ \
    X( float, PreExposure, "PreExposure", 1.0f ) /* ViewFrame::PreExposure: the scene colour arrives x this */       \
    X( TemporalAAPad, Pad, "Pad", {} )

namespace Desert::Graphic
{
    DESERT_SHADER_STRUCT( TemporalAAParams, "TemporalAAParams", Std430, DESERT_TEMPORAL_AA_PARAMS_FIELDS );

    inline constexpr const ShaderProtocols::ShaderStructDecl* kTemporalAAParamsStructs[] = { &kTemporalAAParamsStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kTemporalAAParamsShaderInclude{
         .Path         = "Generated/Post/TemporalAAParams.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/View/TemporalAALayout.hpp",
         .Structs      = kTemporalAAParamsStructs };
} // namespace Desert::Graphic
