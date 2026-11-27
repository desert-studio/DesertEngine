#include <Engine/Graphic/API/Vulkan/VulkanRecordContext.hpp>

namespace Desert::Graphic::API::Vulkan
{
    auto VulkanRecordContext::Bind( VulkanRecordContext* context ) noexcept -> VulkanRecordContext*
    {
        VulkanRecordContext* const previous = t_Bound;
        t_Bound                             = context;
        return previous;
    }

    auto VulkanRecordContext::Bound() noexcept -> VulkanRecordContext*
    {
        return t_Bound;
    }

    auto VulkanBoundCompute::On( VkCommandBuffer commandBuffer ) noexcept -> bool
    {
        if ( CommandBuffer == commandBuffer && Epoch == t_ForeignBinds )
            return true;
        Forget();
        CommandBuffer = commandBuffer;
        Epoch         = t_ForeignBinds;
        return false;
    }

    auto VulkanBoundCompute::ForeignBind() noexcept -> void
    {
        ++t_ForeignBinds;
    }

    thread_local VulkanRecordContext* VulkanRecordContext::t_Bound = nullptr;

    thread_local uint64_t VulkanBoundCompute::t_ForeignBinds = 0;
}
