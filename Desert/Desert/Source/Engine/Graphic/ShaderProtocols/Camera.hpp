#pragma once

#include <Engine/Graphic/ShaderProtocols/Std140Block.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

// THE MEMBERS OF THE CAMERA BLOCK, listed ONCE. They expand into ShaderProtocols::Camera's members and into
// kCameraFields, from which the includer GENERATES Common/CameraUB.glslh (there is no such file on disk). The
// order is the std140 order: mat4 at 64-byte steps, vec2s together, each vec3 completed by a float, the explicit
// tail pad; the static_assert below makes any other order a build error.
//
// It is THE view block (UE's FViewUniformShaderParameters): one per view per frame, the shader view of the
// per-frame ViewFrame (TAA1 step 3). WHICH MATRIX (ViewFrame.hpp): raster geometry -> JitteredViewProjection;
// world position from depth -> InvJitteredViewProjection; reprojection -> PrevViewProjection vs ViewProjection
// (both unjittered); Projection / View stay the camera's own, unjittered.
//
// FIELD( C++ type, member, GLSL name, initialiser ) — the GLSL type is the C++ type's Std140Type.
#define DESERT_CAMERA_UB_FIELDS( FIELD )                                                                          \
    FIELD( glm::mat4, Projection, Projection, { 1.0f } )                               /* offset   0 */           \
    FIELD( glm::mat4, View, View, { 1.0f } )                                           /* offset  64 */           \
    FIELD( glm::mat4, JitteredViewProjection, JitteredViewProjection, { 1.0f } )       /* offset 128 */           \
    FIELD( glm::mat4, ViewProjection, ViewProjection, { 1.0f } )                       /* offset 192 */           \
    FIELD( glm::mat4, PrevViewProjection, PrevViewProjection, { 1.0f } )               /* offset 256 */           \
    FIELD( glm::mat4, InvJitteredViewProjection, InvJitteredViewProjection, { 1.0f } ) /* offset 320 */           \
    FIELD( glm::vec2, JitterNdc, JitterNdc, { 0.0f } )                                 /* offset 384 */           \
    FIELD( glm::vec2, PrevJitterNdc, PrevJitterNdc, { 0.0f } )                         /* offset 392 */           \
    FIELD( glm::vec3, CameraPos, CameraPos, { 0.0f } )                                 /* offset 400, world cm */ \
    FIELD( float, Time, Time, { 0.0f } )                       /* offset 412, seconds (ViewFrame::TimeSeconds) */ \
    FIELD( glm::vec3, PrevCameraPos, PrevCameraPos, { 0.0f } ) /* offset 416 */                                   \
    FIELD( float, PrevTime, PrevTime, { 0.0f } )               /* offset 428 */                                   \
    FIELD( float, MaterialMipBias, MaterialMipBias, { 0.0f } ) /* offset 432 */                                   \
    /* light views only: the caster's slope depth bias per unit tan (Common/ShadowCasterBias.glslh) */            \
    FIELD( float, ShadowSlopeBias, ShadowSlopeBias, { 0.0f } ) /* offset 436 */                                   \
    /* light views only: where the slope term stops */                                                            \
    FIELD( float, ShadowSlopeClamp, ShadowSlopeClamp, { 0.0f } ) /* offset 440 */                                 \
    /* Parallax occlusion (Mesh/Surface/SurfaceParallax.glslh): the machine's step counts (Scalability group */   \
    /* Shading) and the main directional light's direction TOWARD the sun (zero when the scene has none) */       \
    /* for the relief's self-shadow. Every view carries the steps (a light view marches the relief too). */       \
    FIELD( float, ParallaxMinSteps, ParallaxMinSteps, { 0.0f } )       /* offset 444 (ViewFrame::Parallax) */     \
    FIELD( glm::vec3, SunDirection, SunDirection, { 0.0f } )           /* offset 448 */                           \
    FIELD( float, ParallaxMaxSteps, ParallaxMaxSteps, { 0.0f } )       /* offset 460 */                           \
    FIELD( float, ParallaxShadowSteps, ParallaxShadowSteps, { 0.0f } ) /* offset 464 */                           \
    /* every HDR scene-colour write is multiplied by PreExposure (UE View.PreExposure); a history stored last */  \
    /* frame is rescaled by PreExposure / PrevPreExposure */                                                      \
    FIELD( float, PreExposure, PreExposure, { 1.0f } )         /* offset 468, ViewFrame::PreExposure */           \
    FIELD( float, PrevPreExposure, PrevPreExposure, { 1.0f } ) /* offset 472, ViewFrame::PrevPreExposure */       \
    FIELD( float, CameraPad2, CameraPad2, { 0.0f } )           /* offset 476; block size 480 */

namespace Desert::Graphic::ShaderProtocols
{
    /// Camera and kCameraFields — the block's std140 fields, what Common/CameraUB.glslh is generated from
    /// (Std140Block.hpp).
    DESERT_SHADER_STRUCT( Camera, DESERT_CAMERA_UB_FIELDS, inline const static std::string Name = "CameraUB"; )
    static_assert( Std140LayoutMatches( kCameraFields, sizeof( Camera ) ),
                   "ShaderProtocols::Camera is not laid out as std140 places the generated CameraUB block: "
                   "reorder or pad DESERT_CAMERA_UB_FIELDS" );
    static_assert( sizeof( Camera ) == 480 );

    /// The include path the shader includer answers with the generated block (angle include, shader-root relative).
    inline constexpr std::string_view kCameraUBInclude = "Common/CameraUB.glslh";

    /// The GLSL of the camera block — generated once per process from kCameraFields.
    [[nodiscard]] inline const std::string& CameraUBGlsl()
    {
        static const std::string glsl = GenerateUniformBlockGlsl(
             "DESERT_CAMERA_UB_GLSLH", "Engine/Graphic/ShaderProtocols/Camera.hpp", 0, Camera::Name, "cameraUB",
             kCameraFields );
        return glsl;
    }

    // THE one writer of the camera block: every material that declares CameraUB is filled from the frame's
    // ViewFrame through this, never field by field from a Core::Camera.
    [[nodiscard]] inline Camera MakeCameraUB( const ViewFrame& frame )
    {
        Camera ub;
        ub.Projection                = frame.Projection;
        ub.View                      = frame.View;
        ub.JitteredViewProjection    = frame.JitteredViewProjection;
        ub.ViewProjection            = frame.ViewProjection;
        ub.PrevViewProjection        = frame.PrevViewProjection;
        ub.InvJitteredViewProjection = frame.InvJitteredViewProjection;
        ub.JitterNdc                 = frame.JitterNdc;
        ub.PrevJitterNdc             = frame.PrevJitterNdc;
        ub.CameraPos                 = frame.CameraPosition;
        ub.Time                      = static_cast<float>( frame.TimeSeconds );
        ub.PrevCameraPos             = frame.PrevCameraPosition;
        ub.PrevTime                  = static_cast<float>( frame.PrevTimeSeconds );
        ub.MaterialMipBias           = frame.MaterialMipBias;
        ub.ShadowSlopeBias           = frame.ShadowSlopeBias;
        ub.ShadowSlopeClamp          = frame.ShadowSlopeClamp;
        ub.ParallaxMinSteps          = static_cast<float>( frame.Parallax.MinSteps );
        ub.ParallaxMaxSteps          = static_cast<float>( frame.Parallax.MaxSteps );
        ub.ParallaxShadowSteps       = static_cast<float>( frame.Parallax.ShadowSteps );
        ub.SunDirection              = frame.SunDirection;
        ub.PreExposure               = frame.PreExposure;
        ub.PrevPreExposure           = frame.PrevPreExposure;
        return ub;
    }
} // namespace Desert::Graphic::ShaderProtocols
