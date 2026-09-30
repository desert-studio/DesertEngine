#pragma once

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/RenderPassDeclaration.hpp>
#include <Engine/Graphic/PostProcessing/LightShaftRules.hpp>

#include <format>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // The graph's names for the engine images this frame's nodes render into, copy and sample: each image is
    // imported once per graph (Renderer::ImportImage) in the layout its own record holds, and every node naming
    // it shares that one graph texture. The graph writes the layout it leaves the image in back into the record.
    class FrameTextures
    {
    public:
        explicit FrameTextures( RDG::Builder& graph ) : m_Graph( graph )
        {
        }

        std::vector<RDG::TextureRef>
        Refs( std::initializer_list<std::pair<std::shared_ptr<Image2D>, std::string_view>> images )
        {
            std::vector<RDG::TextureRef> refs;
            for ( const auto& [image, name] : images )
                if ( const RDG::TextureRef ref = Import( image, name ); ref.IsValid() )
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
                if ( const RDG::TextureRef ref = Import( framebuffer->GetColorAttachmentImage( i ),
                                                         std::format( "{}.Color{}", name, i ) );
                     ref.IsValid() )
                    refs.push_back( ref );
            return refs;
        }

        // An engine image a graph node renders into or copies: registered in the layout the image records, and
        // the graph writes its final layout back (Renderer::ImportImage), so the work recorded outside the graph
        // and the next frame find the layout the graph left. @p final, when given, is the state the graph
        // leaves the image in at its end. The first registration of an image wins, as for Refs.
        RDG::TextureRef Import( const std::shared_ptr<Image>& image, std::string_view name,
                                std::optional<RDG::Access> final = std::nullopt )
        {
            if ( !image )
                return {};
            if ( const auto it = m_Refs.find( image.get() ); it != m_Refs.end() )
                return it->second;
            RDG::ExternalTexture& external = *m_Storage.emplace_back( std::make_unique<RDG::ExternalTexture>() );
            RDG::TextureRef       ref;
            if ( const Common::BoolResultStr imported = Renderer::GetInstance().ImportImage( image, external );
                 imported )
            {
                ref = m_Graph.RegisterExternal( external, name );
                m_Externals.emplace( image.get(), &external );
                if ( final )
                    m_Graph.Extract( ref, external, *final );
            }
            else
                LOG_ERROR( "[SceneRenderer] the frame graph cannot import '{}': {}", name, imported.GetError() );
            m_Refs.emplace( image.get(), ref );
            return ref;
        }
        // The state the graph leaves @p image in at its end, for a reader outside this graph that samples it --
        // the scene's final image, which the editor viewport (ImGui) and the runtime blit sample after the
        // frame. The image must already be imported by a node of this graph (Import); an image no node
        // imported is not produced this frame, and that is the error returned.
        Common::BoolResultStr ExtractImported( const std::shared_ptr<Image>& image, std::string_view name,
                                               RDG::Access final )
        {
            const auto it = image ? m_Externals.find( image.get() ) : m_Externals.end();
            if ( it == m_Externals.end() )
                return Common::MakeError<bool>(
                     std::format( "'{}' is not imported by any node of this frame graph", name ) );
            m_Graph.Extract( m_Refs.at( image.get() ), *it->second, final );
            return BOOLSUCCESS;
        }

        // Import of every attachment of @p framebuffer (colour i as "<name>.Color<i>", depth as "<name>.Depth").
        RDG::ImportedFramebuffer ImportFramebuffer( const std::shared_ptr<Framebuffer>& framebuffer,
                                                    std::string_view                    name )
        {
            RDG::ImportedFramebuffer imported;
            if ( !framebuffer )
                return imported;
            for ( uint32_t i = 0; i < framebuffer->GetColorAttachmentCount(); ++i )
                imported.Colors.push_back(
                     Import( framebuffer->GetColorAttachmentImage( i ), std::format( "{}.Color{}", name, i ) ) );
            if ( framebuffer->GetDepthAttachmentCount() > 0 )
                imported.Depth =
                     Import( framebuffer->GetDepthAttachmentImage(), std::format( "{}.Depth", name ) );
            return imported;
        }

        // The depth attachment of @p framebuffer, imported with the layout its image records; Execute writes the
        // layout the graph leaves back into the image. An image that cannot be imported gets an invalid ref, and
        // the error is logged.
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
        RDG::Builder&                                      m_Graph;
        std::vector<std::unique_ptr<RDG::ExternalTexture>> m_Storage; // outlive Execute: the graph points at them
        std::map<const Image*, RDG::TextureRef>            m_Refs;
        std::map<const Image*, RDG::ExternalTexture*>      m_Externals; // Import's registrations, for Extract
    };

    // Shared by every raster node on an engine framebuffer (SceneRendererFrameMesh.cpp,
    // SceneRendererFrameDeferred.cpp). The whole of a framebuffer as graph names: every colour attachment by slot,
    // and the depth.
    using RasterTargets = RDG::ImportedFramebuffer;

    // A target the graph cannot declare whole is refused with its pass, never half-declared: a render pass
    // missing an attachment is not the one the pass's pipelines were built against. That covers a colour
    // image outside SHADER_READ_ONLY. A multisampled framebuffer is declared as its engine render pass is: the
    // multisampled colours and depth, plus the single-sample image each colour resolves into.
    inline std::optional<RasterTargets> TargetsOf( FrameTextures&                      textures,
                                                   const std::shared_ptr<Framebuffer>& framebuffer,
                                                   std::string_view name, std::string_view pass )
    {
        if ( !framebuffer )
            return std::nullopt;
        const uint32_t samples      = framebuffer->GetSpecification().Samples;
        const bool     multisampled = samples > 1;
        RasterTargets  targets;
        targets.Colors =
             multisampled ? textures.MultisampleColors( framebuffer, name ) : textures.Colors( framebuffer, name );
        targets.Depth = textures.Depth( framebuffer, name );
        if ( multisampled )
            targets.Resolves = textures.Colors( framebuffer, name );
        const uint32_t colours  = framebuffer->GetColorAttachmentCount();
        const bool     hasDepth = framebuffer->GetDepthAttachmentCount() != 0;
        if ( targets.Colors.size() != colours || hasDepth != targets.Depth.IsValid() ||
             targets.Resolves.size() != ( multisampled ? colours : 0u ) )
        {
            LOG_ERROR( "[SceneRenderer] '{}' is not recorded: the graph can declare {} of the {} colour "
                       "attachment(s) of '{}' ({} sample(s), {} resolve(s)), depth {}",
                       pass, targets.Colors.size(), colours, name, samples, targets.Resolves.size(),
                       targets.Depth.IsValid() ? "declared" : ( hasDepth ? "NOT declared" : "absent" ) );
            return std::nullopt;
        }
        return targets;
    }

    // What one node hands a later one inside the same frame graph (the nodes record at Execute).
    struct FrameValues
    {
        std::shared_ptr<Image2D> AoImage;
        std::shared_ptr<Image2D> GiImage;
        std::shared_ptr<Image2D> SceneCopy;
        SunScreen                Sun{ glm::vec2( 0.5f ), 0.0f };
    };

    // The graph textures of every image @p declared names, in order, each through the frame's one import of it
    // (FrameTextures::Import, so the node and every other node naming the image share one graph texture). An
    // image or a buffer the graph does not know refuses the node: false, the error logged naming @p node, and the
    // caller adds nothing. A node is never added half-declared.
    inline bool ResolveDeclared( FrameTextures& textures, const RenderPassDeclaration& declared,
                                 std::string_view node, std::vector<RDG::TextureRef>& images )
    {
        images.clear();
        for ( const RenderPassDeclaration::ImageUse& use : declared.Images() )
        {
            const std::string name =
                 use.Name.empty() ? std::format( "{}.Image{}", node, images.size() ) : use.Name;
            const RDG::TextureRef ref = textures.Import( use.Image, name );
            if ( !ref.IsValid() )
            {
                LOG_ERROR( "[SceneRenderer] '{}' is not recorded: the frame graph cannot import the image '{}' it "
                           "declares",
                           node, name );
                return false;
            }
            images.push_back( ref );
        }
        for ( const RenderPassDeclaration::BufferUse& use : declared.Buffers() )
            if ( !use.Buffer.IsValid() )
            {
                LOG_ERROR(
                     "[SceneRenderer] '{}' is not recorded: it declares a buffer the frame graph was not given",
                     node );
                return false;
            }
        return true;
    }

    // Every entry of @p declared on @p pass; @p images are ResolveDeclared's graph textures of its images.
    inline void DeclareOn( RDG::PassBuilder& pass, const std::vector<RDG::TextureRef>& images,
                           const RenderPassDeclaration& declared )
    {
        const std::vector<RenderPassDeclaration::ImageUse>& uses = declared.Images();
        for ( size_t i = 0; i < uses.size(); ++i )
        {
            if ( uses[i].Writes )
                pass.Write( images[i], uses[i].Access );
            else
                pass.Read( images[i], uses[i].Access );
        }
        for ( const RenderPassDeclaration::BufferUse& use : declared.Buffers() )
        {
            if ( use.Writes )
                pass.Write( use.Buffer, use.Access );
            else
                pass.Read( use.Buffer, use.Access );
        }
    }

    // A system's compute work of this frame, one Compute node per entry in the order given, each declaring exactly
    // what its entry names; the graph places every barrier between them and around them. All or nothing: when one
    // entry cannot be declared none is added (a later dispatch would read what the refused one did not write), and
    // the error names the node. NeverCull: the bodies also advance the system's own per-frame state (history
    // index, frame counters) the graph cannot see.
    // Returns whether every node was added (true for none): the system's Settle*Nodes applies the per-frame state
    // the nodes stand for only then.
    inline bool AddComputeNodes( RDG::Builder& graph, FrameTextures& textures,
                                 std::vector<ComputeNodeDeclaration> nodes )
    {
        std::vector<std::vector<RDG::TextureRef>> images( nodes.size() );
        for ( size_t i = 0; i < nodes.size(); ++i )
            if ( !ResolveDeclared( textures, nodes[i].Access, nodes[i].Name, images[i] ) )
                return false;
        for ( size_t i = 0; i < nodes.size(); ++i )
        {
            ComputeNodeDeclaration& node = nodes[i];
            graph.AddPass(
                 node.Name, RDG::PassFlags::Compute | RDG::PassFlags::NeverCull,
                 [&]( RDG::PassBuilder& pass ) { DeclareOn( pass, images[i], node.Access ); },
                 [record = std::move( node.Record )]( RDG::PassContext& ) -> Common::BoolResultStr
                 {
                     record();
                     return BOOLSUCCESS;
                 } );
        }
        return true;
    }
} // namespace Desert::Graphic
