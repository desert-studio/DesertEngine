#include <Engine/Graphic/GraphImageImporter.hpp>

#include <Engine/Graphic/Renderer.hpp>

namespace Desert::Graphic
{
    Common::BoolResultStr RendererGraphImageImporter::ImportImage( const std::shared_ptr<Image>& image,
                                                                   RDG::ExternalTexture&         into ) const
    {
        return Renderer::ImportImage( image, into );
    }
} // namespace Desert::Graphic
