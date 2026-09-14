#pragma once

#include <Engine/Graphic/RendererAPI.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanGpuProfiler.hpp>

#include <vulkan/vulkan.h>

#include <string>
#include <unordered_set>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanRendererAPI : public RendererAPI
    {
    public:
        explicit VulkanRendererAPI( const std::shared_ptr<Window>& window ) : RendererAPI( window )
        {
        }
        virtual void Init() override;
        virtual void Shutdown() override;

        [[nodiscard]] virtual Common::BoolResultStr BeginFrame() override;
        [[nodiscard]] virtual Common::BoolResultStr EndFrame() override;
        [[nodiscard]] virtual Common::BoolResultStr PresentFinalImage() override;
        [[nodiscard]] virtual Common::BoolResultStr BeginRenderPass( const RenderPass* renderPass,
                                                                     bool              clearFrame ) override;
        virtual Common::BoolResultStr               BeginSwapChainRenderPass() override;
        [[nodiscard]] virtual Common::BoolResultStr EndRenderPass() override;

        virtual void BeginDebugLabel( const char* name ) override;
        virtual void EndDebugLabel() override;
        
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

        virtual void DispatchComputeCull( const ComputePipeline* pipeline, uint32_t groupCountX,
                                          uint32_t groupCountY, uint32_t groupCountZ ) override;

        virtual void ComputeImageBeginWrite( Image* image ) override;
        virtual void ComputeImageEndWrite( Image* image ) override;
        virtual void ComputeImageBeginRead( Image* image ) override;
        virtual void ComputeImageEndRead( Image* image ) override;

        virtual void CopyDepthImage( Image2D* src, Image2D* dst ) override;
        virtual void SetScissor( int32_t x, int32_t y, uint32_t width, uint32_t height ) override;

        virtual void ResizeWindowEvent( uint32_t width, uint32_t height ) override;
        virtual void WaitDeviceIdle() override;

        virtual std::shared_ptr<Framebuffer> GetCompositeFramebuffer() const override;

        // `VkCommandBuffer GetCurrentCmdBuffer() const;` USED TO SIT HERE WITH NO CALLERS AT ALL, and
        // removing it is not tidiness. m_CurrentCommandBuffer being non-null is what every vkCmd* in the
        // implementation reads as "recording", and BeginFrame — the one function the device-lost gate sits
        // in front of — is its only writer. A public getter is a standing invitation to record from
        // outside that invariant. Nothing wanted it; nothing gets it. (DeviceLostCensus asserts the
        // single-writer half.)

    private:
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
        VkCommandBuffer m_CurrentCommandBuffer = nullptr;

        /// Debug names already reported by BindGraphicsPipeline. Written only from the render thread's
        /// recording path, which is the only caller of every submit entry point above.
        std::unordered_set<std::string> m_WarnedUnbuiltPipelines;

        /// Owned outright rather than reached through a global: there is one renderer API and the query
        /// pool's lifetime is exactly its lifetime. The profiling macros find it through the sink the
        /// profiler holds, so nothing else needs a pointer to it.
        VulkanGpuProfiler m_GpuProfiler;

        std::weak_ptr<Framebuffer> m_CompositeFramebuffer;
    };

} // namespace Desert::Graphic::API::Vulkan
