#pragma once

#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanGpuProfiler.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRdgTransient.hpp> // m_RdgTransients is owned here

#include <memory>

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
        VkCommandBuffer GetCurrentCommandBuffer() const
        {
            return m_CurrentCommandBuffer;
        }
        [[nodiscard]] virtual Common::BoolResultStr EndFrame() override;
        [[nodiscard]] virtual Common::BoolResultStr PresentFinalImage() override;
        [[nodiscard]] virtual Common::BoolResultStr BeginRenderPass( const RenderPass* renderPass,
                                                                     bool              clearFrame ) override;
        virtual Common::BoolResultStr               BeginSwapChainRenderPass() override;
        [[nodiscard]] virtual Common::BoolResultStr EndRenderPass() override;

        virtual void BeginDebugLabel( const char* name ) override;
        virtual void EndDebugLabel() override;
        Common::BoolResultStr                  ExecuteGraph( RDG::Builder& graph ) override;
        Common::BoolResultStr                  ImportImage( const std::shared_ptr<Image>& image,
                                                            RDG::ExternalTexture&         into ) override;
        Common::BoolResultStr ImportBuffer( const std::shared_ptr<ShaderResources::StorageBuffer>& buffer,
                                            RDG::ExternalBuffer&                                   into ) override;

        virtual void RenderMesh( const GraphicsPipeline* pipeline, const Mesh* mesh, const glm::mat4 transform,
                                 const MaterialExecutor* materialExecutor, uint32_t instanceCount = 1,
                                 uint32_t firstInstance = 0, uint64_t hiddenSubmeshMask = 0,
                                 uint32_t lodLevel = 0 ) override;

        virtual void SubmitFullscreenQuad( const GraphicsPipeline*         pipeline,
                                           const MaterialExecutor* materialExecutor ) override;

        virtual void SubmitIndexed( const GraphicsPipeline* pipeline, VertexBuffer* vertexBuffer,
                                    IndexBuffer* indexBuffer, uint32_t indexCount, uint32_t firstIndex,
                                    const MaterialExecutor* materialExecutor ) override;

        virtual void SubmitLines( const GraphicsPipeline* pipeline, uint32_t vertexCount, float lineWidth,
                                  const MaterialExecutor* materialExecutor ) override;

        virtual void SubmitVertices( const GraphicsPipeline* pipeline, uint32_t vertexCount,
                                     const MaterialExecutor* materialExecutor ) override;

        virtual void DispatchComputeInFrame( const ComputePipeline* pipeline, uint32_t groupCountX,
                                             uint32_t groupCountY, uint32_t groupCountZ ) override;

        virtual void ComputeImageBeginWrite( Image* image ) override;
        virtual void ComputeImageEndWrite( Image* image ) override;
        virtual void ComputeImageBeginRead( Image* image ) override;
        virtual void ComputeImageEndRead( Image* image ) override;

        virtual Common::BoolResultStr CopyDepthImage( Image2D* src, Image2D* dst ) override;
        virtual void SetScissor( int32_t x, int32_t y, uint32_t width, uint32_t height ) override;

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

        // The compatibility key of the render pass this API opened on m_CurrentCommandBuffer (BeginRenderPass /
        // BeginSwapChainRenderPass), empty between passes. BindGraphicsPipeline resolves the pipeline against it,
        // or against the graph backend's open pass when the draw is recorded inside one.
        std::optional<RdgRenderPassKey> m_OpenRenderPass;

        /// Debug names already reported by BindGraphicsPipeline. Written only from the render thread's
        /// recording path, which is the only caller of every submit entry point above.
        std::unordered_set<std::string> m_WarnedUnbuiltPipelines;

        /// Owned outright rather than reached through a global: there is one renderer API and the query
        /// pool's lifetime is exactly its lifetime. The profiling macros find it through the sink the
        /// profiler holds, so nothing else needs a pointer to it.
        ///
        /// Absent in a Shipping build, together with the four calls into it — see
        /// Common/Core/DevInstruments.hpp.
#if DESERT_DEV_INSTRUMENTS
        VulkanGpuProfiler m_GpuProfiler;
#endif

        std::weak_ptr<Framebuffer> m_CompositeFramebuffer;

        // The frame-graph executor, made on the first ExecuteGraph (the device exists by then). The pool
        // holds transient images per frame in flight; the backend is re-pointed at the frame's command
        // buffer on every graph.
        VulkanRdgDevice                   m_RdgDevice;
        std::unique_ptr<VulkanRdgPool>    m_RdgPool;
        std::unique_ptr<VulkanRdgBackend> m_RdgBackend;
        // RDG-CONTRACTS A/B frame objects, per frame slot and re-begun by BeginFrame after the slot's fence:
        // transient heaps, per-pass descriptor pools, segment command pools + semaphores, and the queues.
        std::unique_ptr<VulkanRdgTransientAllocator> m_RdgTransients;
        std::unique_ptr<VulkanRdgPassDescriptors>    m_RdgDescriptors;
        std::unique_ptr<VulkanRdgQueueObjects>       m_RdgQueueObjects;
        VulkanRdgQueueSet                            m_RdgQueues;
        // The frame so far in submission order: the frame command buffer is split at every graph (what was
        // recorded before it is submitted before it), followed by that graph's segment submissions.
        std::vector<VulkanRdgSubmission> m_FrameSubmissions;

        // Makes the RDG frame objects on the first frame and begins them all for the current frame slot.
        Common::BoolResultStr BeginRdgFrame();
    };

} // namespace Desert::Graphic::API::Vulkan
