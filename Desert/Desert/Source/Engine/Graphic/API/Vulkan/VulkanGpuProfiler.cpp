#include <Engine/Graphic/API/Vulkan/VulkanGpuProfiler.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/GpuTimestampLayout.hpp>

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

        m_PeriodNs       = static_cast<double>( caps.TimestampPeriodNs );
        m_FramesInFlight = EngineContext::GetInstance().GetMaxFramesInFlight();
        if ( m_FramesInFlight == 0 )
            m_FramesInFlight = 2;

        // Per frame: every slot's scopes, plus one pair bracketing the whole command buffer. That last pair
        // is the denominator the breakdown is checked against — without it "the parts sum to 14 ms" has
        // nothing to be 14 ms OF.
        m_QueriesPerFrame = GpuQueriesPerFrame( EngineContext::kMaxRendererSlots, kMaxScopesPerFrameSlot );
        m_TotalQueries    = m_FramesInFlight * m_QueriesPerFrame;

        VkQueryPoolCreateInfo createInfo{};
        createInfo.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        createInfo.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        createInfo.queryCount = m_TotalQueries;

        if ( vkCreateQueryPool( m_Device, &createInfo, nullptr, &m_QueryPool ) != VK_SUCCESS )
        {
            LOG_ERROR( "[GpuProfiler] vkCreateQueryPool failed — GPU timing stays off." );
            m_QueryPool = VK_NULL_HANDLE;
            return;
        }

        m_State.assign( static_cast<size_t>( m_FramesInFlight ) * EngineContext::kMaxRendererSlots,
                        FrameSlotState{} );
        m_ResultScratch.assign( static_cast<size_t>( m_QueriesPerFrame ) * kWordsPerQuery, 0ull );

        m_Active = true;
        Common::Profiling::Profiler::Get().SetGpuSink( this );

        LOG_INFO( "[GpuProfiler] Timestamps on: {} queries ({} frames x {} slots x {} scopes), period {} "
                  "ns/tick, results read {} frames late.",
                  m_TotalQueries, m_FramesInFlight, EngineContext::kMaxRendererSlots, kMaxScopesPerFrameSlot,
                  m_PeriodNs, m_FramesInFlight );
    }

    void VulkanGpuProfiler::Shutdown()
    {
        if ( Common::Profiling::Profiler::Get().GetGpuSink() == this )
            Common::Profiling::Profiler::Get().SetGpuSink( nullptr );

        if ( m_QueryPool != VK_NULL_HANDLE && m_Device != VK_NULL_HANDLE )
        {
            vkDestroyQueryPool( m_Device, m_QueryPool, nullptr );
            m_QueryPool = VK_NULL_HANDLE;
        }
        m_Active        = false;
        m_CommandBuffer = VK_NULL_HANDLE;
        m_State.clear();
    }

    uint32_t VulkanGpuProfiler::SlotQueryBase( uint32_t frameIndex, uint32_t slot ) const
    {
        return GpuSlotQueryBase( frameIndex, slot, EngineContext::kMaxRendererSlots, kMaxScopesPerFrameSlot );
    }

    uint32_t VulkanGpuProfiler::FrameTotalQueryBase( uint32_t frameIndex ) const
    {
        return GpuFrameTotalQueryBase( frameIndex, EngineContext::kMaxRendererSlots, kMaxScopesPerFrameSlot );
    }

    void VulkanGpuProfiler::BeginFrame( VkCommandBuffer commandBuffer )
    {
        if ( !m_Active || commandBuffer == VK_NULL_HANDLE )
            return;

        const uint32_t frameIndex = EngineContext::GetInstance().GetCurrentFrameIndex();

        // Everything this frame index submitted last time round has completed — VulkanQueue::Present()
        // waited on its fence before handing the index back. So this read never blocks.
        Resolve( frameIndex );

        for ( uint32_t slot = 0; slot < EngineContext::kMaxRendererSlots; ++slot )
        {
            FrameSlotState& state = m_State[frameIndex * EngineContext::kMaxRendererSlots + slot];
            state.Scopes.clear();
            state.OpenStack.clear();
            state.Pending = false;
        }

        m_CommandBuffer     = commandBuffer;
        m_FrameTotalWritten = false;

        // Switched OFF means off: no reset, no writes, not one command in the buffer. The reset is
        // conditional with the writes rather than unconditional-and-cheap so that turning GPU timing off
        // costs exactly nothing and the A/B that prices this feature measures the whole of it.
        if ( !Common::Profiling::Profiler::Get().GpuEnabled() )
            return;

        // One reset for the frame's whole range, all slots at once, outside any render pass. A timestamp
        // written to a query that was not reset is undefined, so this must precede every write.
        vkCmdResetQueryPool( commandBuffer, m_QueryPool, frameIndex * m_QueriesPerFrame, m_QueriesPerFrame );

        vkCmdWriteTimestamp( commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_QueryPool,
                             FrameTotalQueryBase( frameIndex ) );
        m_FrameTotalWritten = true;
    }

    void VulkanGpuProfiler::EndFrame( VkCommandBuffer commandBuffer )
    {
        if ( !m_Active || commandBuffer == VK_NULL_HANDLE )
            return;

        const uint32_t frameIndex = EngineContext::GetInstance().GetCurrentFrameIndex();

        if ( m_FrameTotalWritten )
        {
            vkCmdWriteTimestamp( commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_QueryPool,
                                 FrameTotalQueryBase( frameIndex ) + 1 );

            // Slot 0 carries the whole-frame bracket: it is one number per frame, not per renderer, and
            // parking it in the slot that always exists keeps Resolve's loop uniform.
            FrameSlotState& state = m_State[static_cast<size_t>( frameIndex ) * EngineContext::kMaxRendererSlots];
            state.Scopes.push_back( ScopeRecord{ Common::Profiling::kGpuFrameTotalScope,
                                                 FrameTotalQueryBase( frameIndex ), kNoParent } );
            state.Pending = true;
        }

        m_CommandBuffer = VK_NULL_HANDLE;
    }

    int32_t VulkanGpuProfiler::BeginScope( const char* name )
    {
        // m_FrameTotalWritten doubles as "this frame's queries have been reset". Without it, flipping the
        // toggle on mid-frame would write timestamps into queries nobody reset — undefined, and the kind
        // of thing that reads as a wild number rather than an error.
        if ( !m_Active || m_CommandBuffer == VK_NULL_HANDLE || !m_FrameTotalWritten )
            return -1;

        const uint32_t frameIndex = EngineContext::GetInstance().GetCurrentFrameIndex();
        const uint32_t slot       = EngineContext::GetInstance().GetActiveRendererSlot();

        FrameSlotState& state = m_State[frameIndex * EngineContext::kMaxRendererSlots + slot];

        // A scope that cannot be timed must still not corrupt the nesting of the ones that can, so the
        // overflow path below returns -1 and EndScope's matching -1 leaves the stack untouched.
        if ( state.Scopes.size() >= kMaxScopesPerFrameSlot )
        {
            if ( !m_WarnedOverflow )
            {
                m_WarnedOverflow = true;
                LOG_WARN( "[GpuProfiler] More than {} GPU scopes in one (frame x slot); the rest of this "
                          "frame is untimed. Raise kMaxScopesPerFrameSlot or mark fewer passes.",
                          kMaxScopesPerFrameSlot );
            }
            return -1;
        }

        const int32_t  index     = static_cast<int32_t>( state.Scopes.size() );
        const uint32_t queryBase = SlotQueryBase( frameIndex, slot ) + static_cast<uint32_t>( index ) * 2;
        const int32_t  parent    = state.OpenStack.empty() ? kNoParent : state.OpenStack.back();

        state.Scopes.push_back( ScopeRecord{ name, queryBase, parent } );
        state.OpenStack.push_back( index );
        state.Pending = true;

        vkCmdWriteTimestamp( m_CommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_QueryPool, queryBase );

        return static_cast<int32_t>( queryBase );
    }

    void VulkanGpuProfiler::EndScope( int32_t handle )
    {
        if ( handle < 0 || !m_Active || m_CommandBuffer == VK_NULL_HANDLE )
            return;

        // The handle is the absolute query index, which carries the frame and the slot with it — so a
        // scope closes against the state it opened against even if the ambient slot moved meanwhile.
        const uint32_t queryBase = static_cast<uint32_t>( handle );
        const uint32_t frameIndex =
             GpuDecodeFrame( queryBase, EngineContext::kMaxRendererSlots, kMaxScopesPerFrameSlot );
        const uint32_t slot = GpuDecodeSlot( queryBase, EngineContext::kMaxRendererSlots, kMaxScopesPerFrameSlot );

        if ( frameIndex < m_FramesInFlight && slot < EngineContext::kMaxRendererSlots )
        {
            FrameSlotState& state = m_State[frameIndex * EngineContext::kMaxRendererSlots + slot];
            if ( !state.OpenStack.empty() )
                state.OpenStack.pop_back();
        }

        vkCmdWriteTimestamp( m_CommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_QueryPool, queryBase + 1 );
    }

    void VulkanGpuProfiler::Resolve( uint32_t frameIndex )
    {
        bool anyPending = false;
        for ( uint32_t slot = 0; slot < EngineContext::kMaxRendererSlots; ++slot )
            anyPending |= m_State[frameIndex * EngineContext::kMaxRendererSlots + slot].Pending;

        if ( !anyPending )
            return;

        // One read for the frame's whole range. WITHOUT VK_QUERY_RESULT_WAIT_BIT: the fence guarantees the
        // data is there, and asking Vulkan to wait would turn a free read into a GPU stall.
        const VkResult result = vkGetQueryPoolResults(
             m_Device, m_QueryPool, frameIndex * m_QueriesPerFrame, m_QueriesPerFrame,
             m_ResultScratch.size() * sizeof( uint64_t ), m_ResultScratch.data(),
             sizeof( uint64_t ) * kWordsPerQuery, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );

        if ( result != VK_SUCCESS && result != VK_NOT_READY )
            return;

        const uint32_t frameBase = frameIndex * m_QueriesPerFrame;

        for ( uint32_t slot = 0; slot < EngineContext::kMaxRendererSlots; ++slot )
        {
            FrameSlotState& state = m_State[frameIndex * EngineContext::kMaxRendererSlots + slot];
            if ( !state.Pending )
                continue;

            const size_t scopeCount = state.Scopes.size();
            m_Inclusive.assign( scopeCount, -1.0 );
            m_Parents.resize( scopeCount );
            for ( size_t i = 0; i < scopeCount; ++i )
                m_Parents[i] = state.Scopes[i].ParentIndex;

            for ( size_t i = 0; i < scopeCount; ++i )
            {
                const ScopeRecord& scope     = state.Scopes[i];
                const uint32_t     beginWord = ( scope.QueryBase - frameBase ) * kWordsPerQuery;
                const uint32_t     endWord   = beginWord + kWordsPerQuery;
                if ( endWord + 1 >= m_ResultScratch.size() )
                    continue;

                const uint64_t beginAvailable = m_ResultScratch[beginWord + 1];
                const uint64_t endAvailable   = m_ResultScratch[endWord + 1];
                if ( beginAvailable == 0 || endAvailable == 0 )
                    continue; // a scope whose end never recorded (early-out inside the pass)

                const uint64_t beginTicks = m_ResultScratch[beginWord];
                const uint64_t endTicks   = m_ResultScratch[endWord];
                if ( endTicks < beginTicks )
                    continue; // counter wrapped; one lost sample beats a nonsense one

                m_Inclusive[i] = static_cast<double>( endTicks - beginTicks ) * m_PeriodNs * 1e-6;
            }

            // Self time = own interval minus the DIRECT children's, so the parts partition the frame
            // instead of counting a parent's microseconds again in each child. The arithmetic lives in
            // Engine/Graphic/GpuTimestampLayout.hpp and is asserted by Tests/Engine/GpuTimestampLayout.
            m_Self = GpuSelfTimes( m_Inclusive, m_Parents );

            for ( size_t i = 0; i < scopeCount; ++i )
            {
                if ( m_Inclusive[i] < 0.0 )
                    continue;
                Common::Profiling::Profiler::Get().AddGpuSample( state.Scopes[i].Name.c_str(), m_Inclusive[i],
                                                                 m_Self[i] );
            }

            state.Scopes.clear();
            state.OpenStack.clear();
            state.Pending = false;
        }
    }
} // namespace Desert::Graphic::API::Vulkan
