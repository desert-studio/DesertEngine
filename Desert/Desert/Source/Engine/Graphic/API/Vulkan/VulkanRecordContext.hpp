#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    // What a recorder last bound at binding 0 / 1 / the index buffer of one command buffer (VulkanRendererAPI::
    // BindMeshBuffers); reset whenever a buffer is (re)armed and when another path binds binding 0 / the index
    // buffer (DrawIndexed). Pipeline is the GraphicsPipeline's address, compared as an identity only.
    struct VulkanBoundGeometry
    {
        VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
        const void*     Pipeline      = nullptr;
        VkBuffer        Vertices      = VK_NULL_HANDLE;
        VkDeviceSize    VertexOffset  = 0;
        VkBuffer        Streams       = VK_NULL_HANDLE;
        VkDeviceSize    StreamOffset  = 0;
        VkBuffer        Indices       = VK_NULL_HANDLE;
        VkDeviceSize    IndexOffset   = 0;

        [[nodiscard]] bool operator==( const VulkanBoundGeometry& ) const = default;
    };

    // CPU-FRAME-6 (UE: FVulkanCommandListContext's pending graphics state - a bind of what is bound already
    // records nothing): the pipeline and descriptor sets last recorded at the GRAPHICS bind point of
    // CommandBuffer. Vulkan keeps both across render passes and a pipeline bind does not disturb the sets, so a
    // draw whose pipeline handle, or whose layout and set handles, equal these records no bind. Every dynamic
    // state the engine sets (viewport, scissor, line width) is dynamic in every graphics pipeline
    // (VulkanPipeline::m_DynamicStates), so a skipped pipeline bind keeps no stale static state. A secondary
    // command buffer inherits no bound state: On() of a new buffer forgets everything.
    struct VulkanBoundGraphics
    {
        VkCommandBuffer              CommandBuffer = VK_NULL_HANDLE;
        VkPipeline                   Pipeline      = VK_NULL_HANDLE;
        VkPipelineLayout             SetsLayout    = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> Sets; // bound at set 0 on; the capacity is kept across forgets

        // Tracks @p commandBuffer from here: what another buffer held is forgotten.
        void On( VkCommandBuffer commandBuffer )
        {
            if ( CommandBuffer == commandBuffer )
                return;
            Forget();
            CommandBuffer = commandBuffer;
        }
        void ForgetSets()
        {
            SetsLayout = VK_NULL_HANDLE;
            Sets.clear();
        }
        void Forget()
        {
            CommandBuffer = VK_NULL_HANDLE;
            Pipeline      = VK_NULL_HANDLE;
            ForgetSets();
        }
    };

    // RDG-PARALLEL-1 (UE: FVulkanCommandListContext - one per thread that translates a parallel pass set of
    // FRDGBuilder::Execute under r.RDG.ParallelExecute). EVERYTHING A RECORDER KEEPS ABOUT THE COMMAND BUFFER IT
    // RECORDS INTO. The frame has one (VulkanRendererAPI's, around m_CurrentCommandBuffer). A chunk of graph
    // passes recorded on a task worker has its own (VulkanRdgBackend's chunk), bound to that worker thread from
    // the chunk's BeginChunk to its EndChunk. Every recorder that writes commands - VulkanRendererAPI's draw /
    // dispatch / copy entry points, the graph backend's pass bracket, the GPU profiler's timestamps - asks Bound()
    // first and records into the bound context's CommandBuffer when there is one. Nothing in a context is shared
    // with another thread.
    struct VulkanRecordContext
    {
        // The command buffer the context records into now (a chunk: its current secondary; null between two).
        VkCommandBuffer                    CommandBuffer = VK_NULL_HANDLE;
        std::optional<VulkanBoundGeometry> BoundGeometry;
        VulkanBoundGraphics                BoundGraphics;
        std::vector<std::byte>             SubmeshPush; // VulkanRendererAPI::WalkMeshSubmeshes' block, reused
        // VulkanGpuProfiler: the scopes this context has open (a chunk's scopes nest under each other, and the
        // first under the scope the dispatching thread had open), and the context's place in the frame's scope
        // order: GpuOrder is reserved per chunk in pass order before the chunks record (GpuScopeRecorder
        // ReserveOrder), GpuOrderNext numbers the context's scopes within it.
        std::vector<int32_t> GpuScopes;
        uint64_t             GpuOrder     = 0;
        uint32_t             GpuOrderNext = 0;
        // RDG-PARALLEL-3: every scope this context opened (VulkanGpuProfiler::BeginScope), in opening order. The
        // dispatching thread re-stamps their order when it replays the chunk (VulkanGpuProfiler::StampChunkOrder),
        // so the frame's scope order is the replay order - the serial order - not the order chunks were planned.
        std::vector<int32_t> GpuScopesMade;

        void ForgetBoundState()
        {
            BoundGeometry.reset();
            BoundGraphics.Forget();
        }

        // The context bound to the calling thread, or null: the frame's context applies.
        [[nodiscard]] static VulkanRecordContext* Bound() noexcept
        {
            return t_Bound;
        }
        // Binds @p context (null: none) to the calling thread; returns what was bound, for Bind( previous ).
        static VulkanRecordContext* Bind( VulkanRecordContext* context ) noexcept
        {
            VulkanRecordContext* const previous = t_Bound;
            t_Bound                             = context;
            return previous;
        }

    private:
        static inline thread_local VulkanRecordContext* t_Bound = nullptr;
    };
} // namespace Desert::Graphic::API::Vulkan
