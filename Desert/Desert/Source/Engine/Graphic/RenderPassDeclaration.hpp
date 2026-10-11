#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/RDG/RDGAccess.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGResources.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Graphic
{
    class Image;

    // WHAT ONE FRAME-GRAPH NODE TOUCHES, named as engine resources. The system that records the node fills it
    // where it registers the node (SystemRasterPass::Declare for a phase pass, the node list of an
    // atmosphere system for a compute dispatch, ExtensionPass::Declare for an editor pass), and
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

        // RDG-FAULT1. One parameter block of a draw / dispatch the node records (UE: the pass parameter struct),
        // declared against the shader it records with: ShaderBindingLayout from Renderer::GetBindingLayout, the
        // other route from MaterialExecutor::GetRouteFill / Renderer::GetPipelineRouteFill. Each entry IS the
        // declaration of its access (no separate Read for it). The graph validates the block before anything is
        // recorded (RDG::ValidatePassBindings): a broken block faults the node, the frame goes on without it.
        // The exec opens it with RDG::PassBindings( context, context.GetBindingBlock( <index> ) ), where <index>
        // is the value Bindings returned (blocks are numbered in declaration order).
        // A texture entry names either a ref of THIS graph (Texture) or an engine image its system owns across
        // frames (Image, e.g. an atmosphere LUT or the cloud trace): SceneRenderer resolves the image to the
        // frame's one import of it (FrameTextures::Import, ImportName naming the import) exactly as an ImageUse,
        // so the graph orders, barriers and fault-isolates it like any other entry and the pass binds the image
        // the graph resolved, in the layout the graph's barrier left it in. Nothing binds it outside the block.
        struct BlockEntry
        {
            std::string                     ShaderName;
            RDG::ShaderResourceKind         Kind = RDG::ShaderResourceKind::SampledTexture;
            RDG::TextureRef                 Texture;
            std::shared_ptr<Image>          Image;
            std::string                     ImportName;
            RDG::BufferRef                  Buffer;
            RDG::Access                     Access = RDG::Access::None;
            RDG::SubresourceRange           Range  = RDG::SubresourceRange::All();
            std::optional<RDG::SamplerDesc> Sampler;
        };
        struct BlockUse
        {
            std::shared_ptr<const RDG::ShaderBindingLayout> Layout;
            RDG::OtherRouteFill                             Other;
            std::vector<BlockEntry>                         Entries;
            uint32_t                                        PushConstantBytes = 0;
        };
        // Fills the block Bindings opened; mirrors RDG::BindingBlockBuilder. Holds the declaration and the index,
        // not the block, so a later Bindings call (which may grow the list) does not invalidate it.
        class BlockDeclaration
        {
        public:
            BlockDeclaration( RenderPassDeclaration& declaration, uint32_t index )
                 : m_Declaration( declaration ), m_Index( index )
            {
            }
            BlockDeclaration& Sampled( std::string_view shaderName, RDG::TextureRef texture, RDG::Access access,
                                       RDG::SubresourceRange range, RDG::SamplerDesc sampler )
            {
                Block().Entries.push_back( { std::string( shaderName ),
                                             RDG::ShaderResourceKind::SampledTexture,
                                             texture,
                                             {},
                                             {},
                                             {},
                                             access,
                                             range,
                                             sampler } );
                return *this;
            }
            // An engine image its system owns, sampled whole (resolved to its import, see BlockEntry).
            BlockDeclaration& Sampled( std::string_view shaderName, std::shared_ptr<Image> image,
                                       RDG::Access access, RDG::SamplerDesc sampler, std::string importName )
            {
                Block().Entries.push_back( { std::string( shaderName ),
                                             RDG::ShaderResourceKind::SampledTexture,
                                             {},
                                             std::move( image ),
                                             std::move( importName ),
                                             {},
                                             access,
                                             RDG::SubresourceRange::All(),
                                             sampler } );
                return *this;
            }
            BlockDeclaration& Storage( std::string_view shaderName, RDG::TextureRef texture, RDG::Access access,
                                       uint32_t mip = 0 )
            {
                Block().Entries.push_back( { std::string( shaderName ),
                                             RDG::ShaderResourceKind::StorageTexture,
                                             texture,
                                             {},
                                             {},
                                             {},
                                             access,
                                             RDG::SubresourceRange::Mip( mip ),
                                             std::nullopt } );
                return *this;
            }
            // An engine image its system owns, as a storage image on @p mip: StorageWrite for a dispatch that
            // writes it, StorageRead for one that only reads it (resolved to its import, see BlockEntry).
            BlockDeclaration& Storage( std::string_view shaderName, std::shared_ptr<Image> image,
                                       RDG::Access access, std::string importName, uint32_t mip = 0 )
            {
                Block().Entries.push_back( { std::string( shaderName ),
                                             RDG::ShaderResourceKind::StorageTexture,
                                             {},
                                             std::move( image ),
                                             std::move( importName ),
                                             {},
                                             access,
                                             RDG::SubresourceRange::Mip( mip ),
                                             std::nullopt } );
                return *this;
            }
            BlockDeclaration& Uniform( std::string_view shaderName, RDG::BufferRef buffer )
            {
                Block().Entries.push_back( { std::string( shaderName ),
                                             RDG::ShaderResourceKind::UniformBuffer,
                                             {},
                                             {},
                                             {},
                                             buffer,
                                             RDG::Access::UniformRead,
                                             RDG::SubresourceRange::All(),
                                             std::nullopt } );
                return *this;
            }
            BlockDeclaration& Storage( std::string_view shaderName, RDG::BufferRef buffer, RDG::Access access )
            {
                Block().Entries.push_back( { std::string( shaderName ),
                                             RDG::ShaderResourceKind::StorageBuffer,
                                             {},
                                             {},
                                             {},
                                             buffer,
                                             access,
                                             RDG::SubresourceRange::All(),
                                             std::nullopt } );
                return *this;
            }
            BlockDeclaration& PushConstantBytes( uint32_t bytes )
            {
                Block().PushConstantBytes = bytes;
                return *this;
            }
            // The index the exec passes to PassContext::GetBindingBlock.
            [[nodiscard]] uint32_t GetIndex() const
            {
                return m_Index;
            }

        private:
            BlockUse& Block()
            {
                return m_Declaration.m_Blocks[m_Index];
            }

            RenderPassDeclaration& m_Declaration;
            uint32_t               m_Index;
        };
        // The kept layout (ShaderBindingLayoutCache): shared, not copied per frame.
        BlockDeclaration Bindings( const std::shared_ptr<const RDG::ShaderBindingLayout>& layout,
                                   RDG::OtherRouteFill                                    other )
        {
            m_Blocks.push_back( { layout, std::move( other ), {}, 0 } );
            return { *this, static_cast<uint32_t>( m_Blocks.size() - 1 ) };
        }
        // A layout made for this declaration only: the declaration takes it over.
        BlockDeclaration Bindings( RDG::ShaderBindingLayout layout, RDG::OtherRouteFill other )
        {
            m_Blocks.push_back( { std::make_shared<const RDG::ShaderBindingLayout>( std::move( layout ) ),
                                  std::move( other ),
                                  {},
                                  0 } );
            return { *this, static_cast<uint32_t>( m_Blocks.size() - 1 ) };
        }

        [[nodiscard]] const std::vector<BlockUse>& Blocks() const
        {
            return m_Blocks;
        }

        [[nodiscard]] const std::vector<ImageUse>& Images() const
        {
            return m_Images;
        }
        [[nodiscard]] const std::vector<TextureUse>& Textures() const
        {
            return m_Textures;
        }
        [[nodiscard]] const std::vector<BufferUse>& Buffers() const
        {
            return m_Buffers;
        }

    private:
        std::vector<ImageUse>   m_Images;
        std::vector<TextureUse> m_Textures;
        std::vector<BufferUse>  m_Buffers;
        std::vector<BlockUse>   m_Blocks;
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
        for ( const RenderPassDeclaration::BlockUse& block : declared.Blocks() )
        {
            for ( const RenderPassDeclaration::BlockEntry& entry : block.Entries )
            {
                const bool texture = entry.Kind == RDG::ShaderResourceKind::SampledTexture ||
                                     entry.Kind == RDG::ShaderResourceKind::StorageTexture;
                // An engine image is not a ref yet: its import is checked where it is resolved (ResolveDeclared).
                if ( texture && entry.Image )
                    continue;
                if ( texture ? !entry.Texture.IsValid() : !entry.Buffer.IsValid() )
                    return texture ? "a bound graph texture this frame did not produce"
                                   : "a bound buffer the frame graph was not given";
            }
        }
        return nullptr;
    }

    // The engine images @p declared's binding blocks name (BlockEntry::Image), in block and entry order: the
    // order ResolveDeclared imports them in and DeclareRefsOn consumes their refs in.
    inline std::vector<const RenderPassDeclaration::BlockEntry*>
    BlockImageEntries( const RenderPassDeclaration& declared )
    {
        std::vector<const RenderPassDeclaration::BlockEntry*> entries;
        for ( const RenderPassDeclaration::BlockUse& block : declared.Blocks() )
            for ( const RenderPassDeclaration::BlockEntry& entry : block.Entries )
                if ( entry.Image )
                    entries.push_back( &entry );
        return entries;
    }

    // Every graph texture (with its subresource range) and buffer @p declared names, on @p pass. Call only after
    // InvalidDeclaredRef returned nullptr. @p blockImages are the graph textures of BlockImageEntries( declared ),
    // in that order (ResolveDeclared's imports); an entry naming an engine image is declared on its ref.
    inline void DeclareRefsOn( RDG::PassBuilder& pass, const RenderPassDeclaration& declared,
                               std::span<const RDG::TextureRef> blockImages = {} )
    {
        size_t nextImage = 0;
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
        // In declaration order, so block i of the declaration is block i of the pass
        // (PassContext::GetBindingBlock).
        for ( const RenderPassDeclaration::BlockUse& block : declared.Blocks() )
        {
            RDG::BindingBlockBuilder bindings = pass.Bindings( block.Layout, block.Other );
            for ( const RenderPassDeclaration::BlockEntry& entry : block.Entries )
            {
                // A block image without a resolved ref is declared on an invalid ref, which the graph refuses by
                // name -- never silently skipped.
                RDG::TextureRef texture = entry.Texture;
                if ( entry.Image )
                    texture = nextImage < blockImages.size() ? blockImages[nextImage++] : RDG::TextureRef{};
                switch ( entry.Kind )
                {
                    case RDG::ShaderResourceKind::SampledTexture:
                        bindings.Sampled( entry.ShaderName, texture, entry.Access, entry.Range,
                                          entry.Sampler.value_or( RDG::SamplerDesc::LinearClamp() ) );
                        break;
                    case RDG::ShaderResourceKind::StorageTexture:
                        bindings.Storage( entry.ShaderName, texture, entry.Access, entry.Range.BaseMip );
                        break;
                    case RDG::ShaderResourceKind::UniformBuffer:
                        bindings.Uniform( entry.ShaderName, entry.Buffer );
                        break;
                    case RDG::ShaderResourceKind::StorageBuffer:
                        bindings.Storage( entry.ShaderName, entry.Buffer, entry.Access );
                        break;
                }
            }
            bindings.PushConstantBytes( block.PushConstantBytes );
        }
    }
} // namespace Desert::Graphic
