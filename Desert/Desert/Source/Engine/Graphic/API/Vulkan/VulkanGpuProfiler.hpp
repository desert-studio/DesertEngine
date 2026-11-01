#pragma once

#include <vulkan/vulkan.h>

#include <Common/Core/DevInstruments.hpp>
#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/GpuTimestampLayout.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// THE WHOLE CLASS IS THE INSTRUMENT, so the whole class is behind the boundary — there is no useful
// "empty VulkanGpuProfiler" to leave standing. Under Shipping `VulkanRendererAPI` has no member of this
// type and makes none of the four calls (VulkanRenderer.hpp / .cpp), which is what keeps a query pool
// from being created and a timestamp pair per pass per view from being written on a player's machine
// for a panel that is not in the binary either.
#if DESERT_DEV_INSTRUMENTS

namespace Desert::Graphic::API::Vulkan
{
    // Per-pass GPU time, measured with VkQueryPools of timestamps and published into the SAME profiler
    // rows the CPU scopes use — so one panel shows what a pass cost on each side of the bus.
    //
    // Three things decide the shape of this class:
    //
    // 1. ONE POOL PER FRAME IN FLIGHT, NO VIEW DIMENSION. The editor runs any number of live SceneRenderers
    //    (viewport, asset previews, thumbnails) into one frame (several command buffers: see the constructor).
    //    Queries are handed out linearly in the order scopes open, so views need no block of their own and there
    //    is no ceiling on how many the profiler sees; nesting is kept apart per view by the ActiveViewScope's
    //    target (see GpuScopeRecorder). A pool that proves too small drops the extra scopes for that frame and is
    //    replaced — by high-water mark, geometrically — the next time its frame index begins, never while
    //    recording: a profiler that reallocates mid-frame measures its own allocator. The replaced pool goes
    //    to the allocator's deferred deletion queue like every other per-frame GPU object.
    //
    // 2. RESULTS ARE READ MaxFramesInFlight FRAMES LATE, AND NEVER WAITED ON.
    // VulkanRendererAPI::PresentFinalImage()
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
        // @p recordingTarget is the renderer's recording target (VulkanRendererAPI::m_CurrentCommandBuffer),
        // read at every write and never copied. The frame is not one command buffer: ExecuteGraph ends the
        // buffer BeginFrame armed, the graph records each pipe segment into its own, and a fresh one is armed
        // after it. A timestamp is a command like any other, so it goes where the recording is going at the
        // moment it is written — a pass scope into its segment's buffer, a scope that spans a graph opens in
        // the buffer before it and closes in the one after (all of them submitted in order, after the reset
        // at the head of the frame). A copy taken at BeginFrame is what wrote into an ended buffer.
        explicit VulkanGpuProfiler( const VkCommandBuffer& recordingTarget ) : m_RecordingTarget( recordingTarget )
        {
        }
        ~VulkanGpuProfiler() override;

        VulkanGpuProfiler( const VulkanGpuProfiler& )            = delete;
        VulkanGpuProfiler& operator=( const VulkanGpuProfiler& ) = delete;

        // Creates one pool per frame in flight and installs this as the profiler's sink. A device without
        // usable timestamps leaves the sink uninstalled, and every GPU column simply stays empty.
        void Init();
        void Shutdown();

        [[nodiscard]] bool IsActive() const
        {
            return m_Active;
        }

        // --- frame lifecycle, driven by VulkanRendererAPI ------------------------------------------------

        // Right after the frame's first command buffer is armed: resolve the queries this frame index left
        // behind the last time it was used, grow its pool if any frame has outgrown it, then reset the pool
        // so this frame can write into it.
        void BeginFrame();
        // Right before the frame's last command buffer is ended: closes the whole-frame bracket.
        void EndFrame();

        // --- IGpuProfilerSink ---------------------------------------------------------------------------

        int32_t BeginScope( const char* name ) override;
        void    EndScope( int32_t handle ) override;
        void    SetQueue( Common::Profiling::GpuQueue queue ) override;

        int32_t BeginDrawScope( std::string_view pass, std::string_view mesh, std::string_view material ) override;
        void    EndDrawScope( int32_t handle, const Common::Profiling::DrawSplitCounts& counts ) override;

    private:
        // One draw group of the split as recorded: its labels (copied - read MaxFramesInFlight frames later)
        // and what the CPU counted while it recorded.
        struct DrawScope
        {
            std::string                        Pass;
            std::string                        Mesh;
            std::string                        Material;
            Common::Profiling::DrawSplitCounts Counts;
            bool                               Ended = false;
        };

        struct FrameQueries
        {
            VkQueryPool      Pool       = VK_NULL_HANDLE;
            uint32_t         QueryCount = 0;
            GpuScopeRecorder Recorder;
            bool             FrameTotalWritten = false; // queries 0/1 were written and not yet resolved

            // THE MESH DRAW SPLIT's own pool (Profiler::MeshDrawSplit): group i = queries 2i / 2i+1. Created
            // the first frame the split is on, never while it is off - off costs no pool, reset or query.
            VkQueryPool            DrawPool       = VK_NULL_HANDLE;
            uint32_t               DrawQueryCount = 0;
            std::vector<DrawScope> DrawScopes;            // the groups that got a pair this frame, in order
            uint32_t               DrawRequested = 0;     // groups asked for, timed or not (the high-water)
            bool                   DrawWritten   = false; // the pool was reset this frame and not yet resolved
        };

        [[nodiscard]] VkQueryPool CreatePool( uint32_t queryCount ) const;
        // Replaces @p frame's pool when the high-water mark no longer fits it. Only called where nothing
        // is recording into that pool and its previous contents have been resolved.
        void GrowIfNeeded( FrameQueries& frame );
        void Resolve( FrameQueries& frame );
        void PrepareDrawPool( FrameQueries& frame, VkCommandBuffer commandBuffer );
        void ResolveDraws( FrameQueries& frame );
        // The Unaccounted rows (Profiler.hpp, kGpuUnaccountedGraphicsScope): what of each queue's timeline no
        // scope covered, and the named gaps of the graphics one. Reads m_ResultScratch as Resolve left it.
        void PublishUnaccounted( const std::vector<GpuScopeRecorder::Scope>& scopes );

        VkDevice m_Device = VK_NULL_HANDLE;

        bool m_Active = false;
        /// Nanoseconds per tick, straight from VkPhysicalDeviceLimits::timestampPeriod.
        double m_PeriodNs = 0.0;

        /// Indexed by frame in flight.
        std::vector<FrameQueries> m_Frames;
        /// The most scopes any single frame has asked for, refused ones included. Every frame's pool is
        /// grown to fit it on that frame's next turn.
        uint32_t m_HighWaterScopes = 0;
        uint32_t m_HighWaterDraws  = 0; // the most draw-split groups one frame asked for

        /// The frame recording right now; null between EndFrame and BeginFrame, which is how a scope
        /// entered outside recording knows to do nothing.
        FrameQueries* m_Recording = nullptr;
        /// The renderer's recording target (see the constructor); VK_NULL_HANDLE while nothing records.
        const VkCommandBuffer& m_RecordingTarget;

        /// Scratch for vkGetQueryPoolResults: {value, availability} per query, reused every frame.
        std::vector<uint64_t> m_ResultScratch;
        // The last pass-order trace Resolve logged (see there); a new line only when the order changes.
        std::string m_LastLoggedOrder;
        /// Per-scope working values during Resolve. Members rather than locals so their capacity survives
        /// between frames instead of being rebuilt from nothing every time. -1 in m_Inclusive marks a
        /// scope whose two queries did not both land.
        std::vector<double>  m_Inclusive;
        std::vector<double>  m_Self;
        std::vector<int32_t> m_Parents;
        std::vector<GpuInterval> m_Intervals;
    };
} // namespace Desert::Graphic::API::Vulkan

#endif // DESERT_DEV_INSTRUMENTS
