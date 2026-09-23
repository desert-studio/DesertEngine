#include <Engine/Graphic/MipMapGenerator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanMipMapGenerator.hpp>

#include <Engine/Graphic/RendererAPI.hpp>

namespace Desert::Graphic
{

    std::unique_ptr<MipMapCubeGenerator> MipMapCubeGenerator::Create( MipGenStrategy strategy )
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                return nullptr;
            case RendererAPIType::Vulkan:
            {
                if ( strategy == MipGenStrategy::ComputeShader )
                {
                    DESERT_VERIFY( false, "ComputeShader was not impl" );
                }
                return std::make_unique<API::Vulkan::VulkanMipMapCubeGeneratorTO>();
            }
        }
        DESERT_VERIFY( false, "Unknown RendererAPI" );
        return nullptr;
    }

} // namespace Desert::Graphic