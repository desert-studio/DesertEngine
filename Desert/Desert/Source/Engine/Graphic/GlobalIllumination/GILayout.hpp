#pragma once

// THE GI PASSES' GPU RECORDS, declared once: the frame record every GI pass binds, the software tracer's record,
// the world radiance cache's parameters and probe slot, the ReSTIR gather's and the screen probe gather's
// parameters and probe. Tools/ShaderProtocolGen writes them as Generated/GI/*.glslh, which the Common/*.glslh of
// each pass include.

#include <Engine/Graphic/DistanceField/DistanceFieldLayout.hpp>
#include <Engine/Graphic/GlobalIllumination/RadianceCacheCeiling.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderLayout.hpp>
#include <Engine/Graphic/ShaderProtocols/ShaderProtocol.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace Desert::Graphic::GlobalIllumination
{
    // THE GI FRAME RECORD - what every GI pass knows of the frame, neutral to the tracer. Uploaded once per frame
    // by the frame's owner and bound as "GIFrameUB" by the radiance cache's passes and the screen probe gather.
#define DESERT_GI_FRAME_PAYLOAD_FIELDS( X )                                                                       \
    /* the jittered matrix the G-buffer was rasterised with, and its inverse */                                   \
    X( glm::mat4, ViewProjection, "viewProjection", glm::mat4( 1.0f ) )                                           \
    X( glm::mat4, InvViewProjection, "invViewProjection", glm::mat4( 1.0f ) )                                     \
    X( glm::vec4, Camera, "camera", glm::vec4( 0.0f ) ) /* xyz = camera world position (cm) */                    \
    /* xyz = direction the sunlight travels, w = 1 when there is a sun */                                         \
    X( glm::vec4, SunTravel, "sunTravel", glm::vec4( 0.0f ) )                                                     \
    /* rgb = colour * intensity, w = 1 when it casts shadows */                                                   \
    X( glm::vec4, SunRadiance, "sunRadiance", glm::vec4( 0.0f ) )                                                 \
    /* atmosphere light 1 (the moon), lit by every GI pass as the sun is: xyz = travel, w = 1 when there is one   \
     */                                                                                                           \
    X( glm::vec4, Light1Travel, "light1Travel", glm::vec4( 0.0f ) )                                               \
    X( glm::vec4, Light1Radiance, "light1Radiance", glm::vec4( 0.0f ) ) /* rgb = colour * intensity */            \
    /* the sky look (Graphic::SkyLookGPU) */                                                                      \
    X( glm::vec4, SkyYawCosSin, "skyYawCosSin", glm::vec4( 1.0f, 0.0f, 0.0f, 0.0f ) )                             \
    X( glm::vec4, SkyGain, "skyGain", glm::vec4( 1.0f ) )                                                         \
    X( glm::vec4, Screen, "screen", glm::vec4( 0.0f ) ) /* xy = G-buffer extent (pixels), zw = 1 / that */
    DESERT_SHADER_STRUCT( GIFramePayload, "GIFrame", Std430, DESERT_GI_FRAME_PAYLOAD_FIELDS );
#undef DESERT_GI_FRAME_PAYLOAD_FIELDS

    // "GITracerSoftwareUB" (Common/GITracer.glslh): what the software tracer's walks read of the distance fields.
    using GdfLevelRows       = std::array<glm::vec4, 4>;
    using GdfOccupiedBoxRows = std::array<glm::vec4, 2>;
#define DESERT_TRACER_SOFTWARE_PAYLOAD_FIELDS( X )                                                                \
    X( GdfLevelRows, GlobalDistanceFieldLevels, "giTracerGdfLevels", {} )                                         \
    /* MeshDistanceFieldReads::NearFieldGridPlacement */                                                          \
    X( DistanceField::MeshSDFGridPlacement, MeshSDFGrid, "giTracerMeshSDFGrid", {} )                              \
    /* GlobalDistanceField::FrameField::OccupiedBox, (min.xyz, 0) and (max.xyz, 0) cm: where a far-field march    \
       ends (GIFarFieldOccupiedExit); min > max: nothing is placed */                                             \
    X( GdfOccupiedBoxRows, GlobalDistanceFieldOccupiedBox, "giTracerGdfOccupiedBox", {} )                         \
    /* GlobalDistanceField::FrameField::Ceiling: where a far-field march ends past the last column it is not      \
       above (GIFarFieldCeilingCut); Grid.zw = 0 (nothing placed) leaves the march uncut */                       \
    X( DistanceField::GlobalDistanceFieldCeiling, GlobalDistanceFieldCeiling, "giTracerGdfCeiling", {} )
    DESERT_SHADER_STRUCT( TracerSoftwarePayload, "GITracerSoftware", Std430,
                          DESERT_TRACER_SOFTWARE_PAYLOAD_FIELDS );
#undef DESERT_TRACER_SOFTWARE_PAYLOAD_FIELDS

    // The ReSTIR gather's parameters (ReSTIRGather; Common/ReSTIRGI.glslh `restirParams`).
#define DESERT_RESTIR_PARAMS_PAYLOAD_FIELDS( X )                                                                  \
    /* xy = reservoir extent, z = spatial samples, w = 0 (unused) */                                              \
    X( glm::uvec4, Extent, "extent", glm::uvec4( 0u ) )                                                           \
    X( glm::uvec4, Frame, "frame", glm::uvec4( 0u ) ) /* x = frame index */                                       \
    /* x = temporal M cap, y = spatial radius (reservoir texels), z = depth tolerance, w = normal cos */          \
    X( glm::vec4, Tuning, "tuning", glm::vec4( 0.0f ) )                                                           \
    X( glm::vec4, Jitter, "jitter", glm::vec4( 0.0f ) )     /* xy = this frame's NDC jitter, zw = last frame's */ \
    X( glm::vec4, RayClamp, "rayClamp", glm::vec4( 0.0f ) ) /* x = the largest radiance a ray carries (abs) */    \
    /* x = relative luminance change, y = relative hit distance change, z = luminance floor (abs) */              \
    X( glm::vec4, Validation, "validation", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( ReSTIRParamsPayload, "ReSTIRParams", Std430, DESERT_RESTIR_PARAMS_PAYLOAD_FIELDS );
#undef DESERT_RESTIR_PARAMS_PAYLOAD_FIELDS

    // The screen probe gather's parameters (ScreenProbeGather; Common/ScreenProbes.glslh).
#define DESERT_SCREEN_PROBE_PARAMS_PAYLOAD_FIELDS( X )                                                            \
    /* last frame's (unjittered) view-projection: the screen trace's reprojection; its inverse: the temporal      \
       filter's previous world position of a pixel */                                                             \
    X( glm::mat4, PrevViewProjection, "prevViewProjection", glm::mat4( 1.0f ) )                                   \
    X( glm::mat4, PrevInvViewProjection, "prevInvViewProjection", glm::mat4( 1.0f ) )                             \
    /* xyz = last frame's camera (cm), w = 1 / PrevPreExposure (the scene colour history's scale) */              \
    X( glm::vec4, PrevCamera, "prevCamera", glm::vec4( 0.0f ) )                                                   \
    X( glm::vec4, Jitter, "jitter", glm::vec4( 0.0f ) ) /* xy = this frame's jitter in NDC, zw = last frame's */  \
    /* xy = level-0 grid, z = uniform probes, w = adaptive probe budget */                                        \
    X( glm::uvec4, Grid, "grid", glm::uvec4( 0u ) )                                                               \
    /* x = probes per atlas row, y = frame index, z = kScreenProbe* flags (SP_FLAG_*), w = the frame's probe      \
       jitter in pixels (x | y << 16, ScreenProbeJitter) */                                                       \
    X( glm::uvec4, Atlas, "atlas", glm::uvec4( 0u ) )                                                             \
    /* x = the largest component a traced ray's radiance may carry, absolute (kScreenProbeMaxRayIntensity /       \
       PreExposure); yzw unused */                                                                                \
    X( glm::vec4, RayClamp, "rayClamp", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( ScreenProbeParamsPayload, "ScreenProbeParams", Std430,
                          DESERT_SCREEN_PROBE_PARAMS_PAYLOAD_FIELDS );
#undef DESERT_SCREEN_PROBE_PARAMS_PAYLOAD_FIELDS

    // One screen probe.
#define DESERT_SCREEN_PROBE_PAYLOAD_FIELDS( X )                                                                   \
    /* xyz = world (cm), w = distance to the camera (cm); 0 = no probe */                                         \
    X( glm::vec4, Position, "position", glm::vec4( 0.0f ) )                                                       \
    /* xyz = the G-buffer normal at the probe's pixel, w = that pixel (SPPackPixel) */                            \
    X( glm::vec4, Normal, "normal", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( ScreenProbePayload, "ScreenProbe", Std430, DESERT_SCREEN_PROBE_PAYLOAD_FIELDS );
#undef DESERT_SCREEN_PROBE_PAYLOAD_FIELDS
} // namespace Desert::Graphic::GlobalIllumination

namespace Desert::Graphic::RadianceCache
{
    // The world radiance cache's parameters (Common/RadianceCache.glslh).
    using ClipmapCornerRows = std::array<glm::vec4, kMaxClipmaps>;
    using ClipmapTMinRows   = std::array<glm::vec4, 2>;
#define DESERT_RADIANCE_CACHE_PARAMS_PAYLOAD_FIELDS( X )                                                          \
    /* xyz = corner (world, cm), w = cell size (cm) */                                                            \
    X( ClipmapCornerRows, ClipmapCornerAndCellSize, "clipmapCornerAndCellSize", {} )                              \
    /* per clipmap, the distance a probe's rays skip before they may hit, cm (UE RadianceProbeSettings.x,         \
       GetRadianceProbeTMin): [0].xyzw = clipmaps 0..3, [1].xy = 4..5 */                                          \
    X( ClipmapTMinRows, ClipmapProbeTMin, "clipmapProbeTMin", {} )                                                \
    /* x = clipmaps, y = grid resolution, z = traced probe resolution, w = final probe resolution (with border)   \
     */                                                                                                           \
    X( glm::uvec4, Shape, "shape", glm::uvec4( 0u ) )                                                             \
    /* x = probe atlas resolution in probes, y = irradiance probe resolution, z = probes per frame,               \
       w = distance-moment probe resolution */                                                                    \
    X( glm::uvec4, Atlas, "atlas", glm::uvec4( 0u ) )                                                             \
    /* x = reprojection radius scale, y = 1 / clipmap fade size (cells), z = 1 / final radiance atlas texels */   \
    X( glm::vec4, Sampling, "sampling", glm::vec4( 0.0f ) )
    DESERT_SHADER_STRUCT( RadianceCacheParamsPayload, "RadianceCacheParams", Std430,
                          DESERT_RADIANCE_CACHE_PARAMS_PAYLOAD_FIELDS );
#undef DESERT_RADIANCE_CACHE_PARAMS_PAYLOAD_FIELDS

    // One probe slot's state.
#define DESERT_RADIANCE_CACHE_PROBE_PAYLOAD_FIELDS( X )                                                           \
    /* xyz = world cell (cells of its clipmap from the world origin), w = clipmap */                              \
    X( glm::ivec4, Cell, "cell", glm::ivec4( 0 ) )                                                                \
    /* x = last used, y = last traced (0 = never), z = traced light hash, w = light hash */                       \
    X( glm::uvec4, Frames, "frames", glm::uvec4( 0u ) )                                                           \
    X( glm::vec4, Offset, "offset", glm::vec4( 0.0f ) ) /* xyz = relocation out of geometry (cm), w = 0 */        \
    /* x = priority bucket this frame, y = 1: a coverage reader stamped it this frame (0: the fallback only) */   \
    X( glm::uvec4, Priority, "priority", glm::uvec4( 0u ) )
    DESERT_SHADER_STRUCT( RadianceCacheProbePayload, "RadianceCacheProbe", Std430,
                          DESERT_RADIANCE_CACHE_PROBE_PAYLOAD_FIELDS );
#undef DESERT_RADIANCE_CACHE_PROBE_PAYLOAD_FIELDS
} // namespace Desert::Graphic::RadianceCache

namespace Desert::Graphic::GlobalIllumination
{
    inline constexpr const ShaderProtocols::ShaderStructDecl* kGIFrameStructs[] = { &kGIFramePayloadStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl       kGIFrameShaderInclude{
               .Path         = "Generated/GI/GIFrame.glslh",
               .SourceHeader = "Desert/Desert/Source/Engine/Graphic/GlobalIllumination/GILayout.hpp",
               .Structs      = kGIFrameStructs,
    };

    inline constexpr std::string_view kGITracerRequires[] = { DistanceField::kDistanceFieldShaderInclude.Path };
    inline constexpr const ShaderProtocols::ShaderStructDecl* kGITracerStructs[] = {
         &kTracerSoftwarePayloadStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kGITracerShaderInclude{
         .Path         = "Generated/GI/GITracer.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/GlobalIllumination/GILayout.hpp",
         .Requires     = kGITracerRequires,
         .Structs      = kGITracerStructs,
    };

    inline constexpr const ShaderProtocols::ShaderStructDecl* kRadianceCacheStructs[] = {
         &RadianceCache::kRadianceCacheParamsPayloadStruct,
         &RadianceCache::kRadianceCacheProbePayloadStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kRadianceCacheShaderInclude{
         .Path         = "Generated/GI/RadianceCache.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/GlobalIllumination/GILayout.hpp",
         .Structs      = kRadianceCacheStructs,
    };

    inline constexpr const ShaderProtocols::ShaderStructDecl* kReSTIRStructs[] = { &kReSTIRParamsPayloadStruct };
    inline constexpr ShaderProtocols::ShaderIncludeDecl       kReSTIRShaderInclude{
               .Path         = "Generated/GI/ReSTIR.glslh",
               .SourceHeader = "Desert/Desert/Source/Engine/Graphic/GlobalIllumination/GILayout.hpp",
               .Structs      = kReSTIRStructs,
    };

    inline constexpr const ShaderProtocols::ShaderStructDecl* kScreenProbesStructs[] = {
         &kScreenProbeParamsPayloadStruct,
         &kScreenProbePayloadStruct,
    };
    inline constexpr ShaderProtocols::ShaderIncludeDecl kScreenProbesShaderInclude{
         .Path         = "Generated/GI/ScreenProbes.glslh",
         .SourceHeader = "Desert/Desert/Source/Engine/Graphic/GlobalIllumination/GILayout.hpp",
         .Structs      = kScreenProbesStructs,
    };
} // namespace Desert::Graphic::GlobalIllumination
