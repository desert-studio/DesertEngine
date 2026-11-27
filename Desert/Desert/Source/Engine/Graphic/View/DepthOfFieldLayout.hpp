#pragma once
// THE PARAMETERS EVERY DOF PASS READS, declared once: Tools/ShaderProtocolGen generates
// Generated/Post/DofParams.glslh (`struct DofParams` + DESERT_DofParams_MEMBERS) from it, and each of the six
// Programs/DepthOfField shaders declares its DepthOfFieldBuffer (std430) at its own binding with those members.
// Layout only: MakeDofParams (DepthOfField.hpp) fills it.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

// X( C++ type, member, "GLSL name", initialiser )
namespace Desert::Graphic
{
    // A field type with a template comma cannot pass through X( ... ): named here.
    using DofPad = std::array<float, 2>;
} // namespace Desert::Graphic

#define DESERT_DOF_PARAMS_FIELDS( X )                                                                                \
    X( glm::vec2, OutputSize, "OutputSize", glm::vec2( 0.0f ) )                                                      \
    X( glm::vec2, RenderSize, "RenderSize", glm::vec2( 0.0f ) )                                                      \
    X( glm::vec2, HalfSize, "HalfSize", glm::vec2( 0.0f ) )                                                          \
    X( glm::vec2, TileCount, "TileCount", glm::vec2( 0.0f ) )                                                        \
    X( glm::vec4, DepthToView, "DepthToView", glm::vec4( 0.0f ) ) /* DofDepthToView */                               \
    X( float, FocalDistanceMm, "FocalDistanceMm", 0.0f ) /* s */                                                     \
    X( float, FocalLengthMm, "FocalLengthMm", 0.0f ) /* f */                                                         \
    X( float, CocScale, "CocScale", 0.0f ) /* A f / 2 * OutputWidth / SensorWidth: output-pixel radius numerator */  \
    X( float, MaxRadius, "MaxRadius", 0.0f ) /* output pixels */                                                     \
    X( int32_t, Rings, "Rings", 0 )                                                                                  \
    X( int32_t, DilateTiles, "DilateTiles", 0 )                                                                      \
    X( DofPad, Pad, "Pad", {} )

namespace Desert::Graphic
{
    DESERT_SHADER_STRUCT( DofParams, "DofParams", Std430, DESERT_DOF_PARAMS_FIELDS );

    inline constexpr const ShaderProtocols::ShaderStructDecl* kDofParamsStructs[] = { &kDofParamsStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kDofParamsShaderInclude{
         .Path         = "Generated/Post/DofParams.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/View/DepthOfFieldLayout.hpp",
         .Structs      = kDofParamsStructs };
} // namespace Desert::Graphic
