#pragma once

#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/DynamicResources.hpp>

namespace Desert::Graphic
{
    class FallbackTextures : public DynamicResources
    {
    public:
        virtual const std::shared_ptr<Image2D>&
        GetFallbackTexture2D( Core::Formats::ImageFormat format ) const = 0;
        virtual const std::shared_ptr<ImageCube>&
        GetFallbackTextureCube( Core::Formats::ImageFormat format ) const = 0;
        virtual const std::shared_ptr<Image2D>&
        GetFallbackStorageImage2D( Core::Formats::ImageFormat format ) const = 0;
        // Volumes. A `sampler3D` / `image3D` binding that nobody wrote is an UNDEFINED descriptor, and the
        // 2D fallbacks cannot stand in for it: a 2D view in a 3D binding is exactly the silent mis-bind
        // the reflection work was done to prevent.
        virtual const std::shared_ptr<Image3D>&
        GetFallbackTexture3D( Core::Formats::ImageFormat format ) const = 0;
        virtual const std::shared_ptr<Image3D>&
        GetFallbackStorageImage3D( Core::Formats::ImageFormat format ) const = 0;
        // The ZERO volume (UE GSystemTextures.VolumetricBlackDummy): 1x1x1 RGBA8, all four channels 0 - what a
        // volume of light reads where its producer did not run this frame (RDG System.Black3D). The 3D fallbacks
        // above are white ("no attenuation"), which in a volume of light would be light from nowhere.
        virtual const std::shared_ptr<Image3D>& GetBlackTexture3D() const = 0;

    public:
        FallbackTextures( const FallbackTextures& )            = delete;
        FallbackTextures& operator=( const FallbackTextures& ) = delete;

        static FallbackTextures& Get();

    protected:
        FallbackTextures() = default;
    };
} // namespace Desert::Graphic