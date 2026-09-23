#pragma once

#include <Engine/Graphic/MipMapGenerator.hpp>

#include <memory>

namespace Desert::Graphic::API::Vulkan
{
    // Compute shader

    class VulkanMipMap2DGeneratorCS : public MipMap2DGenerator
    {
    public:
        virtual Common::BoolResultStr GenerateMips( const std::shared_ptr<Image2D>& image ) const override;
    };

    class VulkanMipMapCubeGeneratorCS : public MipMapCubeGenerator
    {
    public:
        virtual Common::BoolResultStr GenerateMips( const std::shared_ptr<ImageCube>& imageCube ) const override;
    };

    // Transfer ops

    class VulkanMipMap2DGeneratorTO : public MipMap2DGenerator
    {
    public:
        virtual Common::BoolResultStr GenerateMips( const std::shared_ptr<Image2D>& image ) const override;
    };

    class VulkanMipMapCubeGeneratorTO : public MipMapCubeGenerator
    {
    public:
        virtual Common::BoolResultStr GenerateMips( const std::shared_ptr<ImageCube>& imageCube ) const override;
    };
} // namespace Desert::Graphic::API::Vulkan