#pragma once
// THE CAMERA BLOCK'S GPU LAYOUT, declared once: Tools/ShaderProtocolGen generates Common/CameraUB.glslh from it at
// build time (kCameraUBShaderInclude). Layout only: the writer (MakeCameraUB) and the RDG parameter live in
// Camera.hpp, which needs the ViewFrame.
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>

// The members of the camera block. The order is the std140 order: mat4 at 64-byte steps, vec2s together, each
// vec3 completed by a float; DESERT_SHADER_STRUCT's static_assert makes any other order a build error.
//
// It is THE view block (UE's FViewUniformShaderParameters): one per view per frame, the shader view of the
// per-frame ViewFrame (TAA1 step 3), for the raster AND for ray tracing: every ray generation that traces into
// hit groups binds it (PathTracer "PathTracing: Trace", SSR "Deferred: Reflections (HitLighting)"), so a hit cell
// that includes Common/CameraUB.glslh reads the program's own binding (VulkanRayTracingPipeline: a hit cell reads
// only what the program binds); the landscape's hit reads CameraPos and GrassLens here (GPU-COST-25), and so does
// the landscape's raster with its matrices and PreExposure - it keeps no copy of the view (GPU-COST-26).
// WHICH MATRIX (ViewFrame.hpp): raster geometry -> JitteredViewProjection;
// world position from depth -> InvJitteredViewProjection; reprojection -> PrevViewProjection vs ViewProjection
// (both unjittered); Projection / View stay the camera's own, unjittered.
//
// X( C++ type, member, "GLSL name", initialiser )
#define DESERT_CAMERA_UB_FIELDS( X )                                                                              \
    X( glm::mat4, Projection, "Projection", glm::mat4( 1.0f ) )                               /* offset   0 */    \
    X( glm::mat4, View, "View", glm::mat4( 1.0f ) )                                           /* offset  64 */    \
    X( glm::mat4, JitteredViewProjection, "JitteredViewProjection", glm::mat4( 1.0f ) )       /* offset 128 */    \
    X( glm::mat4, ViewProjection, "ViewProjection", glm::mat4( 1.0f ) )                       /* offset 192 */    \
    X( glm::mat4, PrevViewProjection, "PrevViewProjection", glm::mat4( 1.0f ) )               /* offset 256 */    \
    X( glm::mat4, InvJitteredViewProjection, "InvJitteredViewProjection", glm::mat4( 1.0f ) ) /* offset 320 */    \
    X( glm::vec2, JitterNdc, "JitterNdc", {} )                                                /* offset 384 */    \
    X( glm::vec2, PrevJitterNdc, "PrevJitterNdc", {} )                                        /* offset 392 */    \
    X( glm::vec3, CameraPos, "CameraPos", {} )           /* offset 400, world cm */                               \
    X( float, Time, "Time", 0.0f )                       /* offset 412, seconds (ViewFrame::TimeSeconds) */       \
    X( glm::vec3, PrevCameraPos, "PrevCameraPos", {} )   /* offset 416 */                                         \
    X( float, PrevTime, "PrevTime", 0.0f )               /* offset 428 */                                         \
    X( float, MaterialMipBias, "MaterialMipBias", 0.0f ) /* offset 432 */                                         \
    /* light views only: the caster's slope depth bias per unit tan (Common/ShadowCasterBias.glslh) */            \
    X( float, ShadowSlopeBias, "ShadowSlopeBias", 0.0f ) /* offset 436 */                                         \
    /* light views only: where the slope term stops */                                                            \
    X( float, ShadowSlopeClamp, "ShadowSlopeClamp", 0.0f ) /* offset 440 */                                       \
    /* Parallax occlusion (Mesh/Surface/SurfaceParallax.glslh): the machine's step counts (Scalability group */   \
    /* Shading) and the main directional light's direction TOWARD the sun (zero when the scene has none) */       \
    /* for the relief's self-shadow. Every view carries the steps (a light view marches the relief too). */       \
    X( float, ParallaxMinSteps, "ParallaxMinSteps", 0.0f )       /* offset 444 (ViewFrame::Parallax) */           \
    X( glm::vec3, SunDirection, "SunDirection", {} )             /* offset 448 */                                 \
    X( float, ParallaxMaxSteps, "ParallaxMaxSteps", 0.0f )       /* offset 460 */                                 \
    X( float, ParallaxShadowSteps, "ParallaxShadowSteps", 0.0f ) /* offset 464 */                                 \
    /* every HDR scene-colour write is multiplied by PreExposure (UE View.PreExposure); a history stored last */  \
    /* frame is rescaled by PreExposure / PrevPreExposure */                                                      \
    X( float, PreExposure, "PreExposure", 1.0f )         /* offset 468, ViewFrame::PreExposure */                 \
    X( float, PrevPreExposure, "PrevPreExposure", 1.0f ) /* offset 472, ViewFrame::PrevPreExposure */             \
    /* UE View.StateFrameIndexMod8: the view's frame count mod 8, the temporal step of a dithered opacity mask */ \
    X( float, FrameIndexMod8, "FrameIndexMod8", 0.0f ) /* offset 476 */                                           \
    /* light views only: the shadow wind rule's measure (ViewFrame::CasterWindOrigin) */                          \
    X( glm::vec4, CasterWindOrigin, "CasterWindOrigin", {} ) /* offset 480 */                                     \
    /* scene views only: the view's grass field lens (ViewFrame::GrassLens, x DistanceScale, y */                 \
    /* PixelsPerUnit) the landscape's far grass cover is laid through, by the raster and a ray hit alike */       \
    X( glm::vec4, GrassLens, "GrassLens", {} ) /* offset 496 */                                                   \
    /* light views only: the shadow wind rule's caster ring (ViewFrame::CasterWindRing, x the inner distance; */  \
    /* y, z, w unused) */                                                                                         \
    X( glm::vec4, CasterWindRing, "CasterWindRing", {} ) /* offset 512 */                                         \
    /* UE SvPositionToScreenPosition: a fragment's gl_FragCoord.xy to its JITTERED NDC through the view's */      \
    /* raster viewport (ViewFrame::RasterRect), ndc = xy * PixelToNdc.xy + PixelToNdc.zw; the G-buffer cell's */  \
    /* clip position (Mesh/Surface/Pass_GBuffer.glslh DesertGBufferClipFromPixel). 0 for a view with no rect */   \
    X( glm::vec4, PixelToNdc, "PixelToNdc", {} ) /* offset 528 (MakePixelToNdc) */                                \
    /* scene views: the dithered LOD transition (ViewFrame::TemporalLOD; Mesh/Surface/LODTransition.glslh): */    \
    /* x how far the fade has come (0..1), y 1 while the view fades its level changes; a cascade's light */       \
    /* view carries its scene view's (WriteLightCamera): its casters clip the view's dither */                    \
    X( glm::vec4, LodTransition, "LodTransition", {} ) /* offset 544 */                                           \
    /* every view: the gust fronts the foliage sway is scaled by (ViewFrame::WindGust, Common/WindGust.glslh: */  \
    /* xy the fronts' offset, gust cells; z 1 / GustSize; w Gustiness, 0 = none), now and one step before */      \
    X( glm::vec4, WindGust, "WindGust", {} )         /* offset 560 */                                             \
    X( glm::vec4, PrevWindGust, "PrevWindGust", {} ) /* offset 576 */                                             \
    /* every view: the local wind sources nearest the main camera (ViewFrame::LocalWind, */                       \
    /* Graphic::SelectLocalWind, Common/LocalWind.glslh: [i] = position, 1 / Radius; the speeds four to a */      \
    /* vec4, in reference-speed units), now and one step before */                                                \
    X( CameraLocalWindSources, LocalWind, "LocalWind", {} )                  /* offset 592, vec4[8] */            \
    X( CameraLocalWindSpeeds, LocalWindSpeed, "LocalWindSpeed", {} )         /* offset 720, vec4[2] */            \
    X( CameraLocalWindSources, PrevLocalWind, "PrevLocalWind", {} )          /* offset 752 */                     \
    X( CameraLocalWindSpeeds, PrevLocalWindSpeed, "PrevLocalWindSpeed", {} ) /* offset 880; block size 912 */

namespace Desert::Graphic::ShaderProtocols
{
    // The local wind arrays (a field's type is one macro argument: no comma). Camera.hpp checks that they are the
    // ViewFrame's LocalWindSources arrays (kMaxLocalWindSources sources, four speeds to a vec4).
    using CameraLocalWindSources = std::array<glm::vec4, 8>;
    using CameraLocalWindSpeeds  = std::array<glm::vec4, 2>;

    DESERT_SHADER_STRUCT( Camera, "CameraUB", Std140, DESERT_CAMERA_UB_FIELDS );
    static_assert( sizeof( Camera ) == 912 );

    // The block: binding 0 of set 0, held apart from the other scene protocols' slots (the generator refuses two
    // Fixed scene blocks on one number; IncludesScene.cpp checks the lit programs' own slots).
    inline constexpr ShaderBlockDecl        kCameraUBBlocks[] = { { .Kind = ShaderBlockKind::Uniform,
                                                                    .Binding = { ShaderBindingKind::Fixed, 0u, {} },
                                                                    .BlockName    = "CameraUB",
                                                                    .InstanceName = "cameraUB",
                                                                    .Members      = &kCameraStruct } };
    inline constexpr const ShaderBlockDecl& kCameraUBBlock    = kCameraUBBlocks[0];

    inline constexpr ShaderIncludeDecl kCameraUBShaderInclude{
         .Path         = "Common/CameraUB.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/ShaderProtocols/CameraLayout.hpp",
         .Blocks       = kCameraUBBlocks };
} // namespace Desert::Graphic::ShaderProtocols
