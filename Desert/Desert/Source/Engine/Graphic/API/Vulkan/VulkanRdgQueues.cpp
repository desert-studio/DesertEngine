#include <Engine/Graphic/API/Vulkan/VulkanRdgQueues.hpp>

#include <algorithm>
#include <cstdio>
#include <format>
#include <utility>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        size_t PipeIndex( RDG::Pipe pipe )
        {
            return pipe == RDG::Pipe::Graphics ? 0u : 1u;
        }

        const char* PipeName( RDG::Pipe pipe )
        {
            return pipe == RDG::Pipe::Graphics ? "Graphics" : "AsyncCompute";
        }
    } // namespace

    // ── VulkanRdgQueueObjects ───────────────────────────────────────────────────────────────────────────────

    VulkanRdgQueueObjects::VulkanRdgQueueObjects( VkDevice device, uint32_t graphicsFamily,
                                                  std::optional<uint32_t> computeFamily, uint32_t frameSlots )
         : m_Device( device ), m_Slots( std::max( 1u, frameSlots ) )
    {
        for ( Slot& slot : m_Slots )
        {
            for ( size_t pipe = 0; pipe < slot.Pipes.size(); ++pipe )
            {
                if ( pipe == 1 && !computeFamily )
                    continue;
                VkCommandPoolCreateInfo info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                info.queueFamilyIndex = pipe == 0 ? graphicsFamily : *computeFamily;
                info.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
                if ( vkCreateCommandPool( m_Device, &info, nullptr, &slot.Pipes[pipe].Pool ) != VK_SUCCESS )
                    slot.Pipes[pipe].Pool = VK_NULL_HANDLE; // refused by name at BeginCommandBuffer
            }
        }
    }

    VulkanRdgQueueObjects::~VulkanRdgQueueObjects()
    {
        for ( Slot& slot : m_Slots )
        {
            for ( PipePool& pipe : slot.Pipes )
            {
                if ( pipe.Pool != VK_NULL_HANDLE )
                    vkDestroyCommandPool( m_Device, pipe.Pool, nullptr ); // frees its command buffers
            }
            for ( VkSemaphore semaphore : slot.Semaphores )
                vkDestroySemaphore( m_Device, semaphore, nullptr );
        }
    }

    Common::BoolResultStr VulkanRdgQueueObjects::BeginFrameSlot( uint32_t slot )
    {
        m_Slot          = slot % static_cast<uint32_t>( m_Slots.size() );
        Slot& frameSlot = m_Slots[m_Slot];
        for ( PipePool& pipe : frameSlot.Pipes )
        {
            if ( pipe.Pool == VK_NULL_HANDLE )
                continue;
            const VkResult reset = vkResetCommandPool( m_Device, pipe.Pool, 0 );
            if ( reset != VK_SUCCESS )
                return Common::MakeError( std::format( "RDG queue objects: vkResetCommandPool failed ({})",
                                                       static_cast<int>( reset ) ) );
            pipe.Used = 0;
        }
        frameSlot.SemaphoresUsed = 0;
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<VkCommandBuffer> VulkanRdgQueueObjects::BeginCommandBuffer( RDG::Pipe pipe )
    {
        using Out      = VkCommandBuffer;
        PipePool& pool = m_Slots[m_Slot].Pipes[PipeIndex( pipe )];
        if ( pool.Pool == VK_NULL_HANDLE )
            return Common::MakeError<Out>(
                 std::format( "RDG queue objects: no command pool for the {} pipe", PipeName( pipe ) ) );
        if ( pool.Used == pool.Buffers.size() )
        {
            VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocate.commandPool        = pool.Pool;
            allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            VkCommandBuffer buffer      = VK_NULL_HANDLE;
            if ( vkAllocateCommandBuffers( m_Device, &allocate, &buffer ) != VK_SUCCESS )
                return Common::MakeError<Out>( "RDG queue objects: vkAllocateCommandBuffers failed" );
            pool.Buffers.push_back( buffer );
        }
        const VkCommandBuffer    buffer = pool.Buffers[pool.Used++];
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ( vkBeginCommandBuffer( buffer, &begin ) != VK_SUCCESS )
            return Common::MakeError<Out>( "RDG queue objects: vkBeginCommandBuffer failed" );
        return Common::MakeSuccess( buffer );
    }

    Common::ResultStr<VkSemaphore> VulkanRdgQueueObjects::AcquireSemaphore()
    {
        Slot& slot = m_Slots[m_Slot];
        if ( slot.SemaphoresUsed == slot.Semaphores.size() )
        {
            const VkSemaphoreCreateInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            VkSemaphore                 semaphore = VK_NULL_HANDLE;
            if ( vkCreateSemaphore( m_Device, &info, nullptr, &semaphore ) != VK_SUCCESS )
                return Common::MakeError<VkSemaphore>( "RDG queue objects: vkCreateSemaphore failed" );
            slot.Semaphores.push_back( semaphore );
        }
        return Common::MakeSuccess( slot.Semaphores[slot.SemaphoresUsed++] );
    }

    // ── VulkanRdgQueueSet ───────────────────────────────────────────────────────────────────────────────────

    RDG::PipeCapabilities VulkanRdgQueueSet::GetCapabilities() const
    {
        RDG::PipeCapabilities capabilities;
        capabilities.SeparateComputeFamily = ComputeQueue != VK_NULL_HANDLE && ComputeFamily != GraphicsFamily;
        return capabilities;
    }

    VkQueue VulkanRdgQueueSet::QueueOf( RDG::Pipe pipe ) const
    {
        return pipe == RDG::Pipe::Graphics ? GraphicsQueue : ComputeQueue;
    }

    uint32_t VulkanRdgQueueSet::FamilyOf( RDG::Pipe pipe ) const
    {
        return pipe == RDG::Pipe::Graphics ? GraphicsFamily : ComputeFamily;
    }

    // ── VulkanRdgSegmentRecorder ────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<VkCommandBuffer> VulkanRdgSegmentRecorder::BeginGraph( const RDG::CompileResult& result,
                                                                             const VulkanRdgQueueSet&  queues )
    {
        using Out = VkCommandBuffer;
        // A previous graph's open tail (its caller runs several graphs before TakeSubmissions) closes in order.
        if ( m_Open )
        {
            if ( Common::BoolResultStr closed = Close(); !closed )
                return Common::MakeError<Out>( closed.GetError() );
        }
        m_Result = &result;
        m_SyncSemaphores.assign( result.Syncs.size(), VK_NULL_HANDLE );
        m_NextSegment      = 0;
        m_GraphSubmissions = m_Submissions.size();
        if ( !result.Segments.empty() )
            return Common::MakeSuccess<Out>( VK_NULL_HANDLE );
        if ( Common::BoolResultStr opened = Open( RDG::Pipe::Graphics, queues ); !opened )
            return Common::MakeError<Out>( opened.GetError() );
        return Common::MakeSuccess( m_Open->CommandBuffer );
    }

    Common::ResultStr<VkSemaphore> VulkanRdgSegmentRecorder::SemaphoreOf( uint32_t                 sync,
                                                                          const VulkanRdgQueueSet& queues )
    {
        if ( sync >= m_SyncSemaphores.size() )
            return Common::MakeError<VkSemaphore>( std::format( "sync {} is not in the compiled graph", sync ) );
        if ( m_SyncSemaphores[sync] == VK_NULL_HANDLE )
        {
            Common::ResultStr<VkSemaphore> acquired = queues.Objects->AcquireSemaphore();
            if ( !acquired )
                return acquired;
            m_SyncSemaphores[sync] = acquired.GetValue();
        }
        return Common::MakeSuccess( m_SyncSemaphores[sync] );
    }

    Common::BoolResultStr VulkanRdgSegmentRecorder::Open( RDG::Pipe pipe, const VulkanRdgQueueSet& queues )
    {
        if ( queues.Objects == nullptr )
            return Common::MakeError( "the queue set has no per-slot queue objects" );
        const VkQueue queue = queues.QueueOf( pipe );
        if ( queue == VK_NULL_HANDLE )
            return Common::MakeError( std::format( "the queue set has no {} queue", PipeName( pipe ) ) );
        Common::ResultStr<VkCommandBuffer> buffer = queues.Objects->BeginCommandBuffer( pipe );
        if ( !buffer )
            return Common::MakeError( buffer.GetError() );
        m_Open.emplace();
        m_Open->OnPipe        = pipe;
        m_Open->Queue         = queue;
        m_Open->CommandBuffer = buffer.GetValue();
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanRdgSegmentRecorder::Close()
    {
        const VkResult ended = vkEndCommandBuffer( m_Open->CommandBuffer );
        if ( ended != VK_SUCCESS )
        {
            m_Open.reset();
            return Common::MakeError(
                 std::format( "vkEndCommandBuffer of a segment failed ({})", static_cast<int>( ended ) ) );
        }
        m_Submissions.push_back( std::move( *m_Open ) );
        m_Open.reset();
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<VkCommandBuffer> VulkanRdgSegmentRecorder::BeginSegment( const RDG::PipeSegment&  segment,
                                                                               const VulkanRdgQueueSet& queues )
    {
        using Out = VkCommandBuffer;
        if ( m_Result == nullptr || m_NextSegment >= m_Result->Segments.size() ||
             m_Result->Segments[m_NextSegment].FirstPosition != segment.FirstPosition ||
             m_Result->Segments[m_NextSegment].OnPipe != segment.OnPipe )
            return Common::MakeError<Out>( "segment begun out of the compiled order" );
        if ( segment.OnPipe == RDG::Pipe::AsyncCompute && !queues.GetCapabilities().SeparateComputeFamily )
            return Common::MakeError<Out>( "AsyncCompute segment on a device without a separate compute family" );
        if ( m_Open )
            return Common::MakeError<Out>( "segment begun while another segment is open" );
        ++m_NextSegment;
        if ( Common::BoolResultStr opened = Open( segment.OnPipe, queues ); !opened )
            return Common::MakeError<Out>( opened.GetError() );
        for ( const uint32_t sync : segment.WaitSyncs )
        {
            Common::ResultStr<VkSemaphore> semaphore = SemaphoreOf( sync, queues );
            if ( !semaphore )
                return Common::MakeError<Out>( semaphore.GetError() );
            // v1 needs a non-empty wait mask; a wait naming no stage blocks everything after it.
            const VkPipelineStageFlags stages = RdgVulkanStages( m_Result->Syncs[sync].WaitStages );
            m_Open->WaitSemaphores.push_back( semaphore.GetValue() );
            m_Open->WaitStages.push_back( stages != 0 ? stages : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT );
        }
        for ( const uint32_t sync : segment.SignalSyncs )
        {
            Common::ResultStr<VkSemaphore> semaphore = SemaphoreOf( sync, queues );
            if ( !semaphore )
                return Common::MakeError<Out>( semaphore.GetError() );
            m_Open->SignalSemaphores.push_back( semaphore.GetValue() );
        }
        return Common::MakeSuccess( m_Open->CommandBuffer );
    }

    Common::ResultStr<VkCommandBuffer> VulkanRdgSegmentRecorder::EndSegment( const RDG::PipeSegment&  segment,
                                                                             const VulkanRdgQueueSet& queues )
    {
        using Out = VkCommandBuffer;
        if ( !m_Open || m_Open->OnPipe != segment.OnPipe )
            return Common::MakeError<Out>( "segment ended without being begun" );

        std::vector<uint32_t> endJoins; // joins no pass waits on: the tail waits on them
        for ( uint32_t sync = 0; sync < m_Result->Syncs.size(); ++sync )
        {
            if ( m_Result->Syncs[sync].WaitPosition == RDG::CrossPipeSync::kJoinAtGraphEnd )
                endJoins.push_back( sync );
        }
        const bool last = m_NextSegment == m_Result->Segments.size();
        if ( last && segment.OnPipe == RDG::Pipe::Graphics && endJoins.empty() )
            return Common::MakeSuccess( m_Open->CommandBuffer ); // the final barriers record into it

        if ( Common::BoolResultStr closed = Close(); !closed )
            return Common::MakeError<Out>( closed.GetError() );
        if ( !last )
            return Common::MakeSuccess<Out>( VK_NULL_HANDLE );

        // The tail: waits on every graph-end join BEFORE the final barriers (with the acquires of the
        // transfers back to Graphics) are recorded. It records only barriers, so it waits on all stages.
        if ( Common::BoolResultStr opened = Open( RDG::Pipe::Graphics, queues ); !opened )
            return Common::MakeError<Out>( opened.GetError() );
        for ( const uint32_t sync : endJoins )
        {
            Common::ResultStr<VkSemaphore> semaphore = SemaphoreOf( sync, queues );
            if ( !semaphore )
                return Common::MakeError<Out>( semaphore.GetError() );
            m_Open->WaitSemaphores.push_back( semaphore.GetValue() );
            m_Open->WaitStages.push_back( VK_PIPELINE_STAGE_ALL_COMMANDS_BIT );
        }
        return Common::MakeSuccess( m_Open->CommandBuffer );
    }

    void VulkanRdgSegmentRecorder::AbandonGraph()
    {
        // A command buffer left recording is reset with its pool at the slot's next BeginFrameSlot.
        m_Open.reset();
        m_Submissions.resize( std::min( m_Submissions.size(), m_GraphSubmissions ) );
        m_Result = nullptr;
    }

    std::vector<VulkanRdgSubmission> VulkanRdgSegmentRecorder::Take()
    {
        if ( m_Open )
        {
            if ( Common::BoolResultStr closed = Close(); !closed )
            {
                // A tail that did not end cannot be submitted, and neither can what waits on it: nothing is.
                std::fprintf( stderr, "RDG: %s; the graph's submissions are dropped\n",
                              closed.GetError().c_str() );
                AbandonGraph();
            }
        }
        m_Result           = nullptr;
        m_GraphSubmissions = 0;
        return std::exchange( m_Submissions, {} );
    }
} // namespace Desert::Graphic::API::Vulkan
