#include <Engine/Graphic/MipMapGenerator.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::Graphic
{

    std::unique_ptr<MipMapCubeGenerator> MipMapCubeGenerator::Create( MipGenStrategy strategy )
    {
        return Renderer::GetInstance().Rhi().CreateMipMapCubeGenerator( strategy );
    }

} // namespace Desert::Graphic
