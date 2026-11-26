#include <Engine/ShaderResources/UniformBuffer.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::ShaderResources
{
    // Refused rather than handed back half-built (IRhi::CreateUniformBuffer logs why); null = refused.
    std::shared_ptr<UniformBuffer> UniformBuffer::Create( const ShaderLayout::UniformBuffer& uniform )
    {
        return Graphic::Renderer::GetInstance().Rhi().CreateUniformBuffer( uniform );
    }

} // namespace Desert::ShaderResources
