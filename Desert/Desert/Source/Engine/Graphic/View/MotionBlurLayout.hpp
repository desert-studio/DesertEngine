#pragma once
// THE PARAMETERS EVERY MOTION BLUR PASS READS, declared once: Tools/ShaderProtocolGen generates
// Generated/Post/MotionBlurParams.glslh (`struct MotionBlurParams` + DESERT_MotionBlurParams_MEMBERS) from it,
// and each of the four Programs/MotionBlur shaders declares its MotionBlurBuffer (std430) at its own binding
// with those members. ClipToPrevClip lets Flatten derive the camera motion of a pixel nothing wrote velocity
// for (Common/Velocity.glslh DesertCameraVelocityAtInfinity), so a turning camera blurs the sky with the
// world. Layout only: MakeMotionBlurParams (MotionBlur.hpp) fills it.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

// X( C++ type, member, "GLSL name", initialiser )
namespace Desert::Graphic
{
    // A field type with a template comma cannot pass through X( ... ): named here.
    using MotionBlurPad = std::array<float, 3>;
} // namespace Desert::Graphic

#define DESERT_MOTION_BLUR_PARAMS_FIELDS( X )                                                                        \
    X( glm::vec2, RenderSize, "RenderSize", glm::vec2( 0.0f ) ) /* pixels: velocity, depth, flattened velocity */    \
    X( glm::vec2, OutputSize, "OutputSize", glm::vec2( 0.0f ) ) /* pixels: resolved colour, output */                \
    X( glm::vec2, TileCount, "TileCount", glm::vec2( 0.0f ) ) /* tiles */                                            \
    X( float, VelocityScale, "VelocityScale", 0.0f ) /* MotionBlurVelocityScale */                                   \
    X( float, MaxPixels, "MaxPixels", 0.0f ) /* MotionBlurMaxPixels */                                               \
    X( int32_t, Samples, "Samples", 0 )                                                                              \
    X( MotionBlurPad, Pad, "Pad", {} )                                                                        \
    X( glm::mat4, ClipToPrevClip, "ClipToPrevClip", glm::mat4( 1.0f ) ) /* ViewFrame::ClipToPrevClip (unjittered) */

namespace Desert::Graphic
{
    DESERT_SHADER_STRUCT( MotionBlurParams, "MotionBlurParams", Std430, DESERT_MOTION_BLUR_PARAMS_FIELDS );

    inline constexpr const ShaderProtocols::ShaderStructDecl* kMotionBlurParamsStructs[] = { &kMotionBlurParamsStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kMotionBlurParamsShaderInclude{
         .Path         = "Generated/Post/MotionBlurParams.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/View/MotionBlurLayout.hpp",
         .Structs      = kMotionBlurParamsStructs };
} // namespace Desert::Graphic
