#pragma once

#include "Framebuffer.hpp"
#include "Pipeline.hpp"
#include "RenderPass.hpp"
#include "RenderPassDeclaration.hpp"

#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <Common/Core/Memory/ArenaFunction.hpp>

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
    // CPU-RDG-ALLOC (UE: FRDGBuilder::AddPass, whose raster lambda lives in FRDGAllocator): a pass is built for
    // one frame and MOVED, never copied - into the vector the frame build hands SceneRenderer::AddSystemRasters,
    // and its body into the graph node's closure (Builder::AddPass places that in the graph's arena). Its bodies
    // are move-only ArenaFunctions: a capture set that fits the inline buffer takes no heap block.
    struct SystemRasterPass
    {
        // What the pass reads from the graph (Declare below): called once, while the frame graph is built.
        using DeclareFunc = Common::Memory::ArenaFunction<void( RenderPassDeclaration&, const FrameGraphRefs& )>;

        std::string                  Name;
        NodeRecord                   ExecuteFunc;
        std::shared_ptr<Framebuffer> TargetFramebuffer;
        // Or ONE LAYER of a graph depth array, with no framebuffer (the sun's cascades: layer c of the cascade
        // array, UE's whole-scene shadow depth target). Exactly one of the two is set; the pass's pipelines were
        // built against a framebuffer of the same depth format, which is all the render pass compatibility asks.
        RDG::TextureRef TargetDepthArray;
        uint32_t        TargetLayer = 0;

        // What the pass reads from the graph (its draw list and binding blocks are built here, before any command
        // is recorded): the graph textures it samples, by this frame's FrameGraphRefs.
        DeclareFunc Declare;

        // The values the pass clears its target to when the frame build opens it with a CLEAR (the first pass of a
        // clearing sequence on that framebuffer); unset = the RenderPassSpecification defaults.
        std::optional<glm::vec4> ClearColor;
        std::optional<float>     ClearDepth;

        // The pass depth-TESTS against its target's depth and never writes it, so the node binds the depth as a
        // READ-ONLY attachment (RDG::Access::DepthRead, DEPTH_STENCIL_READ_ONLY_OPTIMAL) and its Declare may also
        // sample that same depth (UE: FExclusiveDepthStencil::DepthRead with the SceneDepth SRV - the particle
        // depth fade). A read-only depth cannot be cleared: the node must LOAD it.
        bool DepthReadOnly = false;

        // RDG::PassFlags::NeverParallel (UE ERDGPassFlags::NeverParallel): the exec writes CPU state shared with
        // other passes or objects (Render2D's vertex buffers and flush bookkeeping), so the node records on the
        // calling thread, in pass order, never in a worker's chunk.
        bool NeverParallel = false;
    };
} // namespace Desert::Graphic
