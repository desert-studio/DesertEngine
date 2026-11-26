#pragma once
// THE TSR PASS CHAIN'S SHARED PARAMETERS, declared once: Tools/ShaderProtocolGen generates
// Generated/Post/TsrParams.glslh (`struct TsrParams` + DESERT_TsrParams_MEMBERS) from it, and
// Common/TSRCommon.glslh declares TSRParamsBuffer (std430, binding 0 of every TSR shader) with those members.
// The history counts are UE TemporalSuperResolution.cpp :3220-3226 (r.TSR.History.SampleCount 16,
// r.TSR.ShadingRejection.SampleCount 2, r.TSR.Velocity.WeightClampingSampleCount 4,
// r.TSR.Velocity.WeightClampingPixelSpeed 1) at this history / output ratio. One render pixel at view depth d
// spans d * DepthPixelRadiusScale + DepthPixelRadiusBias world units (UE GetDepthPixelRadiusForProjectionType).
// ClipToResurrectionClip is this frame's unjittered clip -> the resurrection frame's clip (UE
// ClipToResurrectionClip). Layout only: MakeTsrParams (TemporalSuperResolution.hpp) fills it.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>


// X( C++ type, member, "GLSL name", initialiser )
#define DESERT_TSR_PARAMS_FIELDS( X )                                                                                \
    X( glm::mat4, InvViewProjection, "InverseViewProjection", glm::mat4( 1.0f ) ) /* this frame, unjittered: uv (+ device depth) -> world */ \
    X( glm::mat4, PrevViewProjection, "PreviousViewProjection", glm::mat4( 1.0f ) ) /* previous frame, unjittered: world -> previous clip */ \
    X( glm::mat4, PrevInvProjection, "PreviousInverseProjection", glm::mat4( 1.0f ) ) /* previous frame, unjittered: previous device depth -> view depth */ \
    X( glm::vec2, RenderSize, "RenderSize", glm::vec2( 0.0f ) ) /* pixels */                                         \
    X( glm::vec2, HistorySize, "HistorySize", glm::vec2( 0.0f ) ) /* pixels */                                       \
    X( glm::vec2, OutputSize, "OutputSize", glm::vec2( 0.0f ) ) /* pixels */                                         \
    X( glm::vec2, JitterUv, "JitterUv", glm::vec2( 0.0f ) ) /* this frame's jitter in uv: JitterNdc * (0.5, -0.5) */ \
    X( float, HistoryValid, "HistoryValid", 0.0f )                                                                   \
    X( float, HistorySampleCount, "HistorySampleCount", 16.0f ) /* UE TemporalSuperResolution.cpp :3220-3226 */      \
    X( float, HistoryHisteresis, "HistoryHysteresis", 1.0f / 16.0f )                                                 \
    X( float, WeightClampingRejection, "WeightClampingRejection", 0.0f )                                             \
    X( float, WeightClampingPixelSpeedAmplitude, "WeightClampingPixelSpeedAmplitude", 0.0f )                         \
    X( float, InvWeightClampingPixelSpeed, "InverseWeightClampingPixelSpeed", 1.0f )                                 \
    X( float, InputToHistoryFactor, "InputToHistoryFactor", 1.0f ) /* History.Width / Render.Width */                \
    X( float, FlickeringFramePeriod, "FlickeringFramePeriod", 2.0f ) /* UE r.TSR.ShadingRejection.Flickering.Period */ \
    X( float, DepthPixelRadiusScale, "DepthPixelRadiusScale", 0.0f )                                                 \
    X( float, DepthPixelRadiusBias, "DepthPixelRadiusBias", 0.0f )                                                   \
    X( float, EnableFlickeringHeuristic, "EnableFlickeringHeuristic", 1.0f ) /* TsrQuality::ShadingRejectionFlickering (UE: a uniform) */ \
    X( float, ReprojectionField, "ReprojectionField", 1.0f ) /* TsrQuality::ReprojectionField (UE bReprojectionField) */ \
    X( glm::mat4, InvProjection, "InverseProjection", glm::mat4( 1.0f ) ) /* this frame, unjittered: device depth -> view */ \
    X( glm::mat4, Projection, "Projection", glm::mat4( 1.0f ) ) /* this frame, unjittered: view -> clip */           \
    X( glm::mat4, ClipToResurrectionClip, "ClipToResurrectionClip", glm::mat4( 1.0f ) ) /* identity while ResurrectionEnabled is 0 */ \
    X( float, ReprojectionFieldAntiAliasVelocityThreshold, "ReprojectionFieldAntiAliasVelocityThreshold", 0.0f ) /* UE :2531 */ \
    X( float, ResurrectionEnabled, "ResurrectionEnabled", 0.0f ) /* a persistent frame is resurrectable */           \
    X( float, ThinGeometryDetection, "ThinGeometryDetection", 0.0f ) /* TsrQuality::ThinGeometryDetection */         \
    X( float, ThinGeometryFrameIndex, "ThinGeometryFrameIndex", 0.0f ) /* TsrHistory::AccumulatedFrames mod 64 */    \
    X( float, PreExposure, "PreExposure", 1.0f ) /* ViewFrame::PreExposure */                                        \
    X( float, HistoryPreExposureCorrection, "HistoryPreExposureCorrection", 1.0f ) /* PreExposure / PrevPreExposure */ \
    X( float, ResurrectionPreExposureCorrection, "ResurrectionPreExposureCorrection", 1.0f ) /* PreExposure / the resurrected frame's; 1 without resurrection */ \
    X( float, PreExposurePad, "PreExposurePad", 0.0f )

namespace Desert::Graphic
{
    DESERT_SHADER_STRUCT( TsrParams, "TsrParams", Std430, DESERT_TSR_PARAMS_FIELDS );

    inline constexpr const ShaderProtocols::ShaderStructDecl* kTsrParamsStructs[] = { &kTsrParamsStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kTsrParamsShaderInclude{
         .Path         = "Generated/Post/TsrParams.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/View/TemporalSuperResolutionLayout.hpp",
         .Structs      = kTsrParamsStructs };
} // namespace Desert::Graphic
