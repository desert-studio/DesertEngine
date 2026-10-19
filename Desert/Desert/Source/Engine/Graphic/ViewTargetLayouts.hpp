#pragma once

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <cstdint>

namespace Desert::Graphic
{
    // THE ONE SOURCE of the attachment formats every pipeline drawing into the view's targets is built against
    // (RDG-correct, UE-style: the graph owns the images, a pipeline knows only formats; a pipeline on a view
    // target never names the Framebuffer - census NoViewTargetPipelineNamesAFramebuffer). The scene target:
    // scene colour, the view's velocity and its responsive-AA mask (graph transients, ViewRasterTargets.hpp), scene
    // depth. The G-buffer: A,
    // B, C, Emissive, velocity, depth. Samples 1: a pipeline binds its variant for the open pass's count
    // (VulkanPipeline::GetVkPipelineFor).
    // MERGE NOTE (GBUF1): slot 2 is kGBufferShadingWord on task/GBUF1 - take GBUF1's name there.
    inline RenderTargetLayout SceneTargetLayout()
    {
        return RenderTargetLayout{
             { ViewTargetFormats::kSceneColor, ViewTargetFormats::kVelocity, ViewTargetFormats::kResponsiveAA,
               ViewTargetFormats::kThinGeometryCoverage },
             ViewTargetFormats::kSceneDepth,
             1 };
    }

    inline RenderTargetLayout GBufferLayout()
    {
        return RenderTargetLayout{ { ViewTargetFormats::kGBufferA, ViewTargetFormats::kGBufferB,
                                     ViewTargetFormats::kGBufferShadingWord, ViewTargetFormats::kGBufferEmissive,
                                     ViewTargetFormats::kVelocity, ViewTargetFormats::kThinGeometryCoverage },
                                   ViewTargetFormats::kGBufferDepth,
                                   1 };
    }

    // The translucent front layer's G-buffer ("Translucency: Front Layer G-buffer", Mesh/Surface/Pass_FrontLayer.glslh;
    // UE FrontLayerTranslucency GBufferA + SceneDepth): the G-buffer normal slot's format (normal + roughness, read by
    // the reflection trace exactly as the opaque one), the view's velocity format (the reflection denoiser's
    // reprojection), the G-buffer depth format. All graph transients of the frame.
    inline RenderTargetLayout FrontLayerTargetLayout()
    {
        return RenderTargetLayout{ { ViewTargetFormats::kGBufferB, ViewTargetFormats::kVelocity },
                                   ViewTargetFormats::kGBufferDepth,
                                   1 };
    }
    // The graph-provided colour slots: velocity after each framebuffer's own colours.
    // The scene target's responsive-AA mask after it (no G-buffer slot: only translucency writes it).
    inline constexpr uint32_t kSceneTargetVelocitySlot     = 1;
    inline constexpr uint32_t kSceneTargetResponsiveAASlot = 2;
    inline constexpr uint32_t kGBufferVelocitySlot         = 4;
    // The thin-geometry coverage after them on both (written by the passes that write the view's depth).
    inline constexpr uint32_t kSceneTargetThinGeometryCoverageSlot = 3;
    inline constexpr uint32_t kGBufferThinGeometryCoverageSlot     = 5;
} // namespace Desert::Graphic
