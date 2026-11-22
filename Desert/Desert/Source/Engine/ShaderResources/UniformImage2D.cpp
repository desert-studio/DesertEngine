#include <Engine/ShaderResources/UniformImage2D.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/ShaderResources/API/Vulkan/VulkanUniformImage2D.hpp>


namespace Desert::ShaderResources
{

    std::shared_ptr<UniformImage2D> UniformImage2D::Create( const std::string_view debugName, uint32_t binding )
    {
        switch ( Graphic::RendererAPI::GetAPIType() )
        {
            case Graphic::RendererAPIType::None:
                return nullptr;
            case Graphic::RendererAPIType::Vulkan:
                return std::make_shared<API::Vulkan::VulkanUniformImage2D>( debugName, binding );
        }
        DESERT_VERIFY( false, "Unknown RendererAPI" );
        return nullptr;
    }

} // namespace Desert::ShaderResources
