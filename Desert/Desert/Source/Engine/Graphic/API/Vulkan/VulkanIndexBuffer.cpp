#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanIndexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanPooledBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/MeshBufferUsage.hpp>
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
        if ( usage != BufferUsage::Pooled )
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

    uint64_t VulkanIndexBuffer::GetDeviceAddress() const
    {
        return CurrentBufferAddress( m_VulkanBuffer );
    }

    Common::BoolResultStr VulkanIndexBuffer::RT_Invalidate()
    {
        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                              ->GetVulkanAllocator()
                              .get();

        // A static buffer drops its host copy once the copy is on the device (below), so a second upload would
        // send nothing: new contents are a new buffer (every mesh rebuild makes one).
        if ( m_HostCopyReleased )
            return Common::MakeFormattedError<bool>(
                 "static index buffer of {} bytes uploaded again after its host copy was released", m_Size );

        if ( m_VulkanBuffer )
        {
            allocator->RT_DestroyBuffer( m_VulkanBuffer, m_MemoryAllocation );
            m_VulkanBuffer     = nullptr;
            m_MemoryAllocation = nullptr;
        }

        if ( m_Usage == BufferUsage::Pooled )
        {
            const auto created = RT_CreatePooledBuffer( *allocator, MeshBufferRole::Index, m_Size, "PooledIndexBuffer",
                                                        m_VulkanBuffer );
            if ( !created.IsSuccess() )
                return Common::MakeError<bool>( created.GetError() );
            m_MemoryAllocation = created.GetValue();
            return Common::MakeSuccess( true );
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

            auto vertexBufferCreateInfo = CreateIndexBufferInfo(
                 m_Size, MeshBufferUsage( MeshBufferRole::Index, allocator->GetDeviceCaps() ),
                 VK_SHARING_MODE_EXCLUSIVE );

            const auto buffer = allocator->RT_AllocateBuffer( "IndexBuffer", vertexBufferCreateInfo,
                                                              VMA_MEMORY_USAGE_GPU_ONLY, m_VulkanBuffer );
            if ( !buffer.IsSuccess() )
            {
                return Common::MakeError<bool>( buffer.GetError() );
            }

            m_MemoryAllocation = buffer.GetValue();

            // THE ONE BUFFER UPLOAD PATH, submitted and not waited for (FPS-CPU-3): a mesh load no longer sleeps the
            // loading thread on the whole GPU queue ahead of it. The barriers it records order the copy after every
            // earlier submission and before every later one, so a draw or a BLAS build of this buffer reads it whole.
            const auto uploaded = RT_UploadDeviceBuffer( *allocator, m_VulkanBuffer, 0, m_StorageBuffer.Data,
                                                         m_StorageBuffer.Size );
            if ( !uploaded.IsSuccess() )
                return Common::MakeFormattedError<bool>( "static index buffer upload: {}", uploaded.GetError() );
            // UE drops a static buffer's resource array once it is on the RHI (no bNeedsCPUAccess): nothing reads
            // the host copy after the upload (the staging buffer holds the bytes until the copy has run), and
            // holding it doubled every mesh's geometry in the heap (~250 MB on B16_Bistro_Street). The mesh asset
            // keeps its own arrays for the CPU readers.
            m_StorageBuffer.Release();
            m_HostCopyReleased = true;
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

    Common::BoolResultStr VulkanIndexBuffer::RT_WriteRange( const uint64_t offset, const void* data, const uint64_t size )
    {
        if ( m_Usage != BufferUsage::Pooled )
            return Common::MakeError<bool>( "RT_WriteRange: not a Pooled buffer" );
        if ( offset + size > m_Size )
            return Common::MakeFormattedError<bool>( "RT_WriteRange: [{}, {}) past a {}-byte buffer", offset,
                                                     offset + size, m_Size );
        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                              ->GetVulkanAllocator()
                              .get();
        return RT_UploadDeviceBuffer( *allocator, m_VulkanBuffer, offset, data, size );
    }
} // namespace Desert::Graphic::API::Vulkan