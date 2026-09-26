#pragma once

#include <Engine/ShaderResources/ViewCopiedBlock.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>

#include <memory>
#include <string_view>

namespace Desert::ShaderResources::API::Vulkan
{
    // What a descriptor write for one buffer copy needs: the buffer, and the id of the copy it belongs to,
    // which the material backend compares with what the set was last written with (ViewCopiedBlock.hpp,
    // DescriptorCopyRecord). Shared by VulkanUniformBuffer and VulkanStorageBuffer so both buffer kinds
    // answer the descriptor question in one shape.
    struct ViewCopyBinding
    {
        VkDescriptorBufferInfo Info{};
        uint64_t               CopyId = 0;
    };

    // One host-mapped VkBuffer: a view's copy for one frame in flight, or a persistent storage buffer's
    // single shared buffer. Owns its buffer from the moment it is allocated, so a copy that fails to map is
    // released by the same destructor as one a view drops.
    class MappedBufferCopy final : public IBlockCopy
    {
    public:
        MappedBufferCopy( VkBuffer buffer, VmaAllocation allocation, VkDeviceSize size );
        ~MappedBufferCopy() override;

        MappedBufferCopy( const MappedBufferCopy& )            = delete;
        MappedBufferCopy& operator=( const MappedBufferCopy& ) = delete;

        Common::BoolResultStr Write( const void* data, uint32_t size, uint32_t offset ) override
        {
            return m_Mapping.Write( data, size, offset );
        }

        [[nodiscard]] uint64_t HeldBytes() const noexcept override
        {
            return m_Size;
        }

        [[nodiscard]] ViewCopyBinding Binding() const
        {
            ViewCopyBinding out;
            out.Info.buffer = m_Buffer;
            out.Info.offset = 0;
            out.Info.range  = m_Size;
            out.CopyId      = GetId();
            return out;
        }

    private:
        friend Common::BoolResultStr MakeMappedBufferCopy( std::string_view, VkBufferUsageFlags, uint32_t,
                                                           std::unique_ptr<MappedBufferCopy>& );

        VkBuffer                      m_Buffer;
        VmaAllocation                 m_Allocation;
        VkDeviceSize                  m_Size;
        Desert::Graphic::MappedMemory m_Mapping; // persistent for the copy's life; CPU_TO_GPU stays mappable
    };

    // Allocates and maps one CPU_TO_GPU buffer of `size` bytes named `debugName`. REFUSES AS A WHOLE: a
    // buffer that did not allocate or did not map is never handed out. The eager loops this replaced once
    // answered a failed allocation with a bare `continue`, leaving a VK_NULL_HANDLE copy whose zeroed
    // VkDescriptorBufferInfo was then written into a set — undefined behaviour in the driver, visible only
    // as one frame in flight rendering wrong. The refusal carries the allocator's own reason; the caller
    // adds which buffer, view and frame it was for.
    NO_DISCARD Common::BoolResultStr MakeMappedBufferCopy( std::string_view debugName, VkBufferUsageFlags usage,
                                                           uint32_t size, std::unique_ptr<MappedBufferCopy>& out );
} // namespace Desert::ShaderResources::API::Vulkan
