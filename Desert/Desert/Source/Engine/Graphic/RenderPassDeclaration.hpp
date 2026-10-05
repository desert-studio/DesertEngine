#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/RDG/RDGAccess.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    class Image;

    // WHAT ONE FRAME-GRAPH NODE TOUCHES, named as engine resources. The system that records the node fills it
    // where it registers the node (RenderGraphBuilder::PassConfig::Declare for a phase pass, the node list of an
    // atmosphere system for a compute dispatch, ExternalPassSpecification::Declare for an editor pass), and
    // SceneRenderer resolves every image to the frame graph's import of it (one registration per engine image, so
    // two nodes naming one image name one graph texture) and declares each entry on the node. The graph then
    // places every barrier and layout change between the node and its neighbours; the node's body records no
    // barrier.
    //
    // An image is named with the access its shaders make (SampledGraphics for a fragment-shader sample,
    // SampledCompute / StorageWrite for a compute dispatch). A buffer is named by the graph handle its owner
    // registered this frame (a buffer is imported by the system that owns it, e.g. ParticleRenderer's state).
    // A texture of THIS graph (a transient of FrameGraphRefs, a system texture, a ref an AddFrame* created) is
    // named by its ref with the access and the subresource range its shaders touch; it is declared on the node
    // as given, with no import.
    // The attachments a raster node renders into are NOT listed here: they are its target framebuffer, whole.
    class RenderPassDeclaration
    {
    public:
        struct ImageUse
        {
            std::shared_ptr<Image> Image;
            RDG::Access            Access = RDG::Access::None;
            bool                   Writes = false;
            std::string            Name; // the graph name of the import, when this entry registers it first
        };

        struct TextureUse
        {
            RDG::TextureRef       Texture;
            RDG::Access           Access = RDG::Access::None;
            RDG::SubresourceRange Range  = RDG::SubresourceRange::All();
            bool                  Writes = false;
        };

        struct BufferUse
        {
            RDG::BufferRef Buffer;
            RDG::Access    Access = RDG::Access::None;
            bool           Writes = false;
        };

        void Read( std::shared_ptr<Image> image, RDG::Access access, std::string name )
        {
            if ( image )
                m_Images.push_back( { std::move( image ), access, false, std::move( name ) } );
        }
        void Write( std::shared_ptr<Image> image, RDG::Access access, std::string name )
        {
            if ( image )
                m_Images.push_back( { std::move( image ), access, true, std::move( name ) } );
        }
        void Read( RDG::TextureRef texture, RDG::Access access, RDG::SubresourceRange range )
        {
            m_Textures.push_back( { texture, access, range, false } );
        }
        void Write( RDG::TextureRef texture, RDG::Access access, RDG::SubresourceRange range )
        {
            m_Textures.push_back( { texture, access, range, true } );
        }
        void Read( RDG::BufferRef buffer, RDG::Access access )
        {
            m_Buffers.push_back( { buffer, access, false } );
        }
        void Write( RDG::BufferRef buffer, RDG::Access access )
        {
            m_Buffers.push_back( { buffer, access, true } );
        }

        const std::vector<ImageUse>& Images() const
        {
            return m_Images;
        }
        const std::vector<TextureUse>& Textures() const
        {
            return m_Textures;
        }
        const std::vector<BufferUse>& Buffers() const
        {
            return m_Buffers;
        }

    private:
        std::vector<ImageUse>   m_Images;
        std::vector<TextureUse> m_Textures;
        std::vector<BufferUse>  m_Buffers;
    };

    // The body of a frame-graph node, whatever registered it: it records into the node's command buffer with
    // @p context (RDG::PassBindings over it, Renderer::DispatchCompute / DrawFullscreen / DrawProcedural /
    // DrawIndexed) and binds the graph textures it declared from @p refs. A failure is returned, never swallowed:
    // the graph logs it with the node's name.
    using NodeRecordFunc =
         std::function<Common::BoolResultStr( RDG::PassContext& context, const FrameGraphRefs& refs )>;

    // One compute node of a system's frame work: its name in the graph, what it declares, and the body that
    // records its dispatch. A system that dispatches several times in a frame hands one node per dispatch, in
    // order, so the graph orders and synchronises each against the next.
    struct ComputeNodeDeclaration
    {
        std::string           Name;
        RenderPassDeclaration Access;
        NodeRecordFunc        Record;
    };

    // The graph-ref half of a declaration (textures and buffers named by their handle in THIS graph), kept free of
    // the image imports (FrameTextures) so the device-free RenderGraphCompile suite runs the very code every node
    // kind declares through. What the first invalid ref is, for the refusal message, or nullptr when every ref is
    // a handle of this graph: a node naming a transient no earlier node produced is refused, never half-declared.
    inline const char* InvalidDeclaredRef( const RenderPassDeclaration& declared )
    {
        for ( const RenderPassDeclaration::TextureUse& use : declared.Textures() )
            if ( !use.Texture.IsValid() )
                return "a graph texture this frame did not produce";
        for ( const RenderPassDeclaration::BufferUse& use : declared.Buffers() )
            if ( !use.Buffer.IsValid() )
                return "a buffer the frame graph was not given";
        return nullptr;
    }

    // Every graph texture (with its subresource range) and buffer @p declared names, on @p pass. Call only after
    // InvalidDeclaredRef returned nullptr.
    inline void DeclareRefsOn( RDG::PassBuilder& pass, const RenderPassDeclaration& declared )
    {
        for ( const RenderPassDeclaration::TextureUse& use : declared.Textures() )
        {
            if ( use.Writes )
                pass.Write( use.Texture, use.Access, use.Range );
            else
                pass.Read( use.Texture, use.Access, use.Range );
        }
        for ( const RenderPassDeclaration::BufferUse& use : declared.Buffers() )
        {
            if ( use.Writes )
                pass.Write( use.Buffer, use.Access );
            else
                pass.Read( use.Buffer, use.Access );
        }
    }
} // namespace Desert::Graphic
