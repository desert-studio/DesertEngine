#include <Engine/ShaderResources/UniformImageCube.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/ShaderResources/API/Vulkan/VulkanUniformImageCube.hpp>


namespace Desert::ShaderResources
{

    std::shared_ptr<UniformImageCube>
    UniformImageCube::Create( const std::string_view debugName, uint32_t binding )
    {
        switch ( Graphic::RendererAPI::GetAPIType() )
        {
            case Graphic::RendererAPIType::None:
                return nullptr;
            case Graphic::RendererAPIType::Vulkan:
                return std::make_shared<API::Vulkan::VulkanUniformImageCube>( debugName, binding );
        }
        DESERT_VERIFY( false, "Unknown RendererAPI" );
        return nullptr;
    }

} // namespace Desert::ShaderResources
