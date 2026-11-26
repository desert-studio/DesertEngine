#include <Engine/Graphic/VertexBuffer.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

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
        return NamedAfter( Renderer::GetInstance().Rhi().CreateVertexBuffer( data, size, usage ), file, line );
    }

    std::shared_ptr<Desert::Graphic::VertexBuffer>
    VertexBuffer::Create( uint32_t size, BufferUsage usage /*= BufferUsage::Dynamic */, const char* file,
                          uint32_t line )
    {
        return NamedAfter( Renderer::GetInstance().Rhi().CreateVertexBuffer( size, usage ), file, line );
    }

} // namespace Desert::Graphic
