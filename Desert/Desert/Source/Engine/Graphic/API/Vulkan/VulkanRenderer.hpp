#pragma once

#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanGpuProfiler.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRecordContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRdgTransient.hpp> // m_RdgTransients is owned here
#include <Engine/Graphic/API/Vulkan/VulkanFrameLoop.hpp>    // m_FrameLoop is owned here

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include <vulkan/vulkan.h>

#include <string>
#include <unordered_set>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanRendererAPI : public RendererAPI
    {
    public:
        // An engine image's graph handle, released or resized: kept until every frame that may reference its
        // views has completed (VulkanRdgPool::Retire). Before any graph executed, nothing references it.
        void RetireGraphTexture( std::shared_ptr<RDG::IPhysicalTexture> texture );

        explicit VulkanRendererAPI( const std::shared_ptr<Window>& window ) : RendererAPI( window )
        {
        }
        virtual void Init() override;
        virtual void Shutdown() override;

        [[nodiscard]] virtual Common::BoolResultStr BeginFrame() override;
        // The command buffer the frame records into NOW. It changes at every ExecuteGraph (the frame is split
        // there), so a caller asks for it when it records and never keeps it.
        [[nodiscard]] VkCommandBuffer GetCurrentCommandBuffer() const
        {
            return RecordingTarget();
        }
        [[nodiscard]] virtual Common::BoolResultStr EndFrame() override;
        [[nodiscard]] virtual Common::BoolResultStr PresentFinalImage() override;
        [[nodiscard]] virtual Common::BoolResultStr BeginRenderPass( const RenderPass* renderPass,
                                                                     bool              clearFrame ) override;
        [[nodiscard]] virtual Common::BoolResultStr EndRenderPass() override;

        virtual void BeginDebugLabel( const char* name ) override;
        virtual void EndDebugLabel() override;
        Common::BoolResultStr ExecuteGraph( RDG::Builder& graph ) override;
        Common::BoolResultStr ImportImage( const std::shared_ptr<Image>& image,
                                           RDG::ExternalTexture&         into ) override;
        Common::BoolResultStr ImportBackBuffer( RDG::ExternalTexture& into ) override;
        Common::BoolResultStr ImportBuffer( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer,
                                            RDG::ExternalBuffer&                                   into ) override;
        Common::BoolResultStr ImportVertexBuffer( const std::shared_ptr<VertexBuffer>& buffer,
                                                  RDG::ExternalBuffer&                 into ) override;
        Common::BoolResultStr ImportVertexBufferRange( const std::shared_ptr<VertexBuffer>& buffer, uint64_t offset,
                                                       uint64_t bytes, RDG::ExternalBuffer& into ) override;
        // VulkanSharedGpuMemory.hpp: refused unless CapabilityCatalog::Sharing is Win32 or Fd.
        [[nodiscard]] Common::ResultStr<std::shared_ptr<SharedGpuBuffer>>
        CreateSharedBuffer( std::string_view name, uint64_t bytes ) override;
        [[nodiscard]] Common::ResultStr<std::shared_ptr<SharedGpuTimeline>>
        CreateSharedTimeline( std::string_view name ) override;

        void InvalidateGeometryBinding() override
        {
            ForgetBoundState();
        }

        virtual void SubmitPulled( const GraphicsPipeline* pipeline, uint32_t vertexCount, float lineWidth,
                                   const MaterialExecutor* materialExecutor ) override;

        Common::BoolResultStr DispatchCompute( const RDG::PassBindings& bindings, const ComputePipeline& pipeline,
                                               uint32_t groupCountX, uint32_t groupCountY,
                                               uint32_t groupCountZ ) override;
        Common::BoolResultStr DispatchComputeIndirect( const RDG::PassBindings& bindings,
                                                       const ComputePipeline& pipeline, RDG::BufferRef args,
                                                       uint64_t offset ) override;
        Common::BoolResultStr TraceRays( const RDG::PassBindings& bindings, const RayTracingPipeline& pipeline,
                                         const RayTracing::RayTracingHitGroupTable& hits, uint32_t width,
                                         uint32_t height ) override;
        Common::BoolResultStr DrawProcedural( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline,
                                              const MaterialExecutor* material, std::span<const std::byte> push,
                                              uint32_t vertexCount, uint32_t instanceCount ) override;
        Common::BoolResultStr DrawProceduralIndirect( const RDG::PassBindings&   bindings,
                                                      const GraphicsPipeline&    pipeline,
                                                      const MaterialExecutor*    material,
                                                      std::span<const std::byte> push, RDG::BufferRef args,
                                                      uint64_t offset ) override;
        Common::BoolResultStr DrawProceduralIndexed( const RDG::PassBindings&   bindings,
                                                     const GraphicsPipeline&    pipeline,
                                                     const MaterialExecutor*    material,
                                                     std::span<const std::byte> push, IndexBuffer& indexBuffer,
                                                     std::span<const ProceduralIndexedRange> ranges ) override;
        Common::BoolResultStr DrawIndexed( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline,
                                           const MaterialExecutor* material, VertexBuffer& vertexBuffer,
                                           IndexBuffer& indexBuffer, uint32_t indexCount,
                                           uint32_t firstIndex ) override;
        [[nodiscard]] RDG::ShaderBindingLayout GetBindingLayout( const Shader& shader ) const override;
        [[nodiscard]] std::shared_ptr<const RDG::OtherRouteFill>
        GetPipelineRouteFill( const ComputePipeline& pipeline ) const override;
        Common::BoolResultStr RenderMesh( const RDG::PassBindings& bindings, const GraphicsPipeline& pipeline,
                                          const Mesh& mesh, const glm::mat4& transform,
                                          const MaterialExecutor& material, std::span<const std::byte> push,
                                          uint32_t instanceCount, uint32_t firstInstance,
                                          SubmeshMask hiddenSubmeshMask, uint32_t lodLevel ) override;
        Common::BoolResultStr RenderMeshIndirect( const RDG::PassBindings& bindings,
                                                  const GraphicsPipeline& pipeline, const Mesh& mesh,
                                                  const glm::mat4& transform, const MaterialExecutor& material,
                                                  std::span<const std::byte> push, RDG::BufferRef args,
                                                  uint64_t offset, SubmeshMask hiddenSubmeshMask,
                                                  MeshDrawArgumentsOrder order, uint32_t groups ) override;
        Common::BoolResultStr RenderMeshIndirectCount( const RDG::PassBindings& bindings,
                                                       const GraphicsPipeline& pipeline, const Mesh& mesh,
                                                       const glm::mat4&           transform,
                                                       const MaterialExecutor&    material,
                                                       std::span<const std::byte> push, RDG::BufferRef args,
                                                       uint64_t offset, RDG::BufferRef count, uint64_t countOffset,
                                                       uint32_t maxDraws, SubmeshMask hiddenSubmeshMask ) override;
        Common::BoolResultStr RenderMeshIndirectGroup( const RDG::PassBindings&     bindings,
                                                       const GraphicsPipeline&      pipeline,
                                                       const MeshGeometryBinding&   geometry,
                                                       const MeshIndirectGroupDraw& draw ) override;
        Common::BoolResultStr RenderClusterIndirectGroup( const RDG::PassBindings&        bindings,
                                                          const GraphicsPipeline&         pipeline,
                                                          const ClusterIndirectGroupDraw& draw ) override;

        virtual void ComputeImageBeginWrite( Image* image ) override;
        virtual void ComputeImageEndWrite( Image* image ) override;
        virtual void ComputeImageBeginRead( Image* image ) override;
        virtual void ComputeImageEndRead( Image* image ) override;

        Common::BoolResultStr CopyDepthImage( Image2D* src, Image2D* dst ) override;
        Common::BoolResultStr CopyGraphDepthToLayer( const RDG::PassContext& context, RDG::TextureRef source,
                                                     RDG::TextureRef target, uint32_t targetLayer ) override;
        Common::BoolResultStr ScrollGraphDepthIntoLayer( const RDG::PassContext& context, RDG::TextureRef source,
                                                         RDG::TextureRef target, uint32_t targetLayer, int32_t shiftX,
                                                         int32_t shiftY, float clearDepth ) override;
        Common::BoolResultStr CopyGraphLayerToDepth( const RDG::PassContext& context, RDG::TextureRef source,
                                                     uint32_t sourceLayer, RDG::TextureRef target ) override;
        Common::BoolResultStr CopyGraphBufferToTexture( const RDG::PassContext& context, RDG::BufferRef source,
                                                        RDG::TextureRef target ) override;
        virtual void SetScissor( int32_t x, int32_t y, uint32_t width, uint32_t height ) override;
        virtual void          SetViewportRegion( int32_t x, int32_t y, uint32_t width, uint32_t height ) override;
        virtual void          ClearRegion( const Framebuffer& framebuffer, int32_t x, int32_t y, uint32_t width,
                                           uint32_t height, const glm::vec4& color, float depth ) override;

        virtual void ResizeWindowEvent( uint32_t width, uint32_t height ) override;
        virtual void WaitDeviceIdle() override;

        virtual std::shared_ptr<Framebuffer> GetCompositeFramebuffer() const override;

        // `VkCommandBuffer GetCurrentCmdBuffer() const;` USED TO SIT HERE WITH NO CALLERS AT ALL, and
        // removing it is not tidiness. m_CurrentCommandBuffer being non-null is what every vkCmd* in the
        // implementation reads as "recording", and its writers are the fixed set documented at the field.
        // A public getter is a standing invitation to record from outside that invariant. Nothing wanted
        // it; nothing gets it. (DeviceLostCensus asserts the writer set.)

    private:
        // EVERY DRAW IN THIS FILE GOES THROUGH THESE TWO, AND THAT IS ASSERTED RATHER THAN INTENDED.
        //
        // Six submit paths reach a `vkCmdDraw*`. A counter incremented at six call sites is a counter that
        // will be right at five of them — and nothing in a frame reveals a draw that was not counted, which
        // is exactly the kind of instrument this project keeps finding answers a different question than the
        // one asked. So the vk calls are wrapped, and `Desert/Tests/Engine/DrawCounterFunnel` asserts over
        // the SOURCE TEXT that `vkCmdDraw` and `vkCmdDrawIndexed` appear nowhere else in it.
        //
        // Why here and not in a render pass: an ISM batch is ONE draw with many instances, and only the
        // recording site knows the instance count. Counting passes would report the batch as one of each.
        // Everything an in-graph draw records before its draw call (DrawProcedural, DrawIndexed): the pass's
        // command buffer check, the exec's descriptor sets from @p bindings + the material's written slots, the
        // pipeline, the sets and the push constants. Error when any of it cannot be recorded.
        // Binds @p pipeline, its descriptor sets for this exec and the push constants on the pass's command buffer
        // (the shared part of DispatchCompute and DispatchComputeIndirect); @p cmd receives that command buffer.
        Common::BoolResultStr BindComputePassState( const RDG::PassBindings& bindings,
                                                    const ComputePipeline& pipeline, VkCommandBuffer& cmd );
        // @p drawPush: the draw's own push block (a copy of @p material's with the draw's values over it, built by
        // the caller - RENDER-THREADS M2b, the shared material is not written to record a draw); empty = the
        // material's block as it stands.
        // @p push: Record pushes the chosen block; CallerCoversRange only checks it - the caller records the
        // pipeline layout's WHOLE push range before every draw it makes (WalkMeshSubmeshes: the per-submesh
        // transform over the draw's block), so a push here would be overwritten before any draw reads it.
        enum class PassStatePush : uint8_t
        {
            Record,
            CallerCoversRange,
        };
        Common::BoolResultStr BindGraphicsPassState( const RDG::PassBindings&   bindings,
                                                     const GraphicsPipeline&    pipeline,
                                                     const MaterialExecutor*    material,
                                                     std::span<const std::byte> drawPush, PassStatePush pushMode );
        // Binds @p geometry's vertex buffer (binding 0), its streams or the shared default (binding 1, when
        // @p pipeline's vertex input has it) and its index buffer, each at its byte offset, unless exactly those
        // are bound already for this pipeline on this command buffer (m_BoundGeometry); then the instance id
        // stream @p visibleRows (binding 2, when the pipeline reads a_VisibleRow; refused when it does and none
        // is given). A real bind is counted (GeometryPool::RecordBind, @p shared: the binned group path).
        Common::BoolResultStr BindMeshBuffers( std::string_view pass, const GraphicsPipeline& pipeline,
                                               const MeshGeometryBinding& geometry, VkBuffer visibleRows,
                                               bool shared );
        // RDG-PARALLEL-1: the bound-state caches live in the recording context (VulkanRecordContext.hpp): the frame's
        // (m_FrameRecording, around m_CurrentCommandBuffer) or the graph chunk's bound to this thread, so a chunk
        // recorded on a task worker reads and writes only its own. Recording() is that context, RecordingTarget()
        // the command buffer every entry point below records into.
        using BoundGeometry = VulkanBoundGeometry;
        VulkanRecordContext  m_FrameRecording; // its CommandBuffer is unused: the frame's is m_CurrentCommandBuffer
        [[nodiscard]] VulkanRecordContext& Recording()
        {
            VulkanRecordContext* const chunk = VulkanRecordContext::Bound();
            return chunk != nullptr ? *chunk : m_FrameRecording;
        }
        [[nodiscard]] VkCommandBuffer RecordingTarget() const
        {
            const VulkanRecordContext* const chunk = VulkanRecordContext::Bound();
            return chunk != nullptr ? chunk->CommandBuffer : m_CurrentCommandBuffer;
        }
        // Everything the renderer knows bound on the recording command buffer (geometry, pipeline, sets).
        void ForgetBoundState()
        {
            Recording().ForgetBoundState();
        }
        // The CPU-recorded binned draw's id stream: rows[i] = i, at least @p rows of them (GpuScene.glslh
        // a_VisibleRow; the unculled VisibleInstances list is indexed by firstInstance + instance). Grown, never
        // shrunk, released through the allocator's per-frame deletion queue like DefaultVertexStreams.
        VkBuffer                      VisibleRowIdentity( uint32_t rows );
        std::shared_ptr<VertexBuffer> m_VisibleRowIdentity;
        uint32_t                      m_VisibleRowIdentityRows = 0;
        // RENDER-JOB-CPU-1 (UE FMeshDrawCommand merging: draws of one pipeline, bindings and push are one
        // command): a run of drawn submeshes [First, First + Count) - adjacent in the mesh, none hidden between,
        // one submesh transform, so one push block serves every one of them. FirstDrawn is the first one's index
        // among the drawn submeshes; IndexCount the mesh's index count (0: a mesh without indices).
        struct SubmeshRun
        {
            uint32_t FirstDrawn = 0;
            size_t   First      = 0;
            uint32_t Count      = 0;
            uint32_t IndexCount = 0;
        };
        // Records one run. A view of the caller's recorder, never a copy (it lives through the walk's call):
        // the std::function it replaces heap-allocated the indirect draw's capture list on every mesh draw.
        class SubmeshRunDraw
        {
        public:
            template <typename F>
                requires( !std::is_same_v<std::remove_cvref_t<F>, SubmeshRunDraw> )
            SubmeshRunDraw( F&& recorder ) // NOLINT(google-explicit-constructor): the walk takes the lambda
                 : m_Recorder( static_cast<const void*>( std::addressof( recorder ) ) ),
                   m_Call( []( const void* of, const SubmeshRun& run ) -> Common::BoolResultStr
                           { return ( *static_cast<const std::remove_reference_t<F>*>( of ) )( run ); } )
            {
            }
            Common::BoolResultStr operator()( const SubmeshRun& run ) const
            {
                return m_Call( m_Recorder, run );
            }

        private:
            const void* m_Recorder                                              = nullptr;
            Common::BoolResultStr ( *m_Call )( const void*, const SubmeshRun& ) = nullptr;
        };
        // RenderMesh's walk, shared by the direct and the indirect mesh draw: the pass state, the mesh's vertex,
        // stream and index buffers, then for every run of drawn submeshes (SubmeshRun) its push block (the
        // draw's @p push, transform * submesh.Transform over the first mat4) once before @p draw records the run.
        Common::BoolResultStr WalkMeshSubmeshes( const RDG::PassBindings& bindings,
                                                 const GraphicsPipeline& pipeline, const Mesh& mesh,
                                                 const glm::mat4& transform, const MaterialExecutor& material,
                                                 std::span<const std::byte> push, SubmeshMask hiddenSubmeshMask,
                                                 VkBuffer visibleRows, const SubmeshRunDraw& draw );
        void DrawIndexedCounted( uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                                 int32_t vertexOffset, uint32_t firstInstance );
        void DrawCounted( uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                          uint32_t firstInstance );

        void SetViewportAndScissor( const uint32_t width, const uint32_t height );
        void ClearAttachments( const std::vector<VkClearValue>&    clearValues,
                               const std::shared_ptr<Framebuffer>& framebuffer );

        /**
         * Binds @p pipeline for graphics, or refuses by name and returns false.
         *
         * WHAT IT MAKES TRUE. ShaderService::Register keeps a shader that failed to compile under its
         * NAME on purpose, and tells the artist "every material using it will not draw until it
         * compiles". VulkanPipeline::Invalidate honours the first half — it refuses to build and leaves
         * the handle null — but the second half was nobody's: all six submit paths below then called
         * `vkCmdBindPipeline( ..., VK_NULL_HANDLE )`, which is a validation error and undefined
         * behaviour on the draw that follows. "Will not draw" was a sentence the tree did not keep.
         * This is the one gate every graphics bind in the engine goes through, so it is where the
         * sentence becomes true.
         *
         * Latched by debug name because this is asked once per draw call: a broken shader would
         * otherwise write the same line thousands of times a second and the log would stop being read.
         */
        NO_DISCARD bool BindGraphicsPipeline( const GraphicsPipeline* pipeline );
        // The shared buffer a mesh without vertex streams binds at binding 1 (white, UV1 0,0 in every vertex),
        // grown to hold `capacity` stream vertices (MeshVertexLayout.hpp DefaultVertexStreamsFor).
        // RDG-PARALLEL-1: by value and under m_GrowLock - a chunk on another worker may grow it meanwhile (the
        // replaced buffer stays alive through the allocator's per-frame deletion queue).
        std::shared_ptr<VertexBuffer>        DefaultVertexStreams( uint32_t capacity );
        std::mutex                           m_GrowLock; // DefaultVertexStreams and VisibleRowIdentity
        std::shared_ptr<VertexBuffer>        m_DefaultVertexStreams;
        std::atomic<uint32_t>                m_DefaultVertexStreamsCapacity{ 0 }; // read unlocked by BindMeshBuffers

    private:
        // THE ONE QUESTION EVERY RECORDING ENTRY POINT ASKS. BeginFrame is gated, but a loss can be noted
        // MID-FRAME (an Image2D upload waiting on its fence, a bake): the buffer stayed armed and the rest of
        // the frame kept recording, down to a vkCmdBeginRenderPass on the null framebuffer of an image that
        // failed to create -- a segfault inside MoltenVK after "closing down in order". Disarms on the spot.
        [[nodiscard]] bool IsRecording();

        // THE RECORDING TARGET: the command buffer every recording entry point of this API (every
        // renderer's Record()) writes into. Ownership, one writer per phase of the frame:
        //  - OUTSIDE a graph: the frame. BeginFrame arms the frame's first graphics buffer; ExecuteGraph
        //    ends it before the graph (and clears the field, so nothing records into an ended buffer)
        //    and arms a fresh graphics buffer after the graph. Both sit behind the device-lost gate.
        //  - INSIDE a graph: the graph backend, and nothing else. VulkanRdgBackend calls
        //    SetGraphRecordingTarget at every change of its recording buffer (graph begin, each pipe
        //    segment's begin and end, abandon, TakeSubmissions), so a Record() from a pass lands in the
        //    command buffer of the segment that pass runs on, on that segment's queue.
        // Clearing it to nullptr (a failure, a lost device) is allowed anywhere: it can only stop
        // recording. DeviceLostCensus.OnlyGatedFunctionsCanArmTheCommandBuffer asserts this writer set;
        // RenderGraphVulkan.ARecordThroughTheRecordingTargetLandsInThePassSegmentBuffer proves the
        // in-graph half.
        VkCommandBuffer m_CurrentCommandBuffer = nullptr;
        // The in-graph writer above, bound as the backend's RecordingListener.
        void SetGraphRecordingTarget( VkCommandBuffer commandBuffer );

        // The compatibility key of the render pass this API opened on m_CurrentCommandBuffer (BeginRenderPass),
        // empty between passes. BindGraphicsPipeline resolves the pipeline against it,
        // or against the graph backend's open pass when the draw is recorded inside one.
        std::optional<RdgRenderPassKey> m_OpenRenderPass;

        /// Debug names already reported by BindGraphicsPipeline. RDG-PARALLEL-1: graph chunks record on task
        /// workers, so every insert goes through WarnOnce (true the first time @p key is seen).
        std::unordered_set<std::string> m_WarnedUnbuiltPipelines;
        std::mutex                      m_WarnedLock;
        [[nodiscard]] bool              WarnOnce( std::string key )
        {
            const std::lock_guard<std::mutex> lock( m_WarnedLock );
            return m_WarnedUnbuiltPipelines.insert( std::move( key ) ).second;
        }

        /// Owned outright rather than reached through a global: there is one renderer API and the query
        /// pool's lifetime is exactly its lifetime. The profiling macros find it through the sink the
        /// profiler holds, so nothing else needs a pointer to it.
        ///
        /// Absent in a Shipping build, together with the four calls into it — see
        /// Common/Core/DevInstruments.hpp.
#if DESERT_DEV_INSTRUMENTS
        VulkanGpuProfiler m_GpuProfiler{ m_CurrentCommandBuffer };
#endif

        // The frame-graph executor, made on the first ExecuteGraph (the device exists by then). The pool
        // holds transient images per frame in flight; the backend is re-pointed at the frame's command
        // buffer on every graph.
        VulkanRdgDevice                m_RdgDevice;
        std::unique_ptr<VulkanRdgPool> m_RdgPool;
        // RDG-CONTRACTS A/B frame objects, per frame slot and re-begun by BeginFrame after the slot's fence:
        // transient heaps, per-pass descriptor pools, the frame loop (slot fences + segment command pools and
        // semaphores: the frame's one submission path, windowed or not), and the queues.
        std::unique_ptr<VulkanRdgTransientAllocator> m_RdgTransients;
        std::unique_ptr<VulkanRdgPassDescriptors>    m_RdgDescriptors;
        std::unique_ptr<VulkanFrameLoop>             m_FrameLoop;
        VulkanRdgQueueSet                            m_RdgQueues;
        // DECLARED AFTER the frame objects above: the backend holds raw pointers to m_RdgQueues, m_RdgTransients
        // and m_RdgDescriptors (bound by BeginFrame), so it is destroyed before them.
        std::unique_ptr<VulkanRdgBackend> m_RdgBackend;
        // The frame so far in submission order: the frame command buffer is split at every graph (what was
        // recorded before it is submitted before it), followed by that graph's segment submissions.
        std::vector<VulkanRdgSubmission> m_FrameSubmissions;
        // The index into m_FrameSubmissions at this frame's late acquire (ImportBackBuffer): the first entry that
        // can touch the back buffer. 0 when the image was already held on entry to the frame (the previous
        // frame's submit failed before its present), so the frame's first graphics entry waits, as before.
        size_t m_BackBufferFirstEntry = 0;

        // Makes the RDG frame objects on the first frame and begins them all for the current frame slot.
        Common::BoolResultStr BeginRdgFrame();
    };

} // namespace Desert::Graphic::API::Vulkan
