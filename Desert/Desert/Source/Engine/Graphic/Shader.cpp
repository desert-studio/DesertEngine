#include <Engine/Graphic/Shader.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>

#include <Engine/Graphic/RendererAPI.hpp>

namespace Desert::Graphic
{

    std::shared_ptr<Shader> Shader::Create( const Assets::Asset<Assets::ShaderAsset>& asset,
                                            const ShaderVariant& variant, const std::string& passName )
    {
        std::shared_ptr<Shader> shader = nullptr;
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::Vulkan:
            {
                shader = std::make_shared<API::Vulkan::VulkanShader>( asset, variant, passName );
            }
            // NAMED RATHER THAN LEFT TO FALL THROUGH. `None` is the enum's zero, not a backend, and the
            // verify below is what answers it — but with the case unwritten this switch also stayed silent
            // the day a SECOND backend is added, which is the one moment a factory needs to complain.
            case RendererAPIType::None:
                break;
        }

        DESERT_VERIFY( shader );
        return shader;
    }

} // namespace Desert::Graphic