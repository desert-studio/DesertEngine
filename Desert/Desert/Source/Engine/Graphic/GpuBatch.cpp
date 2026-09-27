#include <Engine/Graphic/GpuBatch.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanGpuBatch.hpp>

#include <Engine/Graphic/RendererAPI.hpp>

namespace Desert::Graphic
{
    Common::ResultStr<std::unique_ptr<GpuBatch>> GpuBatch::Begin()
    {
        if ( RendererAPI::GetAPIType() == RendererAPIType::Vulkan )
            return API::Vulkan::VulkanGpuBatch::Begin();
        return Common::MakeError<std::unique_ptr<GpuBatch>>(
             "no Vulkan renderer is active; a GPU batch needs one." );
    }
} // namespace Desert::Graphic
