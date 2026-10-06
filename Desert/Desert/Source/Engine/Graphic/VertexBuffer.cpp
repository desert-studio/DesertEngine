#include <Engine/Graphic/VertexBuffer.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>

namespace Desert::Graphic
{

    std::shared_ptr<Desert::Graphic::VertexBuffer>
    VertexBuffer::Create( const void* data, uint32_t size, BufferUsage usage /*= BufferUsage::Static */ )
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::Vulkan:
            {
                return std::make_shared<API::Vulkan::VulkanVertexBuffer>( data, size, usage );
            }
            // NAMED RATHER THAN LEFT TO FALL THROUGH. `None` is the enum's zero, not a backend, and the
            // verify below is what answers it — but with the case unwritten this switch also stayed silent
            // the day a SECOND backend is added, which is the one moment a factory needs to complain.
            case RendererAPIType::None:
                break;
        }

        DESERT_VERIFY( false );
        return nullptr;
    }

    std::shared_ptr<Desert::Graphic::VertexBuffer>
    VertexBuffer::Create( uint32_t size, BufferUsage usage /*= BufferUsage::Dynamic */ )
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::Vulkan:
            {
                return std::make_shared<API::Vulkan::VulkanVertexBuffer>( size, usage );
            }
            // NAMED RATHER THAN LEFT TO FALL THROUGH. `None` is the enum's zero, not a backend, and the
            // verify below is what answers it — but with the case unwritten this switch also stayed silent
            // the day a SECOND backend is added, which is the one moment a factory needs to complain.
            case RendererAPIType::None:
                break;
        }

        DESERT_VERIFY( false );
        return nullptr;
    }

} // namespace Desert::Graphic