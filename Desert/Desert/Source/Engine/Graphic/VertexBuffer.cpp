#include <Engine/Graphic/VertexBuffer.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanVertexBuffer.hpp>

#include <filesystem>
#include <format>

namespace Desert::Graphic
{

    std::shared_ptr<VertexBuffer> VertexBuffer::NamedAfter( std::shared_ptr<VertexBuffer> buffer, const char* file,
                                                            uint32_t line )
    {
        if ( buffer )
            buffer->m_Accounting.Name(
                 std::format( "{}:{}", std::filesystem::path( file ).filename().string(), line ) );
        return buffer;
    }

    std::shared_ptr<Desert::Graphic::VertexBuffer>
    VertexBuffer::Create( const void* data, uint32_t size, BufferUsage usage /*= BufferUsage::Static */,
                          const char* file, uint32_t line )
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::Vulkan:
            {
                return NamedAfter( std::make_shared<API::Vulkan::VulkanVertexBuffer>( data, size, usage ), file,
                                   line );
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
    VertexBuffer::Create( uint32_t size, BufferUsage usage /*= BufferUsage::Dynamic */, const char* file,
                          uint32_t line )
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::Vulkan:
            {
                return NamedAfter( std::make_shared<API::Vulkan::VulkanVertexBuffer>( size, usage ), file, line );
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