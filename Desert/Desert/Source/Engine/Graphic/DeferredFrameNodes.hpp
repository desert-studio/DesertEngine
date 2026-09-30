#pragma once

#include <Engine/Graphic/RDG/RDGBuilder.hpp>

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

    // Every attachment of @p target LOAD/STORE, the depth tested and written: the graph opens the render pass a
    // pipeline built for that framebuffer draws in, and leaves the depth in the attachment layout. "Deferred:
    // Composite" declares it, which is what takes the target depth back from DepthResolve's transfer layout.
    inline void LoadTarget( RDG::PassBuilder& pass, const RDG::ImportedFramebuffer& target )
    {
        for ( uint32_t i = 0; i < target.Colors.size(); ++i )
            pass.ColorTarget( i, target.Colors[i], RDG::LoadOp::Load() );
        if ( target.Depth.IsValid() )
            pass.DepthTarget( target.Depth, RDG::LoadOp::Load(), /*write*/ true );
        for ( uint32_t i = 0; i < target.Resolves.size(); ++i )
            pass.ResolveTarget( i, target.Resolves[i] );
    }

    // G-buffer depth -> scene target depth. The same sample count: "Deferred: DepthResolve", a Copy node. A
    // multisampled scene target over the single-sample G-buffer: a copy between sample counts does not exist, so
    // "Deferred: DepthExpand" is a Raster node whose only target is the scene depth (every sample written by a
    // full-screen gl_FragDepth, so its old contents are DontCare) and which samples the G-buffer depth.
    template <typename CopyExec, typename ExpandExec>
    void AddDepthToScene( RDG::Builder& graph, uint32_t targetSamples, RDG::TextureRef source,
                          RDG::TextureRef target, CopyExec&& copy, ExpandExec&& expand )
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
                 pass.Read( source, RDG::Access::SampledGraphics );
                 pass.DepthTarget( target, RDG::LoadOp::DontCare(), /*write*/ true );
             },
             std::forward<ExpandExec>( expand ) );
    }

    // "Scene: DepthResolve": compute passes that read scene depth sample a single-sample image. At MSAA > 1 a
    // Raster node samples the multisampled scene depth (sample 0, SampledGraphics) and writes it through
    // gl_FragDepth into the 1x SceneDepthResolved (its only target, old contents DontCare); every consumer
    // declares its read of that 1x texture. At one sample no node exists and the consumers read the scene depth.
    template <typename ResolveExec>
    void AddSceneDepthResolve( RDG::Builder& graph, uint32_t sceneSamples, RDG::TextureRef sceneDepth,
                               RDG::TextureRef resolved, ResolveExec&& resolve )
    {
        if ( sceneSamples <= 1 )
            return;
        graph.AddPass(
             "Scene: DepthResolve", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( sceneDepth, RDG::Access::SampledGraphics );
                 pass.DepthTarget( resolved, RDG::LoadOp::DontCare(), /*write*/ true );
             },
             std::forward<ResolveExec>( resolve ) );
    }
} // namespace Desert::Graphic::DeferredFrameNodes
