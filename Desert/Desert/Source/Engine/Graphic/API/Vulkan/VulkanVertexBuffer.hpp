#pragma once

#include <Engine/Graphic/VertexBuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Common/Core/Memory/Buffer.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>

#include <vulkan/vulkan.h>

namespace Desert::Graphic::API::Vulkan
{
    // @p buffer's device address through the device's address table (VulkanRayTracingFunctions.hpp): 0 on a
    // device that has none, or for VK_NULL_HANDLE. The one reader behind every geometry buffer's
    // GetDeviceAddress.
    [[nodiscard]] uint64_t CurrentBufferAddress( VkBuffer buffer );

    class VulkanVertexBuffer : public VertexBuffer
    {
    public:
        virtual ~VulkanVertexBuffer();

        VulkanVertexBuffer( const void* data, uint32_t size, BufferUsage usage = BufferUsage::Static );
        VulkanVertexBuffer( uint32_t size, BufferUsage usage = BufferUsage::Dynamic );

        NO_DISCARD virtual Common::BoolResultStr SetData( void* data, uint32_t size,
                                                          uint32_t offset = 0 ) override;

        [[nodiscard]] virtual Common::BoolResultStr Invalidate() override;
        [[nodiscard]] virtual Common::BoolResultStr Release() override;
        [[nodiscard]] virtual Common::BoolResultStr RT_Invalidate() override;
        [[nodiscard]] Common::BoolResultStr         RT_WriteRange( uint64_t offset, const void* data,
                                                                   uint64_t size ) override;
        [[nodiscard]] uint64_t GetDeviceAddress() const override;

        virtual unsigned int GetSize() const override
        {
            return m_Size;
        }

        [[nodiscard]] VkBuffer GetVulkanBuffer() const
        {
            return m_VulkanBuffer;
        }
        [[nodiscard]] BufferUsage GetUsage() const
        {
            return m_Usage;
        }

        // What the last render graph that imported this buffer left it in (VulkanRendererAPI::ImportVertexBuffer),
        // so the next graph starts from the truth - the same record VulkanStorageBuffer keeps. Physical is the
        // graph's non-owning handle on m_VulkanBuffer, rebuilt when the buffer is (Invalidate).
        struct GraphRecord
        {
            VkBuffer                                               Buffer = VK_NULL_HANDLE;
            std::shared_ptr<Desert::Graphic::RDG::IPhysicalBuffer> Physical;
            Desert::Graphic::RDG::AccessState                      State;
        };
        GraphRecord& GetGraphRecord()
        {
            return m_GraphRecord;
        }

    private:
        GraphRecord m_GraphRecord;

        uint32_t    m_Size = 0;
        BufferUsage m_Usage;

        VkBuffer               m_VulkanBuffer = nullptr;
        VmaAllocation          m_MemoryAllocation;
        Common::Memory::Buffer m_StorageBuffer;
        // The static upload released m_StorageBuffer (RT_Invalidate): the buffer cannot be uploaded again.
        bool m_HostCopyReleased = false;
    };
} // namespace Desert::Graphic::API::Vulkan