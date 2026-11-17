#pragma once

#include <Engine/Graphic/MipMapGenerator.hpp>

#include <memory>

namespace Desert::Graphic::API::Vulkan
{
    // Compute shader

    class VulkanMipMapCubeGeneratorCS : public MipMapCubeGenerator
    {
    public:
        virtual Common::BoolResultStr GenerateMips( const std::shared_ptr<ImageCube>& imageCube ) const override;
    };

    // Transfer ops

    class VulkanMipMapCubeGeneratorTO : public MipMapCubeGenerator
    {
    public:
        virtual Common::BoolResultStr GenerateMips( const std::shared_ptr<ImageCube>& imageCube ) const override;
    };
} // namespace Desert::Graphic::API::Vulkan