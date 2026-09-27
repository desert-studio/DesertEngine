#pragma once

#include <Engine/Graphic/GpuBatch.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>

#include <vulkan/vulkan.h>

#include <optional>

namespace Desert::Graphic::API::Vulkan
{
    /// GpuBatch over one CommandBufferAllocator one-off graphics buffer, submitted with its own fence.
    class VulkanGpuBatch final : public GpuBatch
    {
    public:
        /// Refuses, naming the family, when the graphics queue cannot run compute: every recorder of a
        /// batch dispatches compute on it.
        static Common::ResultStr<std::unique_ptr<GpuBatch>> Begin();

        ~VulkanGpuBatch() override;

        /// The buffer commands are recorded into. VK_NULL_HANDLE once submitted.
        [[nodiscard]] VkCommandBuffer Commands() const
        {
            return m_Recording;
        }

        Common::BoolResultStr Submit() override;
        [[nodiscard]] bool    IsComplete() const override;
        void                  Wait() override;

    private:
        explicit VulkanGpuBatch( VkCommandBuffer recording ) : m_Recording( recording )
        {
        }

        VkCommandBuffer                                  m_Recording = VK_NULL_HANDLE;
        std::optional<CommandBufferAllocator::Submitted> m_Submitted;
    };

    /// The command buffer of @p batch, which on this backend is always a VulkanGpuBatch.
    [[nodiscard]] VkCommandBuffer RecordingBuffer( GpuBatch& batch );
} // namespace Desert::Graphic::API::Vulkan
