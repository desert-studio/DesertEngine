#include <Engine/ShaderResources/UniformImage2D.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::ShaderResources
{
    std::shared_ptr<UniformImage2D> UniformImage2D::Create( const std::string_view debugName, uint32_t binding )
    {
        return Graphic::Renderer::GetInstance().Rhi().CreateUniformImage2D( debugName, binding );
    }

} // namespace Desert::ShaderResources
