#pragma once

#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <span>

// The access declarations of the deferred frame's graph nodes (SceneRendererFrameDeferred.cpp), device-free so
// RenderGraphCompile compiles exactly these declarations and checks the barriers the graph plans from them.
namespace Desert::Graphic::DeferredFrameNodes
{
    // The G-buffer depth leaves the frame graph in the attachment layout: the next frame's G-buffer pass begins
    // its render pass from it, and nothing reads the depth again after DepthResolve copied it.
    inline constexpr RDG::Access kGBufferDepthFinal = RDG::Access::DepthWrite;

    // "Deferred: DepthResolve": a Copy node, G-buffer depth -> scene target depth.
    inline void DeclareDepthResolve( RDG::PassBuilder& pass, RDG::TextureRef source, RDG::TextureRef target )
    {
        pass.Read( source, RDG::Access::CopySrc );
        pass.Write( target, RDG::Access::CopyDst );
    }

    // Every attachment of @p target STOREd, colour slot i with @p colors[i] (FrameTextures::ColorLoads of a LOAD:
    // a graph colour this node is the first writer of clears to its own value), the depth loaded, tested and
    // written: the graph opens the render pass a pipeline built for that target layout draws in, and leaves the
    // depth in the attachment layout. "Deferred: Composite" declares it, which is what takes the target depth back
    // from DepthResolve's transfer layout.
    inline void LoadTarget( RDG::PassBuilder& pass, const RDG::ImportedFramebuffer& target,
                            std::span<const RDG::LoadOp> colors )
    {
        // An invalid colour / resolve is an unused slot (FramebufferAttachment::UnusedColourSlot): no target, the
        // backend's render pass references it as VK_ATTACHMENT_UNUSED.
        for ( uint32_t i = 0; i < target.Colors.size(); ++i )
            if ( target.Colors[i].IsValid() )
                pass.ColorTarget( i, target.Colors[i], colors[i] );
        if ( target.Depth.IsValid() )
            pass.DepthTarget( target.Depth, RDG::LoadOp::Load(), /*write*/ true );
        // A slot with no in-pass resolve (an invalid ref: a SampleZero graph colour, ViewRasterTargets.hpp) is
        // skipped, as DeclareResolves does.
        for ( uint32_t i = 0; i < target.Resolves.size(); ++i )
            if ( target.Resolves[i].IsValid() )
                pass.ResolveTarget( i, target.Resolves[i] );
    }

    // G-buffer depth -> scene target depth. The same sample count: "Deferred: DepthResolve", a Copy node. A
    // multisampled scene target over the single-sample G-buffer: a copy between sample counts does not exist, so
    // "Deferred: DepthExpand" is a Raster node whose only target is the scene depth (every sample written by a
    // full-screen gl_FragDepth, so its old contents are DontCare) and which samples the G-buffer depth:
    // @p declareExpand( pass, source ) declares that read (the renderer's binding block, RDG-FAULT1).
    template <typename CopyExec, typename ExpandDeclare, typename ExpandExec>
    void AddDepthToScene( RDG::Builder& graph, uint32_t targetSamples, RDG::TextureRef source,
                          RDG::TextureRef target, CopyExec&& copy, ExpandDeclare&& declareExpand,
                          ExpandExec&& expand )
    {
        if ( targetSamples <= 1 )
        {
            graph.AddPass(
                 "Deferred: DepthResolve", RDG::PassFlags::Copy, [&]( RDG::PassBuilder& pass )
                 { DeclareDepthResolve( pass, source, target ); }, std::forward<CopyExec>( copy ) );
            return;
        }
        graph.AddPass(
             "Deferred: DepthExpand", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 declareExpand( pass, source );
                 pass.DepthTarget( target, RDG::LoadOp::DontCare(), /*write*/ true );
             },
             std::forward<ExpandExec>( expand ) );
    }

    // "Scene: DepthResolve": compute passes that read scene depth sample a single-sample image. At MSAA > 1 a
    // Raster node samples the multisampled scene depth (sample 0, SampledGraphics) and writes it through
    // gl_FragDepth into the 1x SceneDepthResolved (its only target, old contents DontCare); every consumer
    // declares its read of that 1x texture. At one sample no node exists and the consumers read the scene depth.
    // @p declareResolve( pass, sceneDepth ) declares the node's read (the renderer's binding block, RDG-FAULT1).
    template <typename ResolveDeclare, typename ResolveExec>
    void AddSceneDepthResolve( RDG::Builder& graph, uint32_t sceneSamples, RDG::TextureRef sceneDepth,
                               RDG::TextureRef resolved, ResolveDeclare&& declareResolve, ResolveExec&& resolve )
    {
        if ( sceneSamples <= 1 )
            return;
        graph.AddPass(
             "Scene: DepthResolve", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 declareResolve( pass, sceneDepth );
                 pass.DepthTarget( resolved, RDG::LoadOp::DontCare(), /*write*/ true );
             },
             std::forward<ResolveExec>( resolve ) );
    }
} // namespace Desert::Graphic::DeferredFrameNodes
