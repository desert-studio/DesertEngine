#pragma once

#include <Common/Core/ResultStr.hpp>

#include <memory>

namespace Desert::Graphic
{
    class Image;

    namespace RDG
    {
        struct ExternalTexture;
    }

    /// How an engine image becomes a render-graph external: the narrow seam between code that owns images across
    /// frames (a view's temporal history) and the backend that knows what a physical image is. The device
    /// implementation is Renderer::ImportImage (RendererAPI::ImportImage: the image's graph desc, its physical
    /// image, the subresource states its own layout record implies, and the RecordStates hook that writes the
    /// final layout back). A test passes a mock that fills ExternalTexture::Physical with a fake, so the owner's
    /// allocation rules run without a device — the same split as IImageFactory (Graphic/ImageFactory.hpp).
    class IGraphImageImporter
    {
    public:
        virtual ~IGraphImageImporter() = default;

        /// Fills @p into from @p image, or returns the backend's error (an image with no memory, a layout the
        /// graph has no name for); @p into is unspecified after an error.
        [[nodiscard]] virtual Common::BoolResultStr ImportImage( const std::shared_ptr<Image>& image,
                                                                 RDG::ExternalTexture&         into ) const = 0;
    };

    /// The device: the active RendererAPI's ImportImage.
    class RendererGraphImageImporter final : public IGraphImageImporter
    {
    public:
        [[nodiscard]] Common::BoolResultStr ImportImage( const std::shared_ptr<Image>& image,
                                                         RDG::ExternalTexture&         into ) const override;
    };
} // namespace Desert::Graphic
