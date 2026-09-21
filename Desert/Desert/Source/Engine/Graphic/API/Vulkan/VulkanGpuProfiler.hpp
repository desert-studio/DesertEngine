#pragma once

#include <vulkan/vulkan.h>

#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/GpuTimestampLayout.hpp>

#include <cstdint>
#include <string>
#include <vector>

// THE WHOLE CLASS IS THE INSTRUMENT, so the whole class is behind the boundary — there is no useful
// "empty VulkanGpuProfiler" to leave standing. Under Shipping `VulkanRendererAPI` has no member of this
// type and makes none of the four calls (VulkanRenderer.hpp / .cpp), which is what keeps a query pool
// from being created and 64 timestamp pairs per (frame x slot) from being written on a player's machine
// for a panel that is not in the binary either.
#if DESERT_DEV_INSTRUMENTS

namespace Desert::Graphic::API::Vulkan
{
    // Per-pass GPU time, measured with a VkQueryPool of timestamps and published into the SAME profiler
    // rows the CPU scopes use — so one panel shows what a pass cost on each side of the bus.
    //
    // Three things decide the shape of this class:
    //
    // 1. PER (FRAME x RENDERER SLOT). A query is a per-frame GPU resource written during recording and read
    //    later, which is exactly what Docs/RENDERER_FRAME_STATE.md is about. The editor runs several live
    //    SceneRenderers (viewport, asset thumbnails, previews) and they record into ONE command buffer in
    //    one frame; a pool keyed by frame alone would have the preview's "VolumetricClouds" overwrite the
    //    viewport's. The slot comes from EngineContext::GetActiveRendererSlot(), the same ambient read
    //    every other per-frame resource uses.
    //
    // 2. RESULTS ARE READ MaxFramesInFlight FRAMES LATE, AND NEVER WAITED ON. VulkanQueue::Present()
    //    already does vkWaitForFences on the frame index it is about to reuse, so by the time frame index f
    //    begins recording again, everything the GPU was asked to do the last time f was used has completed
    //    and its queries are readable. Resolving right there costs nothing and cannot stall: the wait has
    //    already happened for other reasons. Reading in the same frame would mean idling the GPU, which
    //    would make the profiler the most expensive pass in the frame it is measuring.
    //
    // 3. BOTH TIMESTAMPS AT BOTTOM_OF_PIPE. A begin marker at TOP_OF_PIPE fires as soon as prior work has
    //    *started*, so every pass would appear to begin at once and the parts would sum to far more than
    //    the whole. At BOTTOM_OF_PIPE a scope measures "from when everything before me had finished, to
    //    when I had finished" — the parts then tile the frame and the sum is checkable against it, which
    //    is the entire point of a breakdown. Where passes genuinely overlap this attributes the overlap to
    //    the later one; the engine barriers between dispatches anyway, so there is little to attribute.
    class VulkanGpuProfiler final : public Common::Profiling::IGpuProfilerSink
    {
    public:
        // Scopes per (frame x slot). Two queries each. The frame has ~30 pass-level scopes today, so this
        // is roughly double headroom; overflow warns once and drops the extra scopes rather than growing,
        // because a profiler that reallocates mid-frame measures its own allocator.
        static constexpr uint32_t kMaxScopesPerFrameSlot = 64;

        ~VulkanGpuProfiler() override;

        // Creates the pool and installs this as the profiler's sink. A device without usable timestamps
        // leaves the sink uninstalled, and every GPU column simply stays empty.
        void Init();
        void Shutdown();

        [[nodiscard]] bool IsActive() const
        {
            return m_Active;
        }

        // --- frame lifecycle, driven by VulkanRendererAPI ------------------------------------------------

        // Right after vkBeginCommandBuffer: resolve the queries this frame index left behind the last time
        // it was used, then reset the whole range so this frame can write into it.
        void BeginFrame( VkCommandBuffer commandBuffer );
        // Right before vkEndCommandBuffer: closes the whole-frame bracket.
        void EndFrame( VkCommandBuffer commandBuffer );

        // --- IGpuProfilerSink ---------------------------------------------------------------------------

        int32_t BeginScope( const char* name ) override;
        void    EndScope( int32_t handle ) override;

    private:
        /// One spelling of "no enclosing scope", shared with the self-time arithmetic that reads it.
        static constexpr int32_t kNoParent = kGpuNoParent;

        struct ScopeRecord
        {
            std::string Name;
            uint32_t    QueryBase = 0; // begin at QueryBase, end at QueryBase + 1
            // Index of the enclosing scope in the same (frame x slot), or kNoParent. Passes nest — the
            // cloud march sits inside "Clouds: ExecuteInFrame" inside "VolumetricClouds" — so without this
            // the only total available is one that counts the same microseconds three times.
            int32_t ParentIndex = kNoParent;
        };

        struct FrameSlotState
        {
            std::vector<ScopeRecord> Scopes;
            /// Indices of the scopes currently open, innermost last. RAII guarantees it unwinds.
            std::vector<int32_t> OpenStack;
            bool                 Pending = false; // holds queries written but not yet resolved
        };

        void Resolve( uint32_t frameIndex );

        [[nodiscard]] uint32_t SlotQueryBase( uint32_t frameIndex, uint32_t slot ) const;
        [[nodiscard]] uint32_t FrameTotalQueryBase( uint32_t frameIndex ) const;

        VkQueryPool m_QueryPool = VK_NULL_HANDLE;
        VkDevice    m_Device    = VK_NULL_HANDLE;

        bool m_Active = false;
        /// Nanoseconds per tick, straight from VkPhysicalDeviceLimits::timestampPeriod.
        double m_PeriodNs = 0.0;

        uint32_t m_FramesInFlight    = 0;
        uint32_t m_QueriesPerFrame   = 0;
        uint32_t m_TotalQueries      = 0;
        bool     m_WarnedOverflow    = false;
        bool     m_FrameTotalWritten = false;

        /// Indexed [frameIndex * kMaxRendererSlots + slot].
        std::vector<FrameSlotState> m_State;
        /// The command buffer the current frame is recording into; null between EndFrame and BeginFrame,
        /// which is how a scope entered outside recording knows to do nothing.
        VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;

        /// Scratch for vkGetQueryPoolResults: {value, availability} per query, reused every frame.
        std::vector<uint64_t> m_ResultScratch;
        /// Per-scope working values during Resolve. Members rather than locals so their capacity survives
        /// between frames instead of being rebuilt from nothing every time. -1 in m_Inclusive marks a
        /// scope whose two queries did not both land.
        std::vector<double>  m_Inclusive;
        std::vector<double>  m_Self;
        std::vector<int32_t> m_Parents;
    };
} // namespace Desert::Graphic::API::Vulkan

#endif // DESERT_DEV_INSTRUMENTS
