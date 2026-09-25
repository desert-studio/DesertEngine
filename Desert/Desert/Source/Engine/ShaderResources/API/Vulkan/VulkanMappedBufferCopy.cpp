#include "VulkanMappedBufferCopy.hpp"

#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>

#include <Engine/Core/EngineContext.hpp>

namespace Desert::ShaderResources::API::Vulkan
{
    namespace
    {
        Desert::Graphic::API::Vulkan::VulkanAllocator* Allocator()
        {
            return SP_CAST( Desert::Graphic::API::Vulkan::VulkanContext,
                            EngineContext::GetInstance().GetRendererContext() )
                 ->GetVulkanAllocator()
                 .get();
        }
    } // namespace

    MappedBufferCopy::MappedBufferCopy( VkBuffer buffer, VmaAllocation allocation, VkDeviceSize size )
         : m_Buffer( buffer ), m_Allocation( allocation ), m_Size( size )
    {
    }

    MappedBufferCopy::~MappedBufferCopy()
    {
        // Unmap BEFORE the buffer is queued for destruction; RT_DestroyBuffer defers the release, which is
        // what makes dropping a copy legal at any point of a frame.
        m_Mapping.Unmap();
        Allocator()->RT_DestroyBuffer( m_Buffer, m_Allocation );
    }

    Common::BoolResultStr MakeMappedBufferCopy( std::string_view debugName, VkBufferUsageFlags usage,
                                                uint32_t size, std::unique_ptr<MappedBufferCopy>& out )
    {
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.usage              = usage;
        bufferInfo.size               = size;

        auto*      allocator = Allocator();
        VkBuffer   buffer    = VK_NULL_HANDLE;
        const auto allocated = allocator->RT_AllocateBuffer( std::string( debugName ), bufferInfo,
                                                             VMA_MEMORY_USAGE_CPU_TO_GPU, buffer );
        if ( !allocated.IsSuccess() )
            return Common::MakeFormattedError<bool>( "{} byte(s) could not be allocated: {}", size,
                                                     allocated.GetError() );

        auto copy       = std::make_unique<MappedBufferCopy>( buffer, allocated.GetValue(), size );
        copy->m_Mapping = allocator->MapMemory( copy->m_Allocation );

        // A COPY THAT DID NOT MAP IS THE SAME FAILURE AS ONE THAT DID NOT ALLOCATE: every write to it would
        // refuse for its whole life. Refused here, and the copy's destructor gives the buffer back.
        if ( !copy->m_Mapping.IsMapped() )
            return Common::MakeFormattedError<bool>( "{} byte(s) allocated but not mapped: {}", size,
                                                     copy->m_Mapping.GetRefusal() );

        out = std::move( copy );
        return BOOLSUCCESS;
    }
} // namespace Desert::ShaderResources::API::Vulkan
