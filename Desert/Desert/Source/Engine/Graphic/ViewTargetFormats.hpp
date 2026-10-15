#pragma once

#include <Engine/Core/Formats/ImageFormat.hpp>

#include <array>
#include <optional>

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
    // The shading word (ShadingModels/ShadingModelContract.glslh): model index, the four-float payload (UE's
    // GBufferD CustomData), texture count and the no-sun-shadows bit, as a uvec2. World position is NOT stored:
    // every reader rebuilds it from kGBufferDepth (Common/ReconstructPosition.glslh).
    inline constexpr ImageFormat kGBufferShadingWord = ImageFormat::RG32_UINT;
    inline constexpr ImageFormat kGBufferEmissive = ImageFormat::RGBA16F;
    inline constexpr ImageFormat kGBufferDepth    = ImageFormat::DEPTH32F;
    // Screen motion of the surface each pixel shows: current minus previous UNJITTERED NDC.xy (TAA1, see
    // View/SceneViewState.hpp "Velocity" — kVelocityFormat there is this constant). A colour attachment of the
    // G-buffer AND of the scene target, because both hold passes that write the view's depth.
    inline constexpr ImageFormat kVelocity        = ImageFormat::RG16F;
    // Responsive AA (UE: the stencil bit STENCIL_TEMPORAL_RESPONSIVE_AA_MASK a `bResponsiveAA` material writes): how
    // much of each pixel a responsive translucent surface or particle covers, 0..1, blended like the colour it
    // composites. The temporal resolve raises the current frame's weight to UE's 1/4 by it, so a surface that
    // writes no velocity of its own leaves no trail. A colour attachment of the scene target only: nothing on the
    // G-buffer is translucent. Our depth has no stencil, hence a target of its own.
    inline constexpr ImageFormat kResponsiveAA    = ImageFormat::R8_UNORM;
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

    // Screen-space reflections.
    inline constexpr ImageFormat kSSRTrace  = ImageFormat::RGBA16F;
    // The trace texel's ray (SSR.shader u_TraceRay): xyz = the WORLD point it hit (centimetres: half floats step
    // by 4 cm at 50 m, too coarse to re-aim a mirror's ray from a neighbour), w = log2 of its pdf.
    inline constexpr ImageFormat kSSRTraceRay = ImageFormat::RGBA32F;
    // One texel per SSR tile, 0 or 1 (SSRRenderer). RGBA8 because ImageFormat has no single-channel 8-bit
    // colour format; at 1/64 of the screen the three unused channels cost nothing worth a new format.
    inline constexpr ImageFormat kSSRTileMask = ImageFormat::RGBA8F;
    // The closest-depth hierarchy (HZBRenderer): one 32-bit float per texel, the reversed-Z device depth exactly
    // as the depth attachment stores it (a 16-bit depth would quantise the far half of the scene into a few
    // hundred values).
    inline constexpr ImageFormat kHZB = ImageFormat::R32F;
    // The screen-space denoiser (System::ScreenSpaceDenoiser), every signal: the reconstructed signal, its
    // accumulation history and its output (premultiplied / straight rgb + coverage); the history's sample count,
    // one 32-bit float (R32F: the single-channel storage format every device writes).
    inline constexpr ImageFormat kDenoiserSignal      = ImageFormat::RGBA16F;
    inline constexpr ImageFormat kDenoiserSampleCount = ImageFormat::R32F;
    // The traced GI's final gather, per pixel: diffuse irradiance and specular radiance (rgb), written by the
    // screen probe gather (GlobalIllumination::ScreenProbeGather, a history pair each) or the irradiance field
    // gather (GlobalIllumination::IrradianceFieldGather, graph transients), read by the deferred composite.
    inline constexpr ImageFormat kGIFinalGather = ImageFormat::RGBA16F;

    // EVERY SHADOW MAP IS ONE DEPTH-ONLY D32F IMAGE (UE: the shadow depth pass writes depth and nothing else,
    // ShadowDepthPixelShader.usf). The cascades (two per cascade: the sampled map and its cache of static casters,
    // MeshRenderer::SetupShadowPass) and the local lights' atlas (MeshRenderer::UpdateLocalShadows). It used to be
    // an R32F colour copy of the depth next to a D24S8 attachment: twice the bandwidth for the same number, and a
    // 24-bit depth test under a 32-bit sampled value. The receivers read this image itself.
    inline constexpr ImageFormat kShadowDepth = ImageFormat::DEPTH32F;
} // namespace Desert::Graphic::ViewTargetFormats
