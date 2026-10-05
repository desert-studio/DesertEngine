#pragma once

#include <Engine/Graphic/RendererContext.hpp>
#include <Common/Core/Memory/CommandBuffer.hpp>

#include <Engine/Graphic/Materials/MaterialExecutor.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RenderPass.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/DefaultTextures.hpp>
#include <Engine/Graphic/FallbackTextures.hpp>
#include <Engine/Graphic/Image.hpp>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class RendererAPI;
    namespace RDG
    {
        class Builder;
        class IPhysicalTexture;
        class PassBindings;
        struct ExternalTexture;
        struct ExternalBuffer;
    } // namespace RDG

    class Renderer : public Common::Singleton<Renderer>
    {
    public:
        Common::BoolResultStr Init();
        void                  Shutdown();

        [[nodiscard]] Common::BoolResultStr BeginFrame();
        [[nodiscard]] Common::BoolResultStr EndFrame();
        void BeginRenderPass( const RenderPass* renderPass, bool clearFrame = true );
        void BeginSwapChainRenderPass();
        void EndRenderPass();

        // Named region in the current command buffer (RenderDoc pass tree). Pair Begin/End.
        void BeginDebugLabel( const char* name );
        Common::BoolResultStr ExecuteGraph( RDG::Builder& graph );
        Common::BoolResultStr ImportImage( const std::shared_ptr<Image>& image, RDG::ExternalTexture& into );
        Common::BoolResultStr ImportBuffer( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer,
                                            RDG::ExternalBuffer&                                   into );
        void                  EndDebugLabel();
        // One triangle covering the viewport (3 vertices, Common/FullscreenTriangle.glslh) in the open render
        // pass.
        void SubmitFullscreenTriangle( const GraphicsPipeline* pipeline,
                                       const MaterialExecutor* materialExecutor );

        // Indexed draw from a caller-supplied dynamic VB+IB (the 2D/UI batcher). One call per state batch.
        void SubmitIndexed( const GraphicsPipeline* pipeline, VertexBuffer* vertexBuffer, IndexBuffer* indexBuffer,
                            uint32_t indexCount, uint32_t firstIndex, const MaterialExecutor* materialExecutor );

        // Vertexless line draw: the pipeline (Lines topology) pulls vertices from a storage buffer by index.
        void SubmitLines( const GraphicsPipeline* pipeline, uint32_t vertexCount, float lineWidth,
                          const MaterialExecutor* materialExecutor );

        // RDG-A2 - the renderer-level consumers of a PassBindings (RDGPassBindings.hpp). Called only from inside
        // the exec lambda whose PassContext built @p bindings; they record on that pass's command buffer
        // (VulkanRdgBackend::CommandBufferOf), write one descriptor set per set of the pipeline layout for THIS
        // exec (VulkanRdgBackend::DescriptorsOf) and bind it. Nothing they write outlives the exec.
        // Refused, with the pass and slot named, when: @p bindings has a failed entry (GetStatus); a name is not
        // a resource of the pipeline's shader; an entry's kind does not match the reflected descriptor type; a
        // resource slot of the shader is filled by neither @p bindings nor @p material; or a slot is filled by
        // both (a graph resource is bound only through @p bindings). No barrier and no layout transition is
        // recorded here: the graph placed them from the pass's declarations.
        [[nodiscard]] Common::BoolResultStr DispatchCompute( const RDG::PassBindings& bindings,
                                                             const ComputePipeline& pipeline, uint32_t groupCountX,
                                                             uint32_t groupCountY, uint32_t groupCountZ );
        // One triangle covering the viewport: DrawProcedural( ..., kFullscreenTriangleVertexCount, 1 )
        // (Common/FullscreenTriangle.glslh), drawn inside the render pass the graph opened for this pass (its
        // ColorTarget / DepthTarget declarations). @p material supplies uniform values and asset textures only;
        // may be null.
        [[nodiscard]] Common::BoolResultStr DrawFullscreen( const RDG::PassBindings& bindings,
                                                            const GraphicsPipeline&  pipeline,
                                                            const MaterialExecutor*  material );
        // UE DrawPrimitive: the same contract as DrawFullscreen for a vertex stage that builds @p vertexCount (>
        // 0) vertices x @p instanceCount (> 0) instances from gl_VertexIndex / gl_InstanceIndex with no vertex
        // buffer, e.g. the SSR tile grid (six vertices per tile, unmarked tiles collapsed, one instance).
        [[nodiscard]] Common::BoolResultStr DrawProcedural( const RDG::PassBindings& bindings,
                                                            const GraphicsPipeline&  pipeline,
                                                            const MaterialExecutor* material, uint32_t vertexCount,
                                                            uint32_t instanceCount );
        // The PassBindings route for indexed batched draws (the 2D/UI batcher): SubmitIndexed's draw with the
        // graph textures bound by shader name, e.g. Render2D's glass batches sampling u_Backdrop from this
        // frame's backdrop pyramid. Same contract as DrawFullscreen otherwise.
        [[nodiscard]] Common::BoolResultStr DrawIndexed( const RDG::PassBindings& bindings,
                                                         const GraphicsPipeline&  pipeline,
                                                         const MaterialExecutor*  material,
                                                         VertexBuffer& vertexBuffer, IndexBuffer& indexBuffer,
                                                         uint32_t indexCount, uint32_t firstIndex );
        // The PassBindings route for mesh draws (UE: a mesh pass binds its graph textures through the pass
        // parameters): RenderMesh's submesh / LOD walk (same hidden-submesh mask, same LOD clamp, same instance
        // offset), every submesh drawn with the graph textures bound by shader name from @p bindings, e.g. the
        // forward glass sampling u_SceneColor from this frame's scene copy. @p material is required: its push
        // block is pushed for each submesh with that submesh's transform (transform * submesh.Transform) over its
        // first mat4, as RenderMesh does, and it supplies uniform values / asset textures; a @p bindings with push
        // constants of its own is refused (the block is the transform's). Same contract as DrawFullscreen
        // otherwise. The first refused submesh ends the draw.
        [[nodiscard]] Common::BoolResultStr
        RenderMesh( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline, const Mesh& mesh,
                    const glm::mat4& transform, const MaterialExecutor& material, uint32_t instanceCount,
                    uint32_t firstInstance, uint64_t hiddenSubmeshMask, uint32_t lodLevel );

        // Compute dispatch whose writes are made visible to the VERTEX + DRAW_INDIRECT stages (GPU cull
        // feeding an indirect instanced draw).
        // Layout helpers for compute storage targets used in the frame command buffer (see RendererAPI).
        void ComputeImageBeginWrite( Image* image );
        void ComputeImageEndWrite( Image* image );

        // Layout helpers for an image a compute dispatch SAMPLES that a graphics pass owns — the scene
        // depth attachment above all. See RendererAPI for why this is not a one-line transition.
        void ComputeImageBeginRead( Image* image );
        void ComputeImageEndRead( Image* image );

        // Copy the depth aspect of @p src into @p dst (Deferred: G-buffer depth -> scene target depth so
        // depth-tested overlays occlude against static geometry). Call outside a render pass.
        Common::BoolResultStr CopyDepthImage( Image2D* src, Image2D* dst );

        // Set the scissor rect (framebuffer px, top-left origin). Used by the 2D batcher for UI clipping.
        void SetScissor( int32_t x, int32_t y, uint32_t width, uint32_t height );

        // Carries the backend's own result: a failed submit or present is what a lost device looks like
        // from here, and Application::Run ends the run on it. See Renderer.cpp for why it was void, and
        // RendererAPI.hpp for why the PrepareNextFrame that stood beside it is gone.
        [[nodiscard]] Common::BoolResultStr PresentFinalImage();

        void ResizeWindowEvent( uint32_t width, uint32_t height );
        void WaitDeviceIdle();

        // Recreate every registered image's sampler from the current RenderConfig filter (live filter swap).
        void RecreateImageSamplers();

        RendererAPI* GetRendererAPI() const;

        const std::shared_ptr<Graphic::Texture2D>& GetBRDFTexture() const;

        std::shared_ptr<Framebuffer> GetCompositeFramebuffer();
        uint32_t                     GetCurrentFrameIndex();

        template <typename FuncT>
        static void SubmitCommand( FuncT&& func )
        {
            Common::Memory::SubmitCommand( GetRenderCommandQueue(), std::forward<FuncT>( func ) );
        }

    private:
        [[nodiscard]] Common::BoolResultStr InitGraphicAPI();

    private:
        static Common::Memory::CommandBuffer& GetRenderCommandQueue();

    private:
        std::shared_ptr<Texture2D> m_BRDFTexture;
    };
} // namespace Desert::Graphic