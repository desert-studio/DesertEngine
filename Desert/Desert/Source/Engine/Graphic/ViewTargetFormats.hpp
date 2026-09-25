#pragma once

#include <Engine/Core/Formats/ImageFormat.hpp>

namespace Desert::Graphic::ViewTargetFormats
{
    /**
     * @brief THE FORMAT OF EVERY TARGET A VIEW HOLDS — one spelling, read by the creator AND by the census.
     *
     * WHY ONE PLACE. The view-memory census (ViewMemory.hpp) listed each target's format as a mirror of the
     * file that allocated it, twelve files each spelling its own ImageFormat literal. A format change then
     * had to be made twice, and a census row that disagreed with its creator was a lie nothing caught.
     * Every creator reads its constant from here and the census reads the same constant, so changing a
     * format is one line and the pinned totals in Desert/Tests/Engine/ViewMemory move with it.
     *
     * This header only depends on ImageFormat so that ShadowCascades.hpp (which ViewMemory.hpp itself
     * includes) can derive its per-texel shadow budget from the same two constants.
     */
    using Core::Formats::ImageFormat;

    // SceneRenderer::EnsureRendererResources.
    inline constexpr ImageFormat kSceneColor      = ImageFormat::RGBA16F;
    inline constexpr ImageFormat kSceneDepth      = ImageFormat::DEPTH32F;
    inline constexpr ImageFormat kSceneColorCopy  = kSceneColor; // snapshot of the scene target, same texel
    inline constexpr ImageFormat kGBufferA        = ImageFormat::RGBA8F;  // albedo.rgb + metallic
    inline constexpr ImageFormat kGBufferB        = ImageFormat::RGBA16F; // normal.xyz + roughness
    inline constexpr ImageFormat kGBufferC        = ImageFormat::RGBA32F; // world position.xyz
    inline constexpr ImageFormat kGBufferEmissive = ImageFormat::RGBA16F;
    inline constexpr ImageFormat kGBufferDepth    = ImageFormat::DEPTH32F;
    inline constexpr ImageFormat kSSAO            = ImageFormat::RGBA8F;

    // Post stack. A target written by a compute shader as a storage image (bloom, light shafts, lens flare,
    // height fog, clouds) also spells its format in the shader's image qualifier (rgba32f / rgba16f): change
    // the two together.
    inline constexpr ImageFormat kSilhouetteMask = ImageFormat::RGBA8F;
    // Seeds are PIXEL COORDINATES (JFA_Init.shader: o_Seed = vec4(pixelCoord, ...)): half floats stop being
    // exact integers above 2048 px, so 16F would misplace the outline on a wide window. Keep 32F.
    inline constexpr ImageFormat kJFASeed        = ImageFormat::RGBA32F;
    inline constexpr ImageFormat kTonemap        = ImageFormat::RGBA8F;
    inline constexpr ImageFormat kFXAA           = ImageFormat::RGBA8F;
    inline constexpr ImageFormat kSMAAEdges      = ImageFormat::RGBA8F;
    inline constexpr ImageFormat kSMAABlend      = ImageFormat::RGBA8F;
    inline constexpr ImageFormat kBloom          = ImageFormat::RGBA16F;
    inline constexpr ImageFormat kLightShaft     = ImageFormat::RGBA16F;
    inline constexpr ImageFormat kLensFlare      = ImageFormat::RGBA16F;
    inline constexpr ImageFormat kHeightFog      = ImageFormat::RGBA16F;

    // VolumetricCloudRenderer::EnsureTraceTargets.
    inline constexpr ImageFormat kCloudTrace = ImageFormat::RGBA16F;

    // Screen-space reflections and RSM global illumination.
    inline constexpr ImageFormat kSSRTrace  = ImageFormat::RGBA16F;
    inline constexpr ImageFormat kSSRAccum  = ImageFormat::RGBA16F;
    inline constexpr ImageFormat kGIResolve = ImageFormat::RGBA16F;
    inline constexpr ImageFormat kGIAccum   = ImageFormat::RGBA16F;
    // The RSM render pass MUST stay compatible with the G-buffer's (it reuses the G-buffer pipeline), so its
    // attachments are the G-buffer's formats by construction rather than a second spelling of them.
    inline constexpr ImageFormat kRSMAlbedo   = kGBufferA;
    inline constexpr ImageFormat kRSMNormal   = kGBufferB;
    inline constexpr ImageFormat kRSMPosition = kGBufferC;
    inline constexpr ImageFormat kRSMEmissive = kGBufferEmissive;
    inline constexpr ImageFormat kRSMDepth    = kGBufferDepth;

    // MeshRenderer::SetupShadowPass, one of each per cascade.
    inline constexpr ImageFormat kShadowColor = ImageFormat::R32F;
    inline constexpr ImageFormat kShadowDepth = ImageFormat::DEPTH24STENCIL8;
} // namespace Desert::Graphic::ViewTargetFormats
