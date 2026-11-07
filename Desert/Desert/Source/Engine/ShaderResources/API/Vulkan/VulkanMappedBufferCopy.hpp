#pragma once

#include <Engine/ShaderResources/ViewCopiedBlock.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

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

    // The memory a mapped copy lives in (MakeMappedBufferCopy): Upload CPU_TO_GPU (the host writes, the device
    // reads; uncached device-local BAR where the device has it), Readback GPU_TO_CPU (the device writes, the host
    // reads through its cache; MEM-3).
    enum class MappedCopyMemory : uint8_t
    {
        Upload   = 0,
        Readback = 1,
    };

    // One host-mapped VkBuffer: a view's copy for one frame in flight, or a persistent storage buffer's
    // single shared buffer - or, made by MakeDeviceBufferCopy, a device-only storage buffer's unmapped
    // device-local one (its Write refuses: MappedMemory is unmapped). Owns its buffer from the moment it is allocated, so a copy that fails to map is
    // released by the same destructor as one a view drops.
    class MappedBufferCopy final : public IBlockCopy
    {
    public:
        // @p kind is the row this copy opens in the resource ledger (UniformBuffer or StorageBuffer), with the
        // allocation's device bytes: the ledger is the one statement of what a view's copies hold (RT-LEDGER-3).
        MappedBufferCopy( VkBuffer buffer, VmaAllocation allocation, VkDeviceSize size,
                          Desert::Graphic::ResourceKind kind );
        ~MappedBufferCopy() override;

        MappedBufferCopy( const MappedBufferCopy& )            = delete;
        MappedBufferCopy& operator=( const MappedBufferCopy& ) = delete;

        Common::BoolResultStr Write( const void* data, uint32_t size, uint32_t offset ) override
        {
            return m_Mapping.Write( data, size, offset );
        }

        // The device's bytes, after invalidating the host's view of them (a no-op on coherent memory): the
        // caller has waited for the GPU work that wrote them (VulkanStorageBuffer::ReadBack).
        [[nodiscard]] Common::BoolResultStr Read( void* destination, uint32_t size, uint32_t offset );

        [[nodiscard]] ViewCopyBinding Binding() const
        {
            ViewCopyBinding out;
            out.Info.buffer = m_Buffer;
            out.Info.offset = 0;
            out.Info.range  = m_Size;
            out.CopyId      = GetId();
            return out;
        }

        // CPU-GRASS-VARIETY-1: the buffer's device address, read once when the copy is made (a VkBuffer's address
        // is fixed for its life), so a per-frame reader (the grass variety table) makes no driver call and no
        // device cast. 0: the copy was made without VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, or the device has
        // no bufferDeviceAddress.
        [[nodiscard]] uint64_t DeviceAddress() const noexcept
        {
            return m_Address;
        }

    private:
        friend Common::BoolResultStr MakeMappedBufferCopy( std::string_view, VkBufferUsageFlags, uint32_t,
                                                           std::unique_ptr<MappedBufferCopy>&, MappedCopyMemory );
        friend Common::BoolResultStr MakeDeviceBufferCopy( std::string_view, VkBufferUsageFlags, uint32_t,
                                                           std::unique_ptr<MappedBufferCopy>& );

        VkBuffer                      m_Buffer;
        VmaAllocation                 m_Allocation;
        VkDeviceSize                  m_Size;
        uint64_t                      m_Address = 0; // DeviceAddress: set by the Make* that made the copy
        Desert::Graphic::MappedMemory m_Mapping; // persistent for the copy's life; CPU_TO_GPU stays mappable
        // The copy's ledger row: opened under the ambient scope (a view's phase scope tags it with the view's
        // instance, so SceneRenderer::HeldBytes reads it back), closed when the copy is dropped.
        Desert::Graphic::ResourceOwnership m_Ledger;
    };

    // Allocates and maps one CPU_TO_GPU buffer of `size` bytes named `debugName`. REFUSES AS A WHOLE: a
    // buffer that did not allocate or did not map is never handed out. The eager loops this replaced once
    // answered a failed allocation with a bare `continue`, leaving a VK_NULL_HANDLE copy whose zeroed
    // VkDescriptorBufferInfo was then written into a set — undefined behaviour in the driver, visible only
    // as one frame in flight rendering wrong. The refusal carries the allocator's own reason; the caller
    // adds which buffer, view and frame it was for.
    // @p memory Readback (MEM-3, Persistence::PerFrameReadback): GPU_TO_CPU - HOST_VISIBLE | HOST_CACHED where the
    // device has it - for a copy the GPU writes and the host reads back; Upload: CPU_TO_GPU.
    NO_DISCARD Common::BoolResultStr MakeMappedBufferCopy( std::string_view debugName, VkBufferUsageFlags usage,
                                                           uint32_t size, std::unique_ptr<MappedBufferCopy>& out,
                                                           MappedCopyMemory memory = MappedCopyMemory::Upload );

    // GRASS-VIEW-LAW-1 (Persistence::DeviceOnly): allocates one DEVICE-LOCAL buffer of `size` bytes named
    // `debugName` and does not map it - the GPU alone writes and reads it, so it never takes the host-visible
    // device heap (256 MB without resizable BAR). Refuses as a whole, as MakeMappedBufferCopy.
    NO_DISCARD Common::BoolResultStr MakeDeviceBufferCopy( std::string_view debugName, VkBufferUsageFlags usage,
                                                           uint32_t size, std::unique_ptr<MappedBufferCopy>& out );
} // namespace Desert::ShaderResources::API::Vulkan
