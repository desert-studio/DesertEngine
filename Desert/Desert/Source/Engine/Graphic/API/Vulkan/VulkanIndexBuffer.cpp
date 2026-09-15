#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanIndexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>

#include <Engine/Core/EngineContext.hpp>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        static VkBufferCreateInfo CreateIndexBufferInfo( uint32_t size, VkBufferUsageFlags flags,
                                                         VkSharingMode sharingMode )
        {
            VkBufferCreateInfo vertexBufferCreateInfo = {};
            vertexBufferCreateInfo.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            vertexBufferCreateInfo.size               = size;
            vertexBufferCreateInfo.usage              = flags;
            vertexBufferCreateInfo.sharingMode        = sharingMode;

            return vertexBufferCreateInfo;
        }
    } // namespace

    VulkanIndexBuffer::VulkanIndexBuffer( const void* data, uint32_t size,
                                          BufferUsage usage /*= BufferUsage::Static */ )
         : m_Size( size ), m_Usage( usage )
    {
        m_StorageBuffer = Common::Memory::Buffer::Copy( data, size );
    }

    VulkanIndexBuffer::VulkanIndexBuffer( uint32_t size, BufferUsage usage /*= BufferUsage::Dynamic */ )
         : m_Size( size ), m_Usage( usage )
    {
        m_StorageBuffer.Allocate( size );
    }

    Common::BoolResultStr VulkanIndexBuffer::SetData( void* data, uint32_t size, uint32_t offset )
    {
        // A refusal rather than a silent no-op — see the sibling arm in VulkanVertexBuffer::SetData for
        // the defect that silence hid.
        if ( m_Usage != BufferUsage::Dynamic )
            return Common::MakeError<bool>( "index buffer is not Dynamic, so it has no mapping to write through" );

        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                              ->GetVulkanAllocator()
                              .get();

        // THE OFFSET IS HONOURED, and until this commit it was not. `IndexBuffer::SetData` declares
        // `uint32_t offset = 0` and `VulkanVertexBuffer::SetData` — the same signature, twenty lines away
        // in the sibling file — writes at `dst + offset`; this one wrote at `dst` and ignored the argument
        // entirely. Two implementations of one interface, one of which quietly drops a parameter: any
        // caller passing a non-zero offset would have overwritten the head of the buffer with no error
        // anywhere. Every current caller passes 0 or omits it, so nothing changes today; what changes is
        // that the interface is now telling the truth.
        //
        // The offset is BOUNDED as well as honoured: `dst + offset` past the end of the mapping
        // corrupted whatever VMA had placed after it.
        MappedMemory mapping = allocator->MapMemory( m_MemoryAllocation );
        const auto   wrote   = mapping.Write( data, size, offset );
        if ( !wrote.IsSuccess() )
            return Common::MakeFormattedError<bool>( "index buffer SetData wrote nothing: {}", wrote.GetError() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanIndexBuffer::Invalidate()
    {
        return RT_Invalidate();
    }

    Common::BoolResultStr VulkanIndexBuffer::RT_Invalidate()
    {
        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                              ->GetVulkanAllocator()
                              .get();

        if ( m_VulkanBuffer )
        {
            allocator->RT_DestroyBuffer( m_VulkanBuffer, m_MemoryAllocation );
            m_VulkanBuffer     = nullptr;
            m_MemoryAllocation = nullptr;
        }

        if ( m_Usage == BufferUsage::Dynamic )
        {
            VkBufferCreateInfo bufferInfo =
                 CreateIndexBufferInfo( m_Size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE );

            auto allocation = allocator->RT_AllocateBuffer( "DynamicIndexBuffer", bufferInfo,
                                                            VMA_MEMORY_USAGE_CPU_TO_GPU, m_VulkanBuffer );

            if ( !allocation.IsSuccess() )
                return Common::MakeError<bool>( allocation.GetError() );

            m_MemoryAllocation = allocation.GetValue();

            if ( m_StorageBuffer.Data )
            {
                MappedMemory mapping = allocator->MapMemory( m_MemoryAllocation );
                const auto   wrote   = mapping.Write( m_StorageBuffer.Data, m_Size );
                if ( !wrote.IsSuccess() )
                    return Common::MakeFormattedError<bool>( "dynamic index buffer initial upload: {}",
                                                             wrote.GetError() );
            }

            return Common::MakeSuccess( true );
        }

        if ( m_StorageBuffer.Data == nullptr ) [[unlikely]]
        {
            auto vertexBufferCreateInfo =
                 CreateIndexBufferInfo( m_Size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE );
            // ASKED, NOT ASSUMED — see the sibling line in VulkanVertexBuffer::RT_Invalidate. The tag
            // said "VertexBuffer" here too, in the index buffer, so a VMA leak report named the wrong
            // resource.
            const auto allocated = allocator->RT_AllocateBuffer( "IndexBuffer", vertexBufferCreateInfo,
                                                                 VMA_MEMORY_USAGE_CPU_TO_GPU, m_VulkanBuffer );
            if ( !allocated.IsSuccess() )
                return Common::MakeError<bool>( allocated.GetError() );
            m_MemoryAllocation = allocated.GetValue();
        }

        else [[likely]]
        {

            VkBufferCreateInfo stagingBufferCreateInfo =
                 CreateIndexBufferInfo( m_Size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE );

            VkBuffer stagingBuffer;
            auto     stagingBufferAllocation = allocator->RT_AllocateBuffer(
                 "IndexBuffer_staging", stagingBufferCreateInfo, VMA_MEMORY_USAGE_CPU_TO_GPU, stagingBuffer );

            if ( !stagingBufferAllocation.IsSuccess() )
            {
                return Common::MakeError<bool>( stagingBufferAllocation.GetError() );
            }

            auto stagingBufferAllocationVAL = stagingBufferAllocation.GetValue();

            // copy data to staging buffer
            {
                MappedMemory staged = allocator->MapMemory( stagingBufferAllocationVAL );
                const auto   wrote  = staged.Write( m_StorageBuffer.Data, m_StorageBuffer.Size );
                if ( !wrote.IsSuccess() )
                {
                    staged.Unmap();
                    allocator->RT_DestroyBuffer( stagingBuffer, stagingBufferAllocationVAL );
                    return Common::MakeFormattedError<bool>( "index buffer staging upload: {}", wrote.GetError() );
                }
            }

            auto vertexBufferCreateInfo = CreateIndexBufferInfo(
                 m_Size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_SHARING_MODE_EXCLUSIVE );

            const auto buffer = allocator->RT_AllocateBuffer( "IndexBuffer", vertexBufferCreateInfo,
                                                              VMA_MEMORY_USAGE_GPU_ONLY, m_VulkanBuffer );
            if ( !buffer.IsSuccess() )
            {
                return Common::MakeError<bool>( buffer.GetError() );
            }

            m_MemoryAllocation = buffer.GetValue();
            auto copyCmd       = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
            if ( !copyCmd.IsSuccess() )
            {
                return Common::MakeError<bool>( copyCmd.GetError() );
            }

            auto copyCmdVal = copyCmd.GetValue();

            VkBufferCopy copyRegion = {};
            copyRegion.size         = m_Size;

            vkCmdCopyBuffer( copyCmdVal, stagingBuffer, m_VulkanBuffer, 1, &copyRegion );

            CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( copyCmdVal );

            allocator->RT_DestroyBuffer( stagingBuffer, stagingBufferAllocationVAL );
        }
        return Common::MakeSuccess( true );
    }

    VulkanIndexBuffer::~VulkanIndexBuffer()
    try
    {
        // A destructor has no channel, so the report is the log. Left silent, a index buffer whose
        // VMA de-allocation refused leaked device memory with nothing anywhere to say a leak had begun —
        // and the symptom of that arrives much later, as an allocation failure in unrelated code.
        const auto released = Release();
        if ( !released.IsSuccess() )
            LOG_ERROR( "[VulkanIndexBuffer] Release failed during destruction: {}", released.GetError() );
    }
    DESERT_DESTRUCTOR_GUARD( "~VulkanIndexBuffer" )

    Common::BoolResultStr VulkanIndexBuffer::Release()
    {
        m_StorageBuffer.Release();

        SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
             ->GetVulkanAllocator()
             ->RT_DestroyBuffer( m_VulkanBuffer, m_MemoryAllocation );
        m_VulkanBuffer     = nullptr;
        m_MemoryAllocation = nullptr;
        return BOOLSUCCESS;
    }

} // namespace Desert::Graphic::API::Vulkan