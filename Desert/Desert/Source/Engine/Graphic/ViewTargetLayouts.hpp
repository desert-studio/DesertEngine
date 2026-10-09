#pragma once

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>

#include <cstdint>

namespace Desert::Graphic
{
    // THE ONE SOURCE of the attachment formats every pipeline drawing into the view's targets is built against
    // (RDG-correct, UE-style: the graph owns the images, a pipeline knows only formats; a pipeline on a view
    // target never names the Framebuffer - census NoViewTargetPipelineNamesAFramebuffer). The scene target:
    // scene colour, the view's velocity (a graph transient, ViewRasterTargets.hpp), scene depth. The G-buffer: A,
    // B, C, Emissive, velocity, depth. Samples 1: a pipeline binds its variant for the open pass's count
    // (VulkanPipeline::GetVkPipelineFor).
    // MERGE NOTE (GBUF1): slot 2 is kGBufferShadingWord on task/GBUF1 - take GBUF1's name there.
    inline RenderTargetLayout SceneTargetLayout()
    {
        return RenderTargetLayout{
             { ViewTargetFormats::kSceneColor, ViewTargetFormats::kVelocity }, ViewTargetFormats::kSceneDepth, 1 };
    }

    inline RenderTargetLayout GBufferLayout()
    {
        return RenderTargetLayout{ { ViewTargetFormats::kGBufferA, ViewTargetFormats::kGBufferB,
                                     ViewTargetFormats::kGBufferShadingWord, ViewTargetFormats::kGBufferEmissive,
                                     ViewTargetFormats::kVelocity },
                                   ViewTargetFormats::kGBufferDepth,
                                   1 };
    }

    // The graph-provided colour slots: velocity after each framebuffer's own colours.
    inline constexpr uint32_t kSceneTargetVelocitySlot = 1;
    inline constexpr uint32_t kGBufferVelocitySlot     = 4;
} // namespace Desert::Graphic
