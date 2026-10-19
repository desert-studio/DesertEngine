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
        // The black cube viewed as a cube ARRAY of one cube (UE GBlackCubeArrayTexture): what a samplerCubeArray
        // is handed where its producer cannot run. A cube in an array binding is a view-type mismatch.
        virtual const std::shared_ptr<ImageCube>&
        GetFallbackTextureCubeArray( Core::Formats::ImageFormat format ) const = 0;
        virtual const std::shared_ptr<Image2D>&
        GetFallbackStorageImage2D( Core::Formats::ImageFormat format ) const = 0;
        // Volumes. A `sampler3D` / `image3D` binding that nobody wrote is an UNDEFINED descriptor, and the
        // 2D fallbacks cannot stand in for it: a 2D view in a 3D binding is exactly the silent mis-bind
        // the reflection work was done to prevent.
        virtual const std::shared_ptr<Image3D>&
        GetFallbackTexture3D( Core::Formats::ImageFormat format ) const = 0;
        virtual const std::shared_ptr<Image3D>&
        GetFallbackStorageImage3D( Core::Formats::ImageFormat format ) const = 0;

    public:
        FallbackTextures( const FallbackTextures& )            = delete;
        FallbackTextures& operator=( const FallbackTextures& ) = delete;

        static FallbackTextures& Get();

    protected:
        FallbackTextures() = default;
    };
} // namespace Desert::Graphic