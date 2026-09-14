#include <Engine/ShaderResources/API/Vulkan/VulkanStorageBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/ShaderResources/BufferCopyLayout.hpp>

#include <cstring>

namespace Desert::ShaderResources::API::Vulkan
{
    VulkanStorageBuffer::VulkanStorageBuffer( const std::string_view bufferName, uint32_t size, uint32_t binding,
                                              bool persistent )
         : m_Lifetime( persistent ? Persistence::AcrossFrames : Persistence::PerFrame ), m_Size( size ),
           m_Binding( binding ), m_BufferName( bufferName )
    {
        if ( m_Size == 0 )
            m_Size = 1; // Vulkan requires size > 0; grows on first SetData.

        // A constructor has no channel, so the answer is kept and handed to every later question. See
        // VulkanUniformBuffer's constructor for the same reasoning.
        m_Built = RT_Invalidate();
        if ( !m_Built.IsSuccess() )
            LOG_ERROR( "[StorageBuffer] '{}' was not built: {}", m_BufferName, m_Built.GetError() );
    }

    VulkanStorageBuffer::~VulkanStorageBuffer()
    {
        Release();
    }

    void VulkanStorageBuffer::Release()
    {
        if ( m_Buffers.empty() )
            return;

        auto allocator = SP_CAST( Desert::Graphic::API::Vulkan::VulkanContext,
                                  EngineContext::GetInstance().GetRendererContext() )
                              ->GetVulkanAllocator()
                              .get();

        for ( uint32_t i = 0; i < static_cast<uint32_t>( m_Buffers.size() ); ++i )
        {
            // Unmap BEFORE the deferred destroy is queued, as before; the mapping's destructor does it.
            if ( i < m_Mappings.size() )
                m_Mappings[i].Unmap();
            if ( m_MemoryAllocs[i] )
            {
                // Deferred destroy: in-flight frames may still reference the old buffer.
                allocator->RT_DestroyBuffer( m_Buffers[i], m_MemoryAllocs[i] );
                m_Buffers[i]      = VK_NULL_HANDLE;
                m_MemoryAllocs[i] = nullptr;
            }
        }

        m_Buffers.clear();
        m_MemoryAllocs.clear();
        m_DescriptorInfos.clear();
        m_Mappings.clear();
    }

    Common::BoolResultStr VulkanStorageBuffer::RT_Invalidate()
    {
        Release();

        const uint32_t framesInFlight = EngineContext::GetInstance().GetMaxFramesInFlight();
        const uint32_t slots          = Engine::kMaxRendererSlots;
        // Persistent = ONE buffer shared by every frame AND every view (GPU simulation state must survive
        // across frames, and a second view must not get a fresh copy of a simulation that has been running).
        // Otherwise one buffer per (frame in flight x renderer slot): the frame dimension keeps the GPU from
        // reading a buffer being rewritten, and the SLOT dimension keeps a second view from overwriting the
        // per-object material array or the pose the first view's draws reference.
        const uint32_t copies = IsPersistent() ? 1u : BufferCopyCount( framesInFlight, slots );

        m_Buffers.resize( copies, VK_NULL_HANDLE );
        m_MemoryAllocs.resize( copies, nullptr );
        m_Mappings.resize( copies );
        // Always the FULL matrix, even when persistent collapses the buffers to one: descriptors are
        // indexed by (frame x slot) regardless, so this array is sized by the layout, not by the buffers.
        const uint32_t descriptorCount = BufferCopyCount( framesInFlight, slots );
        m_DescriptorInfos.resize( descriptorCount );

        auto vulkanContext = SP_CAST( Desert::Graphic::API::Vulkan::VulkanContext,
                                      EngineContext::GetInstance().GetRendererContext() );

        for ( uint32_t i = 0; i < copies; ++i )
        {
            VkBufferCreateInfo bufferInfo = {};
            bufferInfo.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            // STORAGE ONLY. Every storage buffer also carried INDIRECT until Г25, granted for ONE
            // caller: the grass cull compute wrote a VkDrawIndirectCommand into a storage buffer and the
            // grass draw read it back through vkCmdDrawIndirect. That generator is gone and no draw in
            // this engine is indirect any more, so the capability is granted to nobody - and a usage bit
            // nothing uses is a claim about this buffer that is not true.
            bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bufferInfo.size  = m_Size;

            const auto allocatedBuffer = vulkanContext->GetVulkanAllocator()->RT_AllocateBuffer(
                 std::format( "{}-StorageBuffer-{}{}", m_BufferName, IsPersistent() ? "Persistent" : "Frame", i ),
                 bufferInfo, VMA_MEMORY_USAGE_CPU_TO_GPU, m_Buffers[i] );

            // REFUSED, NOT SKIPPED — see VulkanUniformBuffer::RT_Invalidate for the whole argument. It
            // is sharper here: the descriptor loop below runs unconditionally, so a skipped copy was
            // published as `.buffer = VK_NULL_HANDLE` with `.range = m_Size`, which is a descriptor
            // asserting that a non-existent buffer is m_Size bytes long.
            if ( !allocatedBuffer.IsSuccess() )
            {
                Release();
                return Common::MakeFormattedError<bool>( "storage buffer '{}' copy {} of {}: {}", m_BufferName, i,
                                                         copies, allocatedBuffer.GetError() );
            }

            m_MemoryAllocs[i] = allocatedBuffer.GetValue();
            m_Mappings[i]     = vulkanContext->GetVulkanAllocator()->MapMemory( m_MemoryAllocs[i] );
            if ( !m_Mappings[i].IsMapped() )
            {
                const std::string reason = m_Mappings[i].GetRefusal();
                Release();
                return Common::MakeFormattedError<bool>( "storage buffer '{}' copy {} of {} could not be "
                                                         "mapped: {}",
                                                         m_BufferName, i, copies, reason );
            }
        }

        // Point every (frame x slot) descriptor at its buffer (persistent: all at buffer 0).
        for ( uint32_t i = 0; i < descriptorCount; ++i )
        {
            const uint32_t idx          = IsPersistent() ? 0u : i;
            m_DescriptorInfos[i].buffer = ( idx < m_Buffers.size() ) ? m_Buffers[idx] : VK_NULL_HANDLE;
            m_DescriptorInfos[i].offset = 0;
            m_DescriptorInfos[i].range  = m_Size;
        }

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanStorageBuffer::Grow( uint32_t newSize )
    {
        const uint32_t previous = m_Size;
        m_Size                  = newSize;

        const auto rebuilt = RT_Invalidate();
        if ( rebuilt.IsSuccess() )
        {
            m_Built = BOOLSUCCESS;
            return m_Built;
        }

        // THE OLD SIZE IS PUT BACK AND THE OLD BUFFER REBUILT, rather than leaving the object empty.
        // RT_Invalidate refuses as a whole and releases what it had, so a failed growth would otherwise
        // destroy a buffer that was working perfectly at its previous size — a write asking for more
        // room than the device can spare would take the buffer's EXISTING contents down with it, which
        // is a far worse outcome than refusing the one oversized write.
        m_Size                 = previous;
        const auto restoration = RT_Invalidate();
        if ( restoration.IsSuccess() )
        {
            // Braced, and not because of style: `BOOLSUCCESS` is a macro that ENDS IN A SEMICOLON, so an
            // unbraced `m_Built = BOOLSUCCESS;` here expands to two statements and detaches the `else`.
            m_Built = BOOLSUCCESS; // the WRITE is what was refused, not the buffer
        }
        else
        {
            m_Built = Common::MakeFormattedError<bool>(
                 "storage buffer '{}' could not grow to {} bytes ({}) and could not be rebuilt at {} "
                 "either: {}",
                 m_BufferName, newSize, rebuilt.GetError(), previous, restoration.GetError() );
        }

        return Common::MakeFormattedError<bool>( "storage buffer '{}' could not grow from {} to {} bytes: {}",
                                                 m_BufferName, previous, newSize, rebuilt.GetError() );
    }

    Common::BoolResultStr VulkanStorageBuffer::SetData( const void* data, uint32_t size, uint32_t offset )
    {
        if ( !m_Built.IsSuccess() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' is not usable: {}", m_BufferName,
                                                     m_Built.GetError() );

        // ASKED HERE AND NOT ONLY BY MappedMemory, because the CPU SHADOW COPY is written first. The
        // mapped write below refuses a null source (Г7-C), but the `std::memcpy` into m_LocalStorage
        // further down does not, and it runs before anything else has a chance to object.
        if ( data == nullptr && size != 0 )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' was given {} byte(s) from a null "
                                                     "source",
                                                     m_BufferName, size );

        // GROWING IS NOT WRITING, AND THIS IS WHERE THE TWO WERE ONE LINE. The old body did
        // `if ( size + offset > m_Size ) { m_Size = size + offset; RT_Invalidate(); }` for every buffer
        // alike — and RT_Invalidate destroys every VkBuffer this object owns. For a PERSISTENT buffer
        // that is the GPU simulation state gone, from the per-frame write path, with nothing returned
        // and nothing logged. The decision now has a name and lives in one pure function
        // (ShaderResources::ClassifyBufferWrite) that a test can reach without a device.
        switch ( ClassifyBufferWrite( m_Size, size, offset, m_Lifetime ) )
        {
            case BufferWriteVerdict::Fits:
                break;

            case BufferWriteVerdict::Grow:
            {
                const auto grown = Grow( RequiredBufferSize( size, offset ) );
                if ( !grown.IsSuccess() )
                    return grown;
                break;
            }

            case BufferWriteVerdict::RefuseWouldDestroyPersistentState:
                return Common::MakeFormattedError<bool>(
                     "storage buffer '{}' holds {} bytes and a write of {} at offset {} {}. The buffer was "
                     "created persistent, so its contents are authored by the GPU and there is nothing on "
                     "the CPU to restore them from -- create it at the size it needs instead of letting a "
                     "write resize it",
                     m_BufferName, m_Size, size, offset,
                     BufferWriteVerdictName( BufferWriteVerdict::RefuseWouldDestroyPersistentState ) );

            case BufferWriteVerdict::RefuseWouldNotFitAnyBuffer:
                return Common::MakeFormattedError<bool>(
                     "storage buffer '{}': a write of {} bytes at offset {} {}", m_BufferName, size, offset,
                     BufferWriteVerdictName( BufferWriteVerdict::RefuseWouldNotFitAnyBuffer ) );
        }

        if ( !m_LocalStorage.Data || m_LocalStorage.GetAllocatedSize() < RequiredBufferSize( size, offset ) )
            m_LocalStorage.Allocate( m_Size );
        std::memcpy( static_cast<uint8_t*>( m_LocalStorage.Data ) + offset, data, size );

        // Persistent buffers have a single mapping (index 0); the rest write the copy belonging to this
        // frame AND this view.
        const uint32_t idx = IsPersistent() ? 0u : CopyIndex();
        if ( idx >= m_Mappings.size() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' has no copy {} to write ({} exist)",
                                                     m_BufferName, idx, m_Mappings.size() );

        const auto wrote = m_Mappings[idx].Write( data, size, offset );
        if ( !wrote.IsSuccess() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' copy {}: {}", m_BufferName, idx,
                                                     wrote.GetError() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanStorageBuffer::EnsureMapped()
    {
        if ( !m_Built.IsSuccess() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' is not usable: {}", m_BufferName,
                                                     m_Built.GetError() );

        const uint32_t idx = IsPersistent() ? 0u : CopyIndex();
        if ( idx >= m_Mappings.size() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' has no copy {} ({} exist)", m_BufferName,
                                                     idx, m_Mappings.size() );
        if ( !m_Mappings[idx].IsMapped() )
            return Common::MakeFormattedError<bool>( "storage buffer '{}' copy {}: {}", m_BufferName, idx,
                                                     m_Mappings[idx].GetRefusal() );
        return BOOLSUCCESS;
    }

    uint32_t VulkanStorageBuffer::CopyIndex( uint32_t frameIndex )
    {
        return BufferCopyIndex( frameIndex, EngineContext::GetInstance().GetActiveRendererSlot(),
                                Engine::kMaxRendererSlots );
    }

    uint32_t VulkanStorageBuffer::CopyIndex()
    {
        return CopyIndex( EngineContext::GetInstance().GetCurrentFrameIndex() );
    }

} // namespace Desert::ShaderResources::API::Vulkan
