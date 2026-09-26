#include <Engine/Graphic/API/Vulkan/VulkanGpuProfiler.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/GpuTimestampLayout.hpp>
#include <Engine/Graphic/ViewResources.hpp>

#if DESERT_DEV_INSTRUMENTS

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        // vkGetQueryPoolResults with VK_QUERY_RESULT_WITH_AVAILABILITY_BIT writes two uint64 per query:
        // the value, then a non-zero availability word.
        constexpr uint32_t kWordsPerQuery = 2;
    } // namespace

    VulkanGpuProfiler::~VulkanGpuProfiler()
    {
        Shutdown();
    }

    void VulkanGpuProfiler::Init()
    {
        const auto& caps = EngineContext::GetInstance().GetCapabilities();

        if ( !caps.SupportsTimestampQueries )
        {
            LOG_WARN( "[GpuProfiler] Device reports no timestamp queries — the profiler stays CPU-only." );
            return;
        }
        if ( caps.TimestampPeriodNs <= 0.0f )
        {
            LOG_WARN( "[GpuProfiler] timestampPeriod is {} — without it a tick cannot be turned into a "
                      "time, so GPU timing stays off.",
                      caps.TimestampPeriodNs );
            return;
        }

        const auto device = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        m_Device          = device->GetVulkanLogicalDevice();

        // timestampComputeAndGraphics being true already implies every graphics+compute queue has valid
        // bits, but the count is what says how many of the 64 are meaningful, and a zero here would make
        // every delta garbage rather than merely wrong.
        {
            const auto physical = device->GetPhysicalDevice()->GetVulkanPhysicalDevice();

            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties( physical, &familyCount, nullptr );
            std::vector<VkQueueFamilyProperties> families( familyCount );
            vkGetPhysicalDeviceQueueFamilyProperties( physical, &familyCount, families.data() );

            const uint32_t graphicsFamily = device->GetPhysicalDevice()->GetGraphicsFamily();
            if ( graphicsFamily >= familyCount )
            {
                LOG_WARN( "[GpuProfiler] Graphics queue family {} is outside the {} the driver reports "
                          "here — GPU timing stays off.",
                          graphicsFamily, familyCount );
                return;
            }
            const uint32_t validBits = families[graphicsFamily].timestampValidBits;
            if ( validBits == 0 )
            {
                LOG_WARN( "[GpuProfiler] Graphics queue reports 0 valid timestamp bits — GPU timing stays "
                          "off." );
                return;
            }
        }

        m_PeriodNs = static_cast<double>( caps.TimestampPeriodNs );

        uint32_t framesInFlight = EngineContext::GetInstance().GetMaxFramesInFlight();
        if ( framesInFlight == 0 )
            framesInFlight = 2;

        // Every frame starts sized for kGpuInitialScopeCapacity scopes plus the whole-frame bracket — the
        // denominator the breakdown is checked against; without it "the parts sum to 14 ms" has nothing to
        // be 14 ms OF. More views than that holds grow the pools on their next turn (GrowIfNeeded).
        const uint32_t initialQueries = GpuGrownQueryCount( 0, kGpuInitialScopeCapacity );

        m_Frames = std::vector<FrameQueries>( framesInFlight );
        for ( FrameQueries& frame : m_Frames )
        {
            frame.Pool = CreatePool( initialQueries );
            if ( frame.Pool == VK_NULL_HANDLE )
            {
                LOG_ERROR( "[GpuProfiler] vkCreateQueryPool failed for {} queries — GPU timing stays off.",
                           initialQueries );
                Shutdown();
                return;
            }
            frame.QueryCount = initialQueries;
        }

        m_Active = true;
        Common::Profiling::Profiler::Get().SetGpuSink( this );

        LOG_INFO( "[GpuProfiler] Timestamps on: {} pools of {} queries ({} scopes each, grown on demand), "
                  "period {} ns/tick, results read {} frames late.",
                  framesInFlight, initialQueries, GpuScopeCapacity( initialQueries ), m_PeriodNs, framesInFlight );
    }

    void VulkanGpuProfiler::Shutdown()
    {
        if ( Common::Profiling::Profiler::Get().GetGpuSink() == this )
            Common::Profiling::Profiler::Get().SetGpuSink( nullptr );

        // Direct, not deferred: shutdown runs with the device idle, and a pool replaced earlier is already
        // in the allocator's queue, which the device teardown drains.
        if ( m_Device != VK_NULL_HANDLE )
            for ( const FrameQueries& frame : m_Frames )
                if ( frame.Pool != VK_NULL_HANDLE )
                    vkDestroyQueryPool( m_Device, frame.Pool, nullptr );

        m_Frames.clear();
        m_Active        = false;
        m_Recording     = nullptr;
        m_CommandBuffer = VK_NULL_HANDLE;
    }

    VkQueryPool VulkanGpuProfiler::CreatePool( uint32_t queryCount ) const
    {
        VkQueryPoolCreateInfo createInfo{};
        createInfo.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        createInfo.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        createInfo.queryCount = queryCount;

        VkQueryPool pool = VK_NULL_HANDLE;
        if ( vkCreateQueryPool( m_Device, &createInfo, nullptr, &pool ) != VK_SUCCESS )
            return VK_NULL_HANDLE;
        return pool;
    }

    void VulkanGpuProfiler::GrowIfNeeded( FrameQueries& frame )
    {
        const uint32_t wanted = GpuGrownQueryCount( frame.QueryCount, m_HighWaterScopes );
        if ( wanted == frame.QueryCount )
            return;

        // The new pool first: if it cannot be made, the old one keeps timing what it can hold rather than
        // the frame losing every scope.
        const auto grown = CreatePool( wanted );
        if ( grown == VK_NULL_HANDLE )
        {
            LOG_ERROR( "[GpuProfiler] vkCreateQueryPool failed growing a frame's pool from {} to {} queries "
                       "for {} scopes; keeping the old pool, scopes past {} stay untimed.",
                       frame.QueryCount, wanted, m_HighWaterScopes, GpuScopeCapacity( frame.QueryCount ) );
            return;
        }

        // Deferred like every other per-frame GPU object: its last use has been resolved, but the rule is
        // "the allocator decides when a frame's objects are free", not a judgement made at each call site.
        const auto ctx       = EngineContext::GetInstance().GetRendererContext();
        auto*      allocator = ctx ? SP_CAST( VulkanContext, ctx )->GetVulkanAllocator().get() : nullptr;
        if ( allocator != nullptr )
            allocator->RT_DestroyQueryPool( frame.Pool );
        else
            vkDestroyQueryPool( m_Device, frame.Pool, nullptr );

        LOG_INFO( "[GpuProfiler] A frame asked for {} GPU scopes; frame pool grown from {} to {} queries.",
                  m_HighWaterScopes, frame.QueryCount, wanted );

        frame.Pool       = grown;
        frame.QueryCount = wanted;
    }

    void VulkanGpuProfiler::BeginFrame( VkCommandBuffer commandBuffer )
    {
        if ( !m_Active || commandBuffer == VK_NULL_HANDLE )
            return;

        const uint32_t frameIndex = EngineContext::GetInstance().GetCurrentFrameIndex();
        if ( frameIndex >= m_Frames.size() )
            return;
        FrameQueries& frame = m_Frames[frameIndex];

        // Everything this frame index submitted last time round has completed — VulkanQueue::Present()
        // waited on its fence before handing the index back. So this read never blocks, and afterwards
        // nothing still refers to the pool, which is what makes this the one place it may be replaced.
        Resolve( frame );
        GrowIfNeeded( frame );

        frame.Recorder.Reset( GpuScopeCapacity( frame.QueryCount ) );
        frame.FrameTotalWritten = false;
        m_Recording             = &frame;
        m_CommandBuffer         = commandBuffer;

        // Switched OFF means off: no reset, no writes, not one command in the buffer. The reset is
        // conditional with the writes rather than unconditional-and-cheap so that turning GPU timing off
        // costs exactly nothing and the A/B that prices this feature measures the whole of it.
        if ( !Common::Profiling::Profiler::Get().GpuEnabled() )
            return;

        // One reset for the frame's whole pool, outside any render pass. A timestamp written to a query
        // that was not reset is undefined, so this must precede every write.
        vkCmdResetQueryPool( commandBuffer, frame.Pool, 0, frame.QueryCount );

        vkCmdWriteTimestamp( commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.Pool,
                             kGpuFrameTotalQuery );
        frame.FrameTotalWritten = true;
    }

    void VulkanGpuProfiler::EndFrame( VkCommandBuffer commandBuffer )
    {
        if ( !m_Active || commandBuffer == VK_NULL_HANDLE || m_Recording == nullptr )
            return;

        const FrameQueries& frame = *m_Recording;
        if ( frame.FrameTotalWritten )
            vkCmdWriteTimestamp( commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.Pool,
                                 kGpuFrameTotalQuery + 1 );

        if ( frame.Recorder.Requested() > m_HighWaterScopes )
            m_HighWaterScopes = frame.Recorder.Requested();

        m_Recording     = nullptr;
        m_CommandBuffer = VK_NULL_HANDLE;
    }

    int32_t VulkanGpuProfiler::BeginScope( const char* name )
    {
        // FrameTotalWritten doubles as "this frame's queries have been reset". Without it, flipping the
        // toggle on mid-frame would write timestamps into queries nobody reset — undefined, and the kind
        // of thing that reads as a wild number rather than an error.
        if ( !m_Active || m_Recording == nullptr || !m_Recording->FrameTotalWritten )
            return -1;

        // The owner is whichever view the ActiveViewScope targets, else the frame context. Its NAME is
        // captured now, not read at resolve: the view may be gone by the time its queries are readable.
        ViewResources&         active       = ViewResourceRegistry::Active();
        const bool             frameContext = &active == &ViewResourceRegistry::FrameContext();
        const std::string_view viewName = frameContext ? std::string_view{} : std::string_view{ active.GetName() };

        // A refused scope (pool full this frame) returns -1, and EndScope's matching -1 leaves the nesting
        // of the scopes that were timed untouched. The refusal raises the high-water mark, so the pool
        // is big enough the next time this frame index comes round.
        const int32_t index = m_Recording->Recorder.Begin( GpuViewScopeName( viewName, name ), &active );
        if ( index < 0 )
            return -1;

        vkCmdWriteTimestamp( m_CommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_Recording->Pool,
                             GpuScopeQueryBase( static_cast<uint32_t>( index ) ) );
        return index;
    }

    void VulkanGpuProfiler::EndScope( int32_t handle )
    {
        if ( handle < 0 || !m_Active || m_Recording == nullptr )
            return;

        // The handle is the scope's index in the recording frame, and the recorder remembers which view's
        // nesting it belongs to — so a scope closes against the view it opened in even if the active view
        // moved meanwhile.
        m_Recording->Recorder.End( handle );
        vkCmdWriteTimestamp( m_CommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_Recording->Pool,
                             GpuScopeQueryBase( static_cast<uint32_t>( handle ) ) + 1 );
    }

    void VulkanGpuProfiler::Resolve( FrameQueries& frame )
    {
        if ( !frame.FrameTotalWritten )
            return;
        frame.FrameTotalWritten = false;

        const std::vector<GpuScopeRecorder::Scope>& scopes = frame.Recorder.Scopes();
        const uint32_t queryCount = GpuQueriesForScopes( static_cast<uint32_t>( scopes.size() ) );
        m_ResultScratch.assign( static_cast<size_t>( queryCount ) * kWordsPerQuery, 0ull );

        // One read for the queries this frame actually wrote. WITHOUT VK_QUERY_RESULT_WAIT_BIT: the fence
        // guarantees the data is there, and asking Vulkan to wait would turn a free read into a GPU stall.
        const VkResult result = vkGetQueryPoolResults(
             m_Device, frame.Pool, 0, queryCount, m_ResultScratch.size() * sizeof( uint64_t ),
             m_ResultScratch.data(), sizeof( uint64_t ) * kWordsPerQuery,
             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );

        if ( result != VK_SUCCESS && result != VK_NOT_READY )
            return;

        // Inclusive milliseconds of the pair starting at @p queryBase, or -1 when either end did not land.
        const auto pairMs = [this]( uint32_t queryBase ) -> double
        {
            const size_t beginWord = static_cast<size_t>( queryBase ) * kWordsPerQuery;
            const size_t endWord   = beginWord + kWordsPerQuery;
            if ( endWord + 1 >= m_ResultScratch.size() )
                return -1.0;
            if ( m_ResultScratch[beginWord + 1] == 0 || m_ResultScratch[endWord + 1] == 0 )
                return -1.0; // a scope whose end never recorded (early-out inside the pass)

            const uint64_t beginTicks = m_ResultScratch[beginWord];
            const uint64_t endTicks   = m_ResultScratch[endWord];
            if ( endTicks < beginTicks )
                return -1.0; // counter wrapped; one lost sample beats a nonsense one

            return static_cast<double>( endTicks - beginTicks ) * m_PeriodNs * 1e-6;
        };

        auto& profiler = Common::Profiling::Profiler::Get();

        // The whole-frame bracket: one number per frame, belonging to no view, never anyone's child.
        const double frameMs = pairMs( kGpuFrameTotalQuery );
        if ( frameMs >= 0.0 )
            profiler.AddGpuSample( Common::Profiling::kGpuFrameTotalScope, frameMs, frameMs );

        const size_t scopeCount = scopes.size();
        m_Inclusive.assign( scopeCount, -1.0 );
        m_Parents.resize( scopeCount );
        for ( size_t i = 0; i < scopeCount; ++i )
        {
            m_Parents[i]   = scopes[i].Parent;
            m_Inclusive[i] = pairMs( GpuScopeQueryBase( static_cast<uint32_t>( i ) ) );
        }

        // Self time = own interval minus the DIRECT children's, so the parts partition the frame instead
        // of counting a parent's microseconds again in each child. Parents never cross views
        // (GpuScopeRecorder), so one pass over the interleaved list is each view's partition at once. The
        // arithmetic lives in Engine/Graphic/GpuTimestampLayout.hpp and is asserted by
        // Tests/Engine/GpuTimestampLayout.
        m_Self = GpuSelfTimes( m_Inclusive, m_Parents );

        for ( size_t i = 0; i < scopeCount; ++i )
            if ( m_Inclusive[i] >= 0.0 )
                profiler.AddGpuSample( scopes[i].Name.c_str(), m_Inclusive[i], m_Self[i] );
    }
} // namespace Desert::Graphic::API::Vulkan

#endif // DESERT_DEV_INSTRUMENTS
