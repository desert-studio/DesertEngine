#include <Engine/Graphic/IndexBuffer.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::Graphic
{
    std::shared_ptr<IndexBuffer> IndexBuffer::Create( const void* data, uint32_t size, BufferUsage usage )
    {
        return Renderer::GetInstance().Rhi().CreateIndexBuffer( data, size, usage );
    }

    std::shared_ptr<IndexBuffer> IndexBuffer::Create( uint32_t size, BufferUsage usage )
    {
        return Renderer::GetInstance().Rhi().CreateIndexBuffer( size, usage );
    }
} // namespace Desert::Graphic
