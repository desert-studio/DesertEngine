#include <Engine/Graphic/API/Vulkan/VulkanGpuBatch.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>

#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    Common::ResultStr<std::unique_ptr<GpuBatch>> VulkanGpuBatch::Begin()
    {
        const auto     device   = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        const auto&    physical = device->GetPhysicalDevice();
        const uint32_t family   = physical->GetGraphicsFamily();

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties( physical->GetVulkanPhysicalDevice(), &familyCount, nullptr );
        std::vector<VkQueueFamilyProperties> families( familyCount );
        vkGetPhysicalDeviceQueueFamilyProperties( physical->GetVulkanPhysicalDevice(), &familyCount,
                                                  families.data() );
        if ( family >= familyCount || ( families[family].queueFlags & VK_QUEUE_COMPUTE_BIT ) == 0 )
            return Common::MakeFormattedError<std::unique_ptr<GpuBatch>>(
                 "the graphics queue family {} (of {}) does not advertise compute, and a GPU batch records "
                 "compute dispatches on it.",
                 family, familyCount );

        auto recording = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !recording )
            return Common::MakeError<std::unique_ptr<GpuBatch>>( recording.GetError() );
        return Common::MakeSuccess<std::unique_ptr<GpuBatch>>(
             std::unique_ptr<GpuBatch>( new VulkanGpuBatch( recording.GetValue() ) ) );
    }

    VulkanGpuBatch::~VulkanGpuBatch()
    {
        // A batch recorded and never submitted still holds a one-off buffer: submitting and waiting is
        // the one way CommandBufferAllocator takes it back. Both are teardown-only paths.
        if ( m_Recording != VK_NULL_HANDLE )
            (void)Submit();
        if ( m_Submitted )
            CommandBufferAllocator::GetInstance().RT_ReleaseSubmitted( *m_Submitted );
    }

    Common::BoolResultStr VulkanGpuBatch::Submit()
    {
        if ( m_Recording == VK_NULL_HANDLE )
            return Common::MakeError( "the GPU batch was already submitted." );
        const VkCommandBuffer recording = m_Recording;
        m_Recording                     = VK_NULL_HANDLE;
        auto submitted = CommandBufferAllocator::GetInstance().RT_SubmitCommandBufferGraphic( recording );
        if ( !submitted )
            return Common::MakeError( submitted.GetError() );
        m_Submitted = submitted.GetValue();
        return Common::MakeSuccess( true );
    }

    bool VulkanGpuBatch::IsComplete() const
    {
        return m_Submitted && CommandBufferAllocator::GetInstance().IsComplete( *m_Submitted );
    }

    void VulkanGpuBatch::Wait()
    {
        if ( !m_Submitted )
            return;
        CommandBufferAllocator::GetInstance().RT_ReleaseSubmitted( *m_Submitted );
        m_Submitted.reset();
    }

    VkCommandBuffer RecordingBuffer( GpuBatch& batch )
    {
        return static_cast<VulkanGpuBatch&>( batch ).Commands();
    }
} // namespace Desert::Graphic::API::Vulkan
