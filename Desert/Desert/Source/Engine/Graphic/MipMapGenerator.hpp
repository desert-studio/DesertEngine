#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Graphic
{
    class Image2D;
    class ImageCube;

    enum class MipGenStrategy : uint8_t
    {
        ComputeShader,
        TransferOps
    };

    // THE VIRTUAL DESTRUCTORS ARE LOAD-BEARING, not boilerplate. Create() below hands back a
    // `std::unique_ptr<Base>` that actually owns a VulkanMipMap*GeneratorTO, so ~unique_ptr does
    // `delete` through the base. With a non-virtual destructor that is undefined behaviour: the
    // derived destructor is never entered, and the deallocation is performed against the base's size
    // rather than the object's. Both types are stateless today, which is the only reason this has
    // been survivable — the first member either of them acquires would leak on every mip generation.
    class MipMap2DGenerator
    {
    public:
        virtual ~MipMap2DGenerator() = default;

        virtual Common::BoolResultStr GenerateMips( const std::shared_ptr<Image2D>& image ) const = 0;

        static std::unique_ptr<MipMap2DGenerator> Create( MipGenStrategy strategy );
    };

    class MipMapCubeGenerator
    {
    public:
        virtual ~MipMapCubeGenerator() = default;

        virtual Common::BoolResultStr GenerateMips( const std::shared_ptr<ImageCube>& image ) const = 0;

        static std::unique_ptr<MipMapCubeGenerator> Create( MipGenStrategy strategy );
    };
} // namespace Desert::Graphic