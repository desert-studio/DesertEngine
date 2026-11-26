#include <Engine/ShaderResources/UniformImageCube.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::ShaderResources
{
    std::shared_ptr<UniformImageCube>
    UniformImageCube::Create( const std::string_view debugName, uint32_t binding )
    {
        return Graphic::Renderer::GetInstance().Rhi().CreateUniformImageCube( debugName, binding );
    }

} // namespace Desert::ShaderResources
