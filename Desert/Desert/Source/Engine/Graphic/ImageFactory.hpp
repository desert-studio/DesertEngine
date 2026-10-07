#pragma once

#include <Engine/Graphic/Image.hpp>

#include <memory>

namespace Desert::Graphic
{
    /// Where a texture's GPU image is made. The graphics layer's one narrow seam between "a texture owns an
    /// image" and "a device makes images": Texture2D is handed one of these instead of calling the device
    /// factory itself, so its ownership rules (register on create, release on drop, its own slot only) can
    /// be exercised against a mock that counts images without a device, and the engine passes the device.
    class IImageFactory
    {
    public:
        virtual ~IImageFactory() = default;

        /// The image described by @p spec, or nullptr when it could not be made; a device implementation
        /// logs the reason with the size before answering null (Image2D::Create does).
        NO_DISCARD virtual std::shared_ptr<Image2D>
        CreateImage2D( const Core::Formats::Image2DSpecification& spec ) const = 0;
    };

    /// The device: the active RendererAPI's backend image, i.e. Image2D::Create.
    class DeviceImageFactory final : public IImageFactory
    {
    public:
        NO_DISCARD std::shared_ptr<Image2D>
                   CreateImage2D( const Core::Formats::Image2DSpecification& spec ) const override
        {
            return Image2D::Create( spec );
        }
    };
} // namespace Desert::Graphic
