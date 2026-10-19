#pragma once

#include <Engine/Graphic/FallbackTextures.hpp>

#include <vulkan/vulkan.hpp>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanFallbackTextures final : public FallbackTextures
    {
    public:
        VulkanFallbackTextures();

        [[nodiscard]] virtual Common::BoolResultStr Invalidate() override
        {
            return BOOLSUCCESS;
        }
        [[nodiscard]] virtual Common::BoolResultStr Release() override;

        virtual const std::shared_ptr<Image2D>&
        GetFallbackTexture2D( Core::Formats::ImageFormat format ) const override;
        virtual const std::shared_ptr<ImageCube>&
        GetFallbackTextureCube( Core::Formats::ImageFormat format ) const override;
        virtual const std::shared_ptr<ImageCube>&
        GetFallbackTextureCubeArray( Core::Formats::ImageFormat format ) const override;
        virtual const std::shared_ptr<Image2D>&
        GetFallbackStorageImage2D( Core::Formats::ImageFormat format ) const override;
        virtual const std::shared_ptr<Image3D>&
        GetFallbackTexture3D( Core::Formats::ImageFormat format ) const override;
        virtual const std::shared_ptr<Image3D>&
        GetFallbackStorageImage3D( Core::Formats::ImageFormat format ) const override;

    private:
        void CreateFallbackTexture2D( Core::Formats::ImageFormat format );
        void CreateFallbackTextureCube( Core::Formats::ImageFormat format, bool array );
        void CreateFallbackStorageImage2D( Core::Formats::ImageFormat format );
        void CreateFallbackTexture3D( Core::Formats::ImageFormat format );
        void CreateFallbackStorageImage3D( Core::Formats::ImageFormat format );

    private:
        std::unordered_map<Core::Formats::ImageFormat, std::shared_ptr<Image2D>>   m_FallbackTextures2D;
        std::unordered_map<Core::Formats::ImageFormat, std::shared_ptr<ImageCube>> m_FallbackTexturesCube;
        std::unordered_map<Core::Formats::ImageFormat, std::shared_ptr<ImageCube>> m_FallbackTexturesCubeArray;
        std::unordered_map<Core::Formats::ImageFormat, std::shared_ptr<Image2D>>   m_FallbackStorageImages2D;
        std::unordered_map<Core::Formats::ImageFormat, std::shared_ptr<Image3D>>   m_FallbackTextures3D;
        std::unordered_map<Core::Formats::ImageFormat, std::shared_ptr<Image3D>>   m_FallbackStorageImages3D;
    };
} // namespace Desert::Graphic::API::Vulkan