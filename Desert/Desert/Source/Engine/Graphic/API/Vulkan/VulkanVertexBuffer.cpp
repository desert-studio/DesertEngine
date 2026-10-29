#include <Common/Core/DestructorGuard.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanPooledBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/MeshBufferUsage.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>

namespace Desert::Graphic::API::Vulkan
{

    namespace
    {
        static VkBufferCreateInfo CreateVertexBufferInfo( uint32_t size, VkBufferUsageFlags flags,
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

    Common::BoolResultStr VulkanVertexBuffer::SetData( void* data, uint32_t size, uint32_t offset /*= 0 */ )
    {
        // A REFUSAL, NOT A NO-OP, AND THIS ARM IS THE ONE THAT CHANGED MEANING. A static buffer has no
        // mapping to write through — its contents were staged once at Invalidate — so `SetData` on one
        // has always done nothing. It used to do nothing SILENTLY, which is indistinguishable at the
        // call site from a write that landed; a caller updating geometry every frame through a buffer
        // it created as Static would have seen the first frame's data forever with no line anywhere.
        if ( m_Usage != BufferUsage::Dynamic )
            return Common::MakeError<bool>(
                 "vertex buffer is not Dynamic, so it has no mapping to write through" );

        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                              ->GetVulkanAllocator()
                              .get();

        // Unchecked, this wrote `nullptr + offset` on a failed map, and the offset was never bounded
        // against the buffer at all. MappedMemory refuses both and names the numbers; this returns them.
        MappedMemory mapping = allocator->MapMemory( m_MemoryAllocation );
        const auto   wrote   = mapping.Write( data, size, offset );
        if ( !wrote.IsSuccess() )
            return Common::MakeFormattedError<bool>( "vertex buffer SetData wrote nothing: {}", wrote.GetError() );
        return BOOLSUCCESS;
    }

    [[nodiscard]] Common::BoolResultStr VulkanVertexBuffer::Invalidate()
    {
        return RT_Invalidate();
    }

    [[nodiscard]] Common::BoolResultStr VulkanVertexBuffer::RT_Invalidate()
    {
        auto allocator = SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
                              ->GetVulkanAllocator()
                              .get();

        // A static buffer drops its host copy once the copy is on the device (below), so a second upload would
        // send nothing: new contents are a new buffer (every mesh rebuild makes one).
        if ( m_HostCopyReleased )
            return Common::MakeFormattedError<bool>(
                 "static vertex buffer of {} bytes uploaded again after its host copy was released", m_Size );

        if ( m_VulkanBuffer )
        {
            allocator->RT_DestroyBuffer( m_VulkanBuffer, m_MemoryAllocation );
            m_VulkanBuffer     = nullptr;
            m_MemoryAllocation = nullptr;
        }

        if ( m_Usage == BufferUsage::GpuWritten )
        {
            // Device-local, filled by a compute pass (the morph update, the displacement emit) and read as vertices:
            // nothing to upload. MeshBufferUsage's flags, as an uploaded mesh's: a displaced twin's vertices are also
            // the input of its BLAS where the device builds one.
            const VkBufferCreateInfo bufferInfo = CreateVertexBufferInfo(
                 m_Size, MeshBufferUsage( MeshBufferRole::Vertex, allocator->GetDeviceCaps() ),
                 VK_SHARING_MODE_EXCLUSIVE );
            const auto allocation = allocator->RT_AllocateBuffer( "GpuWrittenVertexBuffer", bufferInfo,
                                                                  VMA_MEMORY_USAGE_GPU_ONLY, m_VulkanBuffer );
            if ( !allocation.IsSuccess() )
                return Common::MakeError<bool>( allocation.GetError() );
            m_MemoryAllocation = allocation.GetValue();
            return Common::MakeSuccess( true );
        }

        if ( m_Usage == BufferUsage::Pooled )
        {
            const auto created = RT_CreatePooledBuffer( *allocator, MeshBufferRole::Vertex, m_Size, "PooledVertexBuffer",
                                                        m_VulkanBuffer );
            if ( !created.IsSuccess() )
                return Common::MakeError<bool>( created.GetError() );
            m_MemoryAllocation = created.GetValue();
            return Common::MakeSuccess( true );
        }

        if ( m_Usage == BufferUsage::Dynamic )
        {
            VkBufferCreateInfo bufferInfo =
                 CreateVertexBufferInfo( m_Size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE );

            auto allocation = allocator->RT_AllocateBuffer( "DynamicVertexBuffer", bufferInfo,
                                                            VMA_MEMORY_USAGE_CPU_TO_GPU, m_VulkanBuffer );

            if ( !allocation.IsSuccess() )
                return Common::MakeError<bool>( allocation.GetError() );

            m_MemoryAllocation = allocation.GetValue();

            // initial upload
            if ( m_StorageBuffer.Data )
            {
                MappedMemory mapping = allocator->MapMemory( m_MemoryAllocation );
                const auto   wrote   = mapping.Write( m_StorageBuffer.Data, m_Size );
                if ( !wrote.IsSuccess() )
                    return Common::MakeFormattedError<bool>( "dynamic vertex buffer initial upload: {}",
                                                             wrote.GetError() );
            }

            return Common::MakeSuccess( true );
        }

        if ( m_StorageBuffer.Data == nullptr ) [[unlikely]]
        {
            auto vertexBufferCreateInfo =
                 CreateVertexBufferInfo( m_Size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE );
            // ASKED, NOT ASSUMED. GetValue() on a failed result is a default-constructed VmaAllocation —
            // null — and the two branches beside this one already refuse; this one used to report the
            // whole Invalidate as a success while leaving the buffer bound to nothing.
            const auto allocated = allocator->RT_AllocateBuffer( "VertexBuffer", vertexBufferCreateInfo,
                                                                 VMA_MEMORY_USAGE_CPU_TO_GPU, m_VulkanBuffer );
            if ( !allocated.IsSuccess() )
                return Common::MakeError<bool>( allocated.GetError() );
            m_MemoryAllocation = allocated.GetValue();
        }

        else [[likely]]
        {

            // The usage is MeshBufferUsage's: storage for the morph update, a BLAS build input where the device
            // can build one.
            auto vertexBufferCreateInfo = CreateVertexBufferInfo(
                 m_Size, MeshBufferUsage( MeshBufferRole::Vertex, allocator->GetDeviceCaps() ),
                 VK_SHARING_MODE_EXCLUSIVE );

            const auto buffer = allocator->RT_AllocateBuffer( "VertexBuffer", vertexBufferCreateInfo,
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
                return Common::MakeFormattedError<bool>( "static vertex buffer upload: {}", uploaded.GetError() );
            // UE drops a static buffer's resource array once it is on the RHI (no bNeedsCPUAccess): nothing reads
            // the host copy after the upload (the staging buffer holds the bytes until the copy has run), and
            // holding it doubled every mesh's geometry in the heap (~250 MB on B16_Bistro_Street). The mesh asset
            // keeps its own arrays for the CPU readers.
            m_StorageBuffer.Release();
            m_HostCopyReleased = true;
        }
        return Common::MakeSuccess( true );
    }

    VulkanVertexBuffer::VulkanVertexBuffer( const void* data, uint32_t size,
                                            BufferUsage usage /*= BufferUsage::Static */ )
         : m_Size( size ), m_Usage( usage )
    {
        m_StorageBuffer = Common::Memory::Buffer::Copy( data, size );
    }

    VulkanVertexBuffer::VulkanVertexBuffer( uint32_t size, BufferUsage usage /*= BufferUsage::Dynamic */ )
         : m_Size( size ), m_Usage( usage )
    {
        // A GPU-written buffer has no host copy: nothing ever uploads to it.
        if ( usage != BufferUsage::GpuWritten && usage != BufferUsage::Pooled )
            m_StorageBuffer.Allocate( size );
    }

    Common::BoolResultStr VulkanVertexBuffer::Release()
    {
        m_StorageBuffer.Release();

        SP_CAST( VulkanContext, EngineContext::GetInstance().GetRendererContext() )
             ->GetVulkanAllocator()
             ->RT_DestroyBuffer( m_VulkanBuffer, m_MemoryAllocation );
        m_VulkanBuffer     = nullptr;
        m_MemoryAllocation = nullptr;
        return BOOLSUCCESS;
    }

    VulkanVertexBuffer::~VulkanVertexBuffer()
    try
    {
        // A destructor has no channel, so the report is the log. Left silent, a vertex buffer whose
        // VMA de-allocation refused leaked device memory with nothing anywhere to say a leak had begun —
        // and the symptom of that arrives much later, as an allocation failure in unrelated code.
        const auto released = Release();
        if ( !released.IsSuccess() )
            LOG_ERROR( "[VulkanVertexBuffer] Release failed during destruction: {}", released.GetError() );
    }
    DESERT_DESTRUCTOR_GUARD( "~VulkanVertexBuffer" )

    Common::BoolResultStr VulkanVertexBuffer::RT_WriteRange( const uint64_t offset, const void* data, const uint64_t size )
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

    uint64_t VulkanVertexBuffer::GetDeviceAddress() const
    {
        if ( m_Usage != BufferUsage::Pooled || m_VulkanBuffer == VK_NULL_HANDLE )
            return 0;
        const auto logical = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        if ( !logical || !logical->GetPhysicalDevice()->GetDeviceCaps().Has( Capability::BufferDeviceAddress ) )
            return 0;
        const RayTracingDeviceFunctions& functions = logical->GetRayTracingFunctions();
        if ( functions.GetBufferDeviceAddress == nullptr )
            return 0;
        return functions.BufferAddress( logical->GetVulkanLogicalDevice(), m_VulkanBuffer );
    }
} // namespace Desert::Graphic::API::Vulkan