#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <algorithm>

namespace Desert::Graphic::RDG
{
    namespace
    {
        // Desktop drivers place optimally tiled images on 64 KiB boundaries (the large-page size every
        // current discrete GPU uses for render targets); buffers are placed at 256 B, the largest
        // minStorageBufferOffsetAlignment / minUniformBufferOffsetAlignment in the field. The device's
        // own memory requirements replace these when the Vulkan executor allocates; the estimate exists
        // so the peak can be computed and budgeted without one.
        constexpr uint64_t kRdgTextureAlignment = 64ull * 1024ull;
        constexpr uint64_t kRdgBufferAlignment  = 256ull;

        constexpr uint64_t RdgAlignUp( uint64_t value, uint64_t alignment )
        {
            return ( value + alignment - 1 ) / alignment * alignment;
        }
    } // namespace

    MemoryFootprint EstimateFootprint( const TextureDesc& desc )
    {
        uint64_t bytes = 0;
        for ( uint32_t mip = 0; mip < desc.Mips; ++mip )
        {
            const uint32_t width  = std::max( 1u, desc.Size.Width >> mip );
            const uint32_t height = std::max( 1u, desc.Size.Height >> mip );
            // Only a volume shrinks in depth; a 2D array's layers are counted separately below.
            const uint32_t depth = desc.Dim == TextureDim::Tex3D ? std::max( 1u, desc.Size.Depth >> mip ) : 1u;
            bytes += Core::Formats::CalculateImageSize( width, height, depth, desc.Format );
        }
        bytes *= desc.Layers;
        return { RdgAlignUp( bytes, kRdgTextureAlignment ), kRdgTextureAlignment, MemoryClass::Texture };
    }

    MemoryFootprint EstimateFootprint( const BufferDesc& desc )
    {
        return { RdgAlignUp( desc.Bytes, kRdgBufferAlignment ), kRdgBufferAlignment, MemoryClass::Buffer };
    }
} // namespace Desert::Graphic::RDG
