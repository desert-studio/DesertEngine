#pragma once

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/PostProcessing/LightShaftRules.hpp>

#include <format>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // The graph's names for the engine images the legacy passes render into and sample, registered as
    // external textures on first use. An image is only named when it sits in SHADER_READ_ONLY, the layout
    // AddLegacyPass assumes before and after every legacy pass; one the old code keeps elsewhere (a depth
    // attachment, a multisampled colour target) would have the graph issue barriers from a layout the image
    // is not in, so it stays undeclared until its pass is ported (RDG4).
    class LegacyFrameTextures
    {
    public:
        explicit LegacyFrameTextures( RDG::Builder& graph ) : m_Graph( graph )
        {
        }

        std::vector<RDG::TextureRef>
        Refs( std::initializer_list<std::pair<std::shared_ptr<Image2D>, std::string_view>> images )
        {
            std::vector<RDG::TextureRef> refs;
            for ( const auto& [image, name] : images )
                if ( const RDG::TextureRef ref = Get( image, name ); ref.IsValid() )
                    refs.push_back( ref );
            return refs;
        }

        std::vector<RDG::TextureRef> Colors( const std::shared_ptr<Framebuffer>& framebuffer,
                                             std::string_view                    name )
        {
            std::vector<RDG::TextureRef> refs;
            if ( !framebuffer )
                return refs;
            for ( uint32_t i = 0; i < framebuffer->GetColorAttachmentCount(); ++i )
                if ( const RDG::TextureRef ref =
                          Get( framebuffer->GetColorAttachmentImage( i ), std::format( "{}.Color{}", name, i ) );
                     ref.IsValid() )
                    refs.push_back( ref );
            return refs;
        }

        // The depth attachment of @p framebuffer (a depth target never sits in SHADER_READ_ONLY), imported with
        // the layout its image records; Execute writes the layout the graph leaves back into the image. An
        // image that cannot be imported gets an invalid ref, and the error is logged.
        RDG::TextureRef Depth( const std::shared_ptr<Framebuffer>& framebuffer, std::string_view name )
        {
            if ( !framebuffer || framebuffer->GetDepthAttachmentCount() == 0 )
                return {};
            return Import( framebuffer->GetDepthAttachmentImage(), std::format( "{}.Depth", name ) );
        }

        // The multisampled colour images of @p framebuffer (Samples > 1), by slot, imported with the layout their
        // images record, as the depth is; Colors() of the same framebuffer are the images they resolve into.
        std::vector<RDG::TextureRef> MultisampleColors( const std::shared_ptr<Framebuffer>& framebuffer,
                                                        std::string_view                    name )
        {
            std::vector<RDG::TextureRef> refs;
            if ( !framebuffer || framebuffer->GetSpecification().Samples <= 1 )
                return refs;
            for ( uint32_t i = 0; i < framebuffer->GetColorAttachmentCount(); ++i )
                if ( const RDG::TextureRef ref = Import( framebuffer->GetMultisampleColorAttachmentImage( i ),
                                                         std::format( "{}.Color{}.MSAA", name, i ) );
                     ref.IsValid() )
                    refs.push_back( ref );
            return refs;
        }

    private:
        // An attachment the engine keeps outside SHADER_READ_ONLY (a depth, a multisampled colour), imported with
        // the layout its image records; Execute writes the layout the graph leaves back into the image. An image
        // that cannot be imported gets an invalid ref, and the error is logged.
        RDG::TextureRef Import( const std::shared_ptr<Image2D>& image, const std::string& name )
        {
            if ( !image )
                return {};
            if ( const auto it = m_Refs.find( image.get() ); it != m_Refs.end() )
                return it->second;
            RDG::TextureRef ref;
            auto&           external = m_Storage.emplace_back( std::make_unique<RDG::ExternalTexture>() );
            const Common::BoolResultStr imported = Renderer::GetInstance().ImportImage( image, *external );
            if ( imported )
                ref = m_Graph.RegisterExternal( *external, name );
            else
                LOG_ERROR( "[SceneRenderer] '{}' is not in the frame graph: {}", name, imported.GetError() );
            m_Refs.emplace( image.get(), ref );
            return ref;
        }

        RDG::TextureRef Get( const std::shared_ptr<Image2D>& image, std::string_view name )
        {
            if ( !image )
                return {};
            if ( const auto it = m_Refs.find( image.get() ); it != m_Refs.end() )
                return it->second;

            RDG::TextureRef                        ref;
            std::shared_ptr<RDG::IPhysicalTexture> physical = Renderer::GetInstance().WrapLegacyImage( *image );
            if ( physical )
            {
                const auto&      spec = image->GetImageSpecification();
                RDG::TextureDesc desc;
                desc.Size      = { image->GetWidth(), image->GetHeight(), 1 };
                desc.Format    = spec.Format;
                desc.Mips      = image->GetMipmapLevels();
                desc.Layers    = 1;
                desc.Samples   = std::max( 1u, spec.Samples );
                auto& external = m_Storage.emplace_back(
                     std::make_unique<RDG::ExternalTexture>( desc, RDG::Access::LegacyWrite ) );
                external->Physical = std::move( physical );
                ref                = m_Graph.RegisterExternal( *external, name );
            }
            m_Refs.emplace( image.get(), ref );
            return ref;
        }

        RDG::Builder&                                      m_Graph;
        std::vector<std::unique_ptr<RDG::ExternalTexture>> m_Storage; // outlive Execute: the graph points at them
        std::map<const Image2D*, RDG::TextureRef>          m_Refs;
    };

    // What one legacy pass hands a later one inside the same frame graph (the passes run at Execute).
    struct LegacyFrameValues
    {
        std::shared_ptr<Image2D> AoImage;
        std::shared_ptr<Image2D> GiImage;
        std::shared_ptr<Image2D> SceneCopy;
        SunScreen                Sun{ glm::vec2( 0.5f ), 0.0f };
    };

    inline void AddLegacy( RDG::Builder& graph, std::string_view name, const std::vector<RDG::TextureRef>& reads,
                           const std::vector<RDG::TextureRef>& writes, std::function<void()> body )
    {
        graph.AddLegacyPass( name, reads, writes,
                             [body = std::move( body )]( RDG::PassContext& ) -> Common::BoolResultStr
                             {
                                 body();
                                 return BOOLSUCCESS;
                             } );
    }
} // namespace Desert::Graphic
