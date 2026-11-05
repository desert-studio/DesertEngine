#pragma once

#include <atomic>
#include <functional>

#include <Engine/Graphic/API/Vulkan/VulkanRecordContext.hpp>
#include <mutex>
#include <span>
#include <vector>

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
        // @p recordingTarget answers the renderer's recording target (VulkanRendererAPI::RecordingSlot(), the
        // command buffer of the frame slot the calling thread records), asked at every write and never copied.
        // The frame is not one command buffer: ExecuteGraph ends the
        // buffer BeginFrame armed, the graph records each pipe segment into its own, and a fresh one is armed
        // after it. A timestamp is a command like any other, so it goes where the recording is going at the
        // moment it is written — a pass scope into its segment's buffer, a scope that spans a graph opens in
        // the buffer before it and closes in the one after (all of them submitted in order, after the reset
        // at the head of the frame). A copy taken at BeginFrame is what wrote into an ended buffer.
        explicit VulkanGpuProfiler( std::function<VkCommandBuffer()> recordingTarget )
             : m_RecordingTarget( std::move( recordingTarget ) )
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
        // RDG-PARALLEL-1: @p chunks consecutive recording-order ticks for graph chunks about to record in parallel
        // (VulkanRecordContext::GpuOrder of chunk i = the result + i); 0 outside a recording frame.
        [[nodiscard]] uint64_t ReserveChunkOrder( uint32_t chunks );
        // RDG-PARALLEL-3: on the dispatching thread, as a chunk recorded on a worker is replayed in
        // pass order: @p scopes (VulkanRecordContext::GpuScopesMade) take the next recording-order tick, scope k at
        // tick << 24 | k, so the [GpuOrder] trace and the scope tree read in the serial order.
        void StampChunkOrder( std::span<const int32_t> scopes );
        // RDG-PARALLEL-5: on the dispatching thread, as the graph discards a chunk (its passes are recorded again
        // inline): @p scopes (VulkanRecordContext::GpuScopesMade) are retracted, so the frame's [GpuOrder] trace,
        // tables and coverage hold each of those passes once, as its inline scope.
        void RetractChunkScopes( std::span<const int32_t> scopes );
        // RDG-PARALLEL-1-1: on the dispatching thread, as the graph begins its chunks (before any records): where
        // the chunks' scopes hang - the active view's stack on @p queue (the chunks' pipe) and the scope open on
        // it now. The chunks record while the dispatching thread walks the graph, so they never read its stack.
        [[nodiscard]] GpuChunkRootScope ChunkRoot( Common::Profiling::GpuQueue queue );

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
            // Keyed by the profiler row id (Profiler::LocalScopeName): no string per scope.
            GpuScopeRecorder<uint32_t> Recorder;
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
        // Takes @p frame's results and scopes (the caller's thread; the pool and the recorder are reused right
        // after), then hands ResolveTimes to a worker (JobSystem), inline when Submit refuses.
        void Resolve( FrameQueries& frame );
        // THE RESOLVE'S HALF OFF THE CRITICAL PATH (CPU-ONE-SYNC; UE: the GPU profiler's results are processed
        // off the rendering thread). Reads only the copies Resolve took (m_ResolveWords, m_ResolveScopes,
        // m_Parents) and writes the profiler's samples (AddGpuSample: per-thread, any thread). Not reentrant:
        // Resolve and Shutdown wait for the one in flight (m_ResolveBusy) before they touch its members.
        void ResolveTimes() noexcept;
        void WaitResolve() const noexcept;
        void PrepareDrawPool( FrameQueries& frame, VkCommandBuffer commandBuffer );
        void ResolveDraws( FrameQueries& frame );
        // The Unaccounted rows (Profiler.hpp, kGpuUnaccountedGraphicsScope): what of each queue's timeline no
        // scope covered, and the named gaps of the graphics one. Reads m_ResolveWords as Resolve left it.
        void PublishUnaccounted( std::span<const GpuScopeRecorder<uint32_t>::Scope> scopes );
        // Whether row @p nameId is an async segment's (GpuAsyncSegmentLabelStart), decided once per id.
        [[nodiscard]] bool IsSegmentRow( uint32_t nameId );

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
        std::function<VkCommandBuffer()> m_RecordingTarget;
        // RDG-PARALLEL-1: a scope's timestamp goes into the command buffer of the graph chunk bound to the calling
        // thread (VulkanRecordContext), else into the renderer's target; the scope tree and the frame's queries are
        // shared by every recording thread. CPU-ONE-SYNC: a chunk's scope (BeginUnder / EndUnder on a task
        // worker) takes NO lock - it claims its slot atomically and touches only its chunk's list; m_Lock is held
        // by the shared-stack path (one dispatching thread, so uncontended) and the draw split.
        // The renderer's target belongs to the render task while the views record on it (RenderTask, the same rule
        // VulkanRendererAPI::IsRecording follows): a thread with no chunk of its own then gets VK_NULL_HANDLE, the
        // "nothing records" answer every scope entry point already honours. vkCmd* on a buffer another thread is
        // recording breaks Vulkan's external-synchronisation rule; 11-02 an editor panel's DESERT_PROFILE_PASS wrote
        // a timestamp into the frame's buffer while the render thread recorded it, and the driver faulted.
        [[nodiscard]] VkCommandBuffer Target() const
        {
            if ( const VulkanRecordContext* const chunk = VulkanRecordContext::Bound(); chunk != nullptr )
                return chunk->CommandBuffer;
            if ( !OwnsRecordingTarget() )
                return VK_NULL_HANDLE;
            return m_RecordingTarget();
        }
        // True on the render task's thread, and on any thread while no view records on it.
        [[nodiscard]] static bool OwnsRecordingTarget();
        std::mutex            m_Lock;
        std::vector<uint32_t> m_OrderScratch; // the order trace's scopes sorted by Scope::Order

        /// Scratch for vkGetQueryPoolResults: {value, availability} per query, reused every frame.
        std::vector<uint64_t> m_ResultScratch;
        // THE RESOLVE JOB'S INPUT (Resolve -> ResolveTimes): the frame's query words, its scopes and their live
        // parents, copied on the caller's thread so the job outlives the frame's recorder and pool reuse.
        // ResolveDraws keeps m_ResultScratch for itself.
        std::vector<uint64_t>                        m_ResolveWords;
        std::vector<GpuScopeRecorder<uint32_t>::Scope> m_ResolveScopes;
        // True from the Submit until ResolveTimes returned (the worker clears it and notifies).
        std::atomic<bool> m_ResolveBusy{ false };
        // The last pass-order trace Resolve logged (see there); a new line only when the order changes.
        std::string m_LastLoggedOrder;
        // [GpuOrder] (CPU-ONE-SYNC): the order is compared by a hash of (depth, row id) per scope, built with no
        // string; the text is built and logged only under Profiler::GpuOrderTrace (--gpu-order-trace).
        uint64_t m_LastOrderHash = 0;
        // Per row id: 0 unknown, 1 an async segment's row, 2 not (IsSegmentRow).
        std::vector<uint8_t> m_SegmentById;
        /// Per-scope working values during Resolve. Members rather than locals so their capacity survives
        /// between frames instead of being rebuilt from nothing every time. -1 in m_Inclusive marks a
        /// scope whose two queries did not both land.
        std::vector<double>  m_Inclusive;
        std::vector<double>  m_Self;
        std::vector<int32_t> m_Parents;
        std::vector<GpuInterval> m_Intervals;
        // PublishUnaccounted's scratch (CPU-ONE-SYNC): cleared each frame, never reallocated once grown.
        std::vector<int32_t> m_GraphicsScopes;
        std::vector<int32_t> m_AsyncPasses;
        std::vector<int32_t> m_AsyncSegments;
        GpuCoverage          m_Coverage;
        std::string          m_GapName;
    };
} // namespace Desert::Graphic::API::Vulkan

#endif // DESERT_DEV_INSTRUMENTS
