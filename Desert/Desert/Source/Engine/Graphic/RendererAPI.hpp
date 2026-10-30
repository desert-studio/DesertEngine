#pragma once

#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <span>
#include <Engine/Core/Window.hpp>
#include <Engine/Graphic/RenderPass.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/MeshDrawArguments.hpp>
#include <Engine/Graphic/RDG/RDGBindingDecl.hpp>
#include <Engine/Graphic/RayTracingPipeline.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingHitGroupTable.hpp>
#include <Common/Core/ResultStr.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class VertexBuffer;
    class SharedGpuBuffer;
    class SharedGpuTimeline;
    namespace RDG
    {
        class Builder;
        class IPhysicalTexture;
        class PassBindings;
        class PassContext;
        struct TextureRef;
        struct ExternalTexture;
        struct ExternalBuffer;
    } // namespace RDG

    // One triangle covering the viewport (Common/FullscreenTriangle.glslh): the vertex stage builds it from
    // gl_VertexIndex with no vertex buffer, so every fullscreen draw is Draw(3, 1, 0, 0).
    inline constexpr uint32_t kFullscreenTriangleVertexCount = 3;

    // One IndirectGroup of a binned mesh pass (GpuDriven/IndirectDrawSubmission.hpp): MaxDraws records of
    // VkDrawIndexedIndirectCommand at Stride from ArgsOffset of Args, every one an indexed draw of the group's
    // mesh. The MaxDraws records are read as they are (a culled bin's InstanceCount is 0), MaxDrawsPerCall per
    // vkCmdDrawIndexedIndirect (1 = one call per record: a device without multiDrawIndirect). Args is declared
    // Access::IndirectArgs.
    struct MeshIndirectGroupDraw
    {
        RDG::BufferRef Args{};
        uint64_t       ArgsOffset      = 0;
        uint32_t       MaxDraws        = 0;
        uint32_t       Stride          = 0;
        uint32_t       MaxDrawsPerCall = 1;
        // The pass state the group's shader reads beside the graph block: the scene/view blocks (camera, lights,
        // the lent GpuScene primitives and material table) its `.Binned` cell's per-view carrier holds. Null =
        // the block alone.
        const MaterialExecutor* Material = nullptr;
        // The instance id stream at binding 2 (a_VisibleRow): rows [0, VisibleRows) of the identity stream, so
        // a_VisibleRow = gl_InstanceIndex = the record's firstInstance + i, a row of the pass's culled
        // VisibleInstances. 0 = the pipeline reads no stream.
        uint32_t VisibleRows = 0;
    };

    // One material group of the virtual-geometry raster (VirtualGeometry::RasterBins): ONE non-indexed
    // VkDrawIndirectCommand at Args + ArgsOffset (vertexCount = kClusterMaxTriangles x 3, instanceCount = the
    // group's clusters, firstInstance = its first Rows entry), drawn with the group's `.VirtualGeometry` cell.
    // Rows is bound at binding 2 as the instance id stream (a_VisibleRow = a VisibleClusters row); the cell
    // fetches its vertices from the cluster (Common/VirtualGeometry.glslh), so no vertex or index buffer is bound.
    struct ClusterIndirectGroupDraw
    {
        RDG::BufferRef Args{}; // Access::IndirectArgs
        uint64_t       ArgsOffset = 0;
        RDG::BufferRef Rows{}; // uint32 rows - Access::VertexIndex
        // The view's carrier of the scene blocks, as MeshIndirectGroupDraw::Material.
        const MaterialExecutor* Material = nullptr;
    };

    enum class RendererAPIType : uint8_t
    {
        None   = 0,
        Vulkan = 1,
    };

    class RendererAPI
    {
    public:
        explicit RendererAPI( const std::shared_ptr<Window>& window ) : m_Window( window )
        {
        }
        virtual ~RendererAPI() = default;

    public:
        virtual void Init()     = 0;
        virtual void Shutdown() = 0;

        virtual Common::BoolResultStr BeginFrame()                                                     = 0;
        virtual Common::BoolResultStr EndFrame()                                                       = 0;
        // `PrepareNextFrame() = 0` USED TO SIT HERE, AND THE WHOLE CHAIN BELOW IT WAS UNREACHABLE.
        // `Window::PrepareNextFrame` acquires through `RendererContext::BeginFrame` instead, so this
        // interface, `Renderer::PrepareNextFrame` and `VulkanRendererAPI::PrepareNextFrame` were a second
        // spelling of the acquire that nothing ever called. Two paths to one act, one of them never
        // exercised, is how a guard ends up on the wrong one — removed rather than kept "just in case".
        virtual Common::BoolResultStr PresentFinalImage()                                              = 0;
        virtual Common::BoolResultStr BeginRenderPass( const RenderPass* renderPass, bool clearFrame ) = 0;
        virtual Common::BoolResultStr EndRenderPass()                                                  = 0;

        // Named command-buffer region for graphics debuggers (RenderDoc shows these as a pass
        // tree). Default no-op so non-debug backends don't have to care.
        virtual void BeginDebugLabel( const char* /*name*/ )
        {
        }
        virtual void EndDebugLabel()
        {
        }

        // Records a frame graph into the frame's command buffer: the backend's barriers, render passes,
        // labels and pass timings, and each pass's own recording, in the graph's order.
        virtual Common::BoolResultStr ExecuteGraph( RDG::Builder& graph ) = 0;

        // Imports @p image into @p into for one graph: its description (a 2D image, a volume or a cube, as the
        // image describes itself), its physical image, the state its own layout record implies
        // (RDG::RecordedLayoutState), and the hook through which Execute writes the final layout back into
        // that record. ONE path for every image kind. Fails for an image with no memory or one in a layout
        // the graph has no name for.
        virtual Common::BoolResultStr ImportImage( const std::shared_ptr<Image>& image,
                                                   RDG::ExternalTexture&         into ) = 0;

        // Imports the window's back buffer acquired for this frame into @p into for one graph (UE: the viewport's
        // RHI texture registered as an external texture each frame). Its prior contents are undefined; the graph
        // that writes it extracts it as RDG::Access::Present, and the frame presents after that graph. Fails when
        // there is no window or no acquired image.
        virtual Common::BoolResultStr ImportBackBuffer( RDG::ExternalTexture& into ) = 0;

        // Imports the copy of @p buffer bound for this frame and the active view into @p into for one graph:
        // its size, the graph's non-owning handle on it, the state the last graph that imported the same copy
        // left it in (untouched for a copy no graph has seen), and the hook through which Execute records the
        // final state back. The same shape as ImportImage: a pass that dispatches or draws with the buffer
        // declares its storage/vertex/indirect access on the imported handle, and the graph places the
        // buffer barriers. Fails when the buffer's copy for this frame cannot be bound.
        virtual Common::BoolResultStr ImportBuffer( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer,
                                                    RDG::ExternalBuffer& into ) = 0;
        // The same import for a vertex buffer a compute pass reads or writes as storage (a Static mesh's base
        // vertices, a GpuWritten morph output): one copy, its state carried across graphs by the buffer itself.
        // Fails for a buffer that has not been built.
        virtual Common::BoolResultStr ImportVertexBuffer( const std::shared_ptr<VertexBuffer>& buffer,
                                                          RDG::ExternalBuffer&                 into ) = 0;
        // A mesh's span [@p offset, + @p bytes) of a vertex buffer (MeshGeometry: a GeometryPool arena, or a
        // twin's own buffer). A Pooled buffer is imported by span: no graph writes it (its uploads and growth are
        // blocking copies outside every graph), so the span starts from no access and records no final state,
        // and the graph's handle addresses, describes and fences the span only (VulkanRdgBuffer::GetOffset).
        // Any other buffer holds one mesh from byte 0 and goes through ImportVertexBuffer whole (its state
        // record). Fails for a buffer that has not been built, a span past its end, or a non-zero offset into a
        // buffer that is not Pooled.
        virtual Common::BoolResultStr ImportVertexBufferRange( const std::shared_ptr<VertexBuffer>& buffer,
                                                               uint64_t offset, uint64_t bytes,
                                                               RDG::ExternalBuffer& into ) = 0;

        // GPU memory and order shared with another API on this GPU (SharedGpuMemory.hpp; the reader is the path
        // tracer's OIDN). A device-local storage buffer of @p bytes and a timeline semaphore, both exportable by
        // the handle type CapabilityCatalog::Sharing names. Refused by name when the catalog says
        // GpuSharing::None. Not pure: a renderer API that shares nothing (the tests' fakes) refuses here.
        [[nodiscard]] virtual Common::ResultStr<std::shared_ptr<SharedGpuBuffer>>
        CreateSharedBuffer( std::string_view name, uint64_t /*bytes*/ )
        {
            return Common::MakeError<std::shared_ptr<SharedGpuBuffer>>( std::string( "CreateSharedBuffer(" ) +
                                                                        std::string( name ) +
                                                                        "): this renderer API shares no memory" );
        }
        [[nodiscard]] virtual Common::ResultStr<std::shared_ptr<SharedGpuTimeline>>
        CreateSharedTimeline( std::string_view name )
        {
            return Common::MakeError<std::shared_ptr<SharedGpuTimeline>>(
                 std::string( "CreateSharedTimeline(" ) + std::string( name ) +
                 "): this renderer API shares no semaphore" );
        }

        // Vertexless draw in the pipeline's topology (the vertex stage pulls from a storage buffer by index).
        // Code outside the renderer bound vertex / index buffers on the current command buffer (the ImGui
        // backend): the next mesh draw must bind its geometry, whatever the renderer last bound.
        virtual void InvalidateGeometryBinding() = 0;

        virtual void SubmitPulled( const GraphicsPipeline* pipeline, uint32_t vertexCount, float lineWidth,
                                   const MaterialExecutor* materialExecutor ) = 0;

        // The in-graph consumers of an RDG::PassBindings (see Renderer::DispatchCompute / DrawFullscreen): record
        // into the command buffer of the pass the bindings were built in, with descriptor sets written for this
        // exec only.
        [[nodiscard]] virtual Common::BoolResultStr DispatchCompute( const RDG::PassBindings& bindings,
                                                                     const ComputePipeline&   pipeline,
                                                                     uint32_t groupCountX, uint32_t groupCountY,
                                                                     uint32_t groupCountZ ) = 0;
        // The same dispatch with its group counts read on the GPU (UE DispatchIndirectComputeShader): one
        // VkDispatchIndirectCommand at byte @p offset of @p args, a buffer of this graph the pass declared
        // Access::IndirectArgs. Refused for an undeclared buffer, an offset not a multiple of four or arguments
        // past the buffer's end.
        [[nodiscard]] virtual Common::BoolResultStr DispatchComputeIndirect( const RDG::PassBindings& bindings,
                                                                             const ComputePipeline&   pipeline,
                                                                             RDG::BufferRef           args,
                                                                             uint64_t                 offset ) = 0;
        // The ray tracing pipeline's launch (UE RayTraceDispatch): @p width x @p height ray generation invocations
        // of @p pipeline with the pass's bindings resolved against its program, and the shader binding table built
        // from @p hits (one hit record per segment of the frame's TLAS, RayTracingHitGroupTable.hpp) for this
        // launch. Refused on a pipeline that is not built or a hit record naming a group the pipeline lacks.
        [[nodiscard]] virtual Common::BoolResultStr TraceRays( const RDG::PassBindings&                   bindings,
                                                               const RayTracingPipeline&                  pipeline,
                                                               const RayTracing::RayTracingHitGroupTable& hits,
                                                               uint32_t width, uint32_t height ) = 0;
        // A non-indexed, vertex-buffer-less draw of @p vertexCount vertices x @p instanceCount instances whose
        // vertex stage builds its geometry from gl_VertexIndex / gl_InstanceIndex (UE DrawPrimitive; the
        // fullscreen triangle is 3 x 1; the SSR tile grid is six vertices per tile).
        [[nodiscard]] virtual Common::BoolResultStr
        // @p push: the draw's own push block (a copy of the material's with the draw's values over it, made by the
        // caller - the shared material is not written to record a draw); empty = the material's block.
        DrawProcedural( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline,
                        const MaterialExecutor* material, std::span<const std::byte> push, uint32_t vertexCount,
                        uint32_t instanceCount ) = 0;
        // The same draw with its vertex / instance counts read on the GPU (UE DrawPrimitiveIndirect): one
        // VkDrawIndirectCommand at byte @p offset of @p args, a buffer of this graph the pass declared
        // Access::IndirectArgs (RenderPassDeclaration::Read). Refused for an undeclared buffer or an offset not a
        // multiple of four.
        [[nodiscard]] virtual Common::BoolResultStr
        DrawProceduralIndirect( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline,
                                const MaterialExecutor* material, std::span<const std::byte> push,
                                RDG::BufferRef args, uint64_t offset ) = 0;
        // The PassBindings indexed draw: one indexed draw of @p indexCount indices from @p firstIndex of
        // a caller-filled VB + IB (uint32 indices, vertices addressed absolutely), with the graph textures bound
        // by shader name from @p bindings and @p material supplying uniform values / asset textures only.
        [[nodiscard]] virtual Common::BoolResultStr
        DrawIndexed( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline,
                     const MaterialExecutor* material, VertexBuffer& vertexBuffer, IndexBuffer& indexBuffer,
                     uint32_t indexCount, uint32_t firstIndex ) = 0;
        // The setup-time halves of the consumers above (see Renderer::GetBindingLayout / GetPipelineRouteFill).
        [[nodiscard]] virtual RDG::ShaderBindingLayout GetBindingLayout( const Shader& shader ) const = 0;
        [[nodiscard]] virtual RDG::OtherRouteFill
        GetPipelineRouteFill( const ComputePipeline& pipeline ) const = 0;
        // The PassBindings route of RenderMesh (see Renderer::RenderMesh( const RDG::PassBindings&, ... )).
        [[nodiscard]] virtual Common::BoolResultStr
        RenderMesh( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline, const Mesh& mesh,
                    const glm::mat4& transform, const MaterialExecutor& material, std::span<const std::byte> push,
                    uint32_t instanceCount, uint32_t firstInstance, SubmeshMask hiddenSubmeshMask,
                    uint32_t lodLevel ) = 0;
        // RenderMesh with its counts in a graph buffer (see Renderer::RenderMeshIndirect).
        [[nodiscard]] virtual Common::BoolResultStr
        RenderMeshIndirect( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline, const Mesh& mesh,
                            const glm::mat4& transform, const MaterialExecutor& material,
                            std::span<const std::byte> push, RDG::BufferRef args, uint64_t offset,
                            SubmeshMask hiddenSubmeshMask, MeshDrawArgumentsOrder order ) = 0;
        // One IndirectGroup of a binned pass: @p pipeline's pass state (no per-draw material: binned materials are
        // bindless rows; draw.Material is the view's carrier of the scene blocks), @p geometry's vertex / stream /
        // index buffers (the group's shared binding: one bind per pipeline switch), then the indirect draws @p
        // draw names. No push block is written: a binned draw reads its per-draw state from GpuScene through
        // gl_InstanceIndex.
        [[nodiscard]] virtual Common::BoolResultStr
        RenderMeshIndirectGroup( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline,
                                 const MeshGeometryBinding& geometry, const MeshIndirectGroupDraw& draw ) = 0;
        // One material group of the VG raster (ClusterIndirectGroupDraw): @p pipeline's pass state (the group's
        // `.VirtualGeometry` cell), the Rows stream at binding 2, one vkCmdDrawIndirect. Refused: a pipeline that
        // does not read the instance id stream, arguments past the end of their buffer.
        [[nodiscard]] virtual Common::BoolResultStr
        RenderClusterIndirectGroup( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline,
                                    const ClusterIndirectGroupDraw& draw ) = 0;

        // Transition a storage image to GENERAL for compute writes in the current frame command buffer,
        // making prior graphics (color/shader) writes visible to compute. Pair with ComputeImageEndWrite.
        // Takes Image*, not Image2D*: a compute pass writes volumes as well as 2D targets, and the aspect
        // mask comes from the image's format rather than from its dimensionality.
        virtual void ComputeImageBeginWrite( Image* image ) = 0;
        // Transition the storage image back to SHADER_READ_ONLY for later sampling (e.g. tonemap),
        // making the compute writes visible to the fragment stage.
        virtual void ComputeImageEndWrite( Image* image ) = 0;

        // Present an image that a graphics pass produced to a compute SAMPLER, and hand it back
        // afterwards. This is what lets a compute pass read the scene DEPTH attachment: depth lives in
        // DEPTH_STENCIL_ATTACHMENT_OPTIMAL, ComputePipeline::SetInput binds the layout of the declared
        // access, so the image must actually be in that sampled layout when the dispatch runs — a
        // depth-attachment layout in a COMBINED_IMAGE_SAMPLER is a validation error. EndRead
        // restores the image's default layout so the owning framebuffer can keep using it as an
        // attachment next frame. Works for any format — the aspect is derived, never assumed to be COLOR.
        virtual void ComputeImageBeginRead( Image* image ) = 0;
        virtual void ComputeImageEndRead( Image* image )   = 0;

        // Set the scissor rectangle (framebuffer pixels, top-left origin) on the current command buffer —
        // used by the 2D batcher to clip UI children to a parent rect. Reset by passing the full viewport;
        // every BeginRenderPass also resets it. No-op outside a recording command buffer.
        virtual void SetScissor( int32_t x, int32_t y, uint32_t width, uint32_t height ) = 0;

        // DRAW INTO ONE RECTANGLE OF THE OPEN RENDER PASS'S TARGET (framebuffer pixels, top-left origin): the
        // viewport (the engine's negative-height convention, so +Y stays up inside the rectangle) and the scissor
        // both set to it. A shadow atlas tile is drawn this way (MeshRenderer::LocalShadowPasses). No-op outside a
        // recording command buffer.
        virtual void SetViewportRegion( int32_t x, int32_t y, uint32_t width, uint32_t height ) = 0;

        // Clear ONE RECTANGLE of every attachment of the open render pass on @p framebuffer: colour attachments to
        // @p color, the depth attachment to @p depth (and its stencil, when the format has one, to 0). The rest
        // of the target keeps what it holds - an atlas tile is cleared without touching its neighbours' cached
        // depth. No-op outside a recording command buffer.
        virtual void ClearRegion( const Framebuffer& framebuffer, int32_t x, int32_t y, uint32_t width,
                                  uint32_t height, const glm::vec4& color, float depth ) = 0;

        // Copy the DEPTH aspect of @p src into @p dst (same format + extent). Must be called OUTSIDE any
        // render pass. Used by the Deferred path to resolve the G-buffer depth into the scene target depth
        // so depth-tested overlays (grid, colliders) occlude against the static geometry that the deferred
        // composite never wrote to the target depth.
        // The caller brings src into TRANSFER_SRC and dst into TRANSFER_DST (the frame graph's Copy node
        // does); a size or sample-count mismatch is refused, named, and returned.
        virtual Common::BoolResultStr CopyDepthImage( Image2D* src, Image2D* dst ) = 0;
        // Copy the DEPTH aspect of the graph texture @p source (one layer) into layer @p targetLayer of the graph
        // texture @p target, inside the exec of the Copy node @p context that declared @p source CopySrc and
        // that layer of @p target CopyDst (the sun's static cascade cache into its layer of the cascade array).
        // Same extent and sample count, else refused and named.
        virtual Common::BoolResultStr CopyGraphDepthToLayer( const RDG::PassContext& context,
                                                             RDG::TextureRef source, RDG::TextureRef target,
                                                             uint32_t targetLayer ) = 0;
        // Copy the tightly packed texels of the graph buffer @p source into mip 0, layer 0 of the colour graph
        // texture @p target, inside the exec of the Copy node @p context that declared @p source CopySrc and
        // @p target CopyDst (UE: a texture upload recorded on the graph's command list; no submit, no wait). The
        // buffer must hold at least Width x Height texels of the target's format, else refused and named.
        virtual Common::BoolResultStr CopyGraphBufferToTexture( const RDG::PassContext& context,
                                                                RDG::BufferRef          source,
                                                                RDG::TextureRef         target ) = 0;

        virtual void                         ResizeWindowEvent( uint32_t width, uint32_t height ) = 0;
        virtual void                         WaitDeviceIdle()                                     = 0;
        virtual std::shared_ptr<Framebuffer> GetCompositeFramebuffer() const                      = 0;

    public:
        static RendererAPIType GetAPIType()
        {
            return s_RenderingAPI;
        }

    protected:
        std::weak_ptr<Window>         m_Window;
        static inline RendererAPIType s_RenderingAPI = RendererAPIType::Vulkan;
    };

} // namespace Desert::Graphic
