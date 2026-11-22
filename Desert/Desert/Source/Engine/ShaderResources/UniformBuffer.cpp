#include <Engine/ShaderResources/UniformBuffer.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/ShaderResources/API/Vulkan/VulkanUniformBuffer.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanShader.hpp>
#include <numeric>
#include <Common/Core/ResultStr.hpp>
#include <glm/glm.hpp>
#include <Common/Core/Memory/Buffer.hpp>
#include <Engine/Graphic/Shader.hpp>


namespace Desert::ShaderResources
{

    std::shared_ptr<UniformBuffer> UniformBuffer::Create( const ShaderLayout::UniformBuffer& uniform )
    {
        switch ( Graphic::RendererAPI::GetAPIType() )
        {
            case Graphic::RendererAPIType::None:
                return nullptr;
            case Graphic::RendererAPIType::Vulkan:
            {
                // Refused rather than handed back half-built — see StorageBuffer::Create for the whole
                // argument; it applies here word for word.
                auto       buffer = std::make_shared<API::Vulkan::VulkanUniformBuffer>( uniform );
                const auto usable = buffer->EnsureMapped();
                if ( !usable.IsSuccess() )
                {
                    LOG_ERROR( "[UniformBuffer] '{}' ({} bytes) is not usable and was not created: {}",
                               uniform.Name, uniform.Size, usable.GetError() );
                    return nullptr;
                }
                return buffer;
            }
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
        return nullptr;
    }

} // namespace Desert::ShaderResources
