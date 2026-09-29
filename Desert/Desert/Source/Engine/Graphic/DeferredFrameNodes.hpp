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
    }
} // namespace Desert::Graphic::DeferredFrameNodes
