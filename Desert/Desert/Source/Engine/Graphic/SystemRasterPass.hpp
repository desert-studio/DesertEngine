#pragma once

#include "Framebuffer.hpp"
#include "Pipeline.hpp"
#include "RenderPass.hpp"
#include "RenderPassDeclaration.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Desert::Graphic
{
    // One raster pass a render system hands the frame build. It carries no placement: where it runs in the frame
    // is the position of the SceneRenderer::AddFrame* call that adds it (SceneRenderer::AddSystemRaster /
    // AddSystemRasters), the way UE's FDeferredShadingSceneRenderer::Render calls AddBasePass, RenderTranslucency
    // and AddPostProcessingPasses in order. The graph opens the render pass on TargetFramebuffer and merges
    // consecutive nodes on one framebuffer into one.
    struct SystemRasterPass
    {
        std::string                  Name;
        NodeRecordFunc               ExecuteFunc;
        std::shared_ptr<Framebuffer> TargetFramebuffer;

        // What the pass reads from the graph (its draw list and binding blocks are built here, before any command
        // is recorded): the graph textures it samples, by this frame's FrameGraphRefs.
        std::function<void( RenderPassDeclaration&, const FrameGraphRefs& )> Declare;

        // The values the pass clears its target to when the frame build opens it with a CLEAR (the first pass of a
        // clearing sequence on that framebuffer); unset = the RenderPassSpecification defaults.
        std::optional<glm::vec4> ClearColor;
        std::optional<float>     ClearDepth;

        // The pass depth-TESTS against its target's depth and never writes it, so the node binds the depth as a
        // READ-ONLY attachment (RDG::Access::DepthRead, DEPTH_STENCIL_READ_ONLY_OPTIMAL) and its Declare may also
        // sample that same depth (UE: FExclusiveDepthStencil::DepthRead with the SceneDepth SRV - the particle
        // depth fade). A read-only depth cannot be cleared: the node must LOAD it.
        bool DepthReadOnly = false;
    };
} // namespace Desert::Graphic
