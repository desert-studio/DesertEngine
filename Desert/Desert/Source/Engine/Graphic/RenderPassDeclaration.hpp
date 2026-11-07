#pragma once

#include <Common/Core/Memory/FrameArena.hpp>
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
        //
        // CPU-RDG-ALLOC (UE: FRDGBuilder parameter structs - a binding names its parameter, the name lives in the
        // reflection, nothing formats it per frame): an entry keeps its parameter's own name view and element, no
        // formatted "<Name>[<Element>]" string; the graph forms the slot name once, in its arena
        // (PassBuilder::Bindings). A parameter's Name views a struct member or a named engine constant
        // (RDGShaderParameters.hpp), which outlives every declaration.
        struct BlockEntry
        {
            std::string_view                ParameterName;
            uint32_t                        Element = RDG::TextureParameter::kWhole;
            RDG::ShaderResourceKind         Kind    = RDG::ShaderResourceKind::Texture;
            RDG::TextureRef                 Texture;
            std::shared_ptr<Image>          Image;
            // The import's graph name: a static name (a literal) is viewed, a formed one is kept here.
            const char*                     StaticImportName = nullptr;
            std::string                     FormedImportName;
            RDG::BufferRef                  Buffer;
            RDG::Access                     Access = RDG::Access::None;
            RDG::SubresourceRange           Range  = RDG::SubresourceRange::All();
            std::optional<RDG::SamplerDesc> Sampler;

            // The graph name of the import (empty: ResolveDeclared names it after the node and the slot).
            [[nodiscard]] std::string_view ImportName() const noexcept
            {
                if ( StaticImportName != nullptr )
                    return StaticImportName;
                return FormedImportName;
            }
        };
        // How an engine image's import is named: a literal (or another static name) is viewed with no copy; a name
        // formed for this frame (std::format) is moved into the entry.
        struct ImportNameArg
        {
            ImportNameArg( const char* staticName ) noexcept : Static( staticName ) // NOLINT: implicit by design
            {
            }
            ImportNameArg( std::string formedName ) noexcept : Formed( std::move( formedName ) ) // NOLINT
            {
            }

            const char* Static = nullptr;
            std::string Formed;
        };
        // The entries of one block. With an arena (RenderPassDeclaration( FrameArena& )), they live in it - the
        // frame graph's arena for a node SceneRenderer declares, no heap per frame; without, the heap.
        using BlockEntries = Common::Memory::ArenaVector<BlockEntry>;
        struct BlockUse
        {
            std::shared_ptr<const RDG::ShaderBindingLayout> Layout;
            std::shared_ptr<const RDG::OtherRouteFill>      Other = RDG::NoOtherRouteFill();
            BlockEntries                                    Entries;
            uint32_t                                        PushConstantBytes = 0;
        };

        RenderPassDeclaration() = default;
        // Every list of the declaration (images, graph textures, buffers, prerequisites, the binding blocks and
        // their entries) in @p arena, which outlives the declaration (the frame graph's,
        // RDG::Builder::GetFrameArena, reset only with the graph): a node declared this way costs no heap list.
        explicit RenderPassDeclaration( Common::Memory::FrameArena& arena ) noexcept
             : m_Arena( &arena ), m_Images( Common::Memory::ArenaAllocator<ImageUse>( arena ) ),
               m_Textures( Common::Memory::ArenaAllocator<TextureUse>( arena ) ),
               m_Buffers( Common::Memory::ArenaAllocator<BufferUse>( arena ) ),
               m_Blocks( Common::Memory::ArenaAllocator<BlockUse>( arena ) ),
               m_Prerequisites( Common::Memory::ArenaAllocator<const RDG::IRecordPrerequisite*>( arena ) )
        {
        }

        // Fills the block Bindings opened; mirrors RDG::BindingBlockBuilder. Holds the declaration and the index,
        // not the block, so a later Bindings call (which may grow the list) does not invalidate it.
        class BlockDeclaration
        {
        public:
            BlockDeclaration( RenderPassDeclaration& declaration, uint32_t index )
                 : m_Declaration( declaration ), m_Index( index )
            {
            }
            // A separate texture slot (RDG::ShaderResourceKind::Texture), read through a Sampler slot of the
            // block.
            BlockDeclaration& Texture( RDG::TextureParameter parameter, RDG::TextureRef texture,
                                       RDG::Access access, RDG::SubresourceRange range )
            {
                Add( parameter, access, range ).Texture = texture;
                return *this;
            }
            // A separate sampler slot (RDG::ShaderResourceKind::Sampler): a state, no resource, shared by every
            // Texture entry read through it - named again with the same state it adds nothing (see
            // RDG::BindingBlockBuilder::Sampler).
            BlockDeclaration& Sampler( RDG::SamplerParameter parameter, RDG::SamplerDesc sampler )
            {
                for ( const BlockEntry& e : Block().Entries )
                    if ( e.Kind == RDG::ShaderResourceKind::Sampler && e.ParameterName == parameter.Name &&
                         e.Element == parameter.Element && e.Sampler == sampler )
                        return *this;
                Add( parameter, RDG::Access::None, RDG::SubresourceRange::All() ).Sampler = sampler;
                return *this;
            }
            // An engine image its system owns as a separate texture, read whole (resolved to its import, see
            // BlockEntry) through a Sampler slot of the block.
            BlockDeclaration& Texture( RDG::TextureParameter parameter, std::shared_ptr<Image> image,
                                       RDG::Access access, ImportNameArg importName )
            {
                AddImage( Add( parameter, access, RDG::SubresourceRange::All() ), std::move( image ),
                          std::move( importName ) );
                return *this;
            }
            BlockDeclaration& Storage( RDG::StorageTextureParameter parameter, RDG::TextureRef texture,
                                       RDG::Access access, uint32_t mip = 0 )
            {
                Add( parameter, access, RDG::SubresourceRange::Mip( mip ) ).Texture = texture;
                return *this;
            }
            // An engine image its system owns, as a storage image on @p mip: StorageWrite for a dispatch that
            // writes it, StorageRead for one that only reads it (resolved to its import, see BlockEntry).
            BlockDeclaration& Storage( RDG::StorageTextureParameter parameter, std::shared_ptr<Image> image,
                                       RDG::Access access, ImportNameArg importName, uint32_t mip = 0 )
            {
                AddImage( Add( parameter, access, RDG::SubresourceRange::Mip( mip ) ), std::move( image ),
                          std::move( importName ) );
                return *this;
            }
            BlockDeclaration& Uniform( RDG::UniformBufferParameter parameter, RDG::BufferRef buffer )
            {
                Add( parameter, RDG::Access::UniformRead, RDG::SubresourceRange::All() ).Buffer = buffer;
                return *this;
            }
            BlockDeclaration& Storage( RDG::StorageBufferParameter parameter, RDG::BufferRef buffer,
                                       RDG::Access access )
            {
                Add( parameter, access, RDG::SubresourceRange::All() ).Buffer = buffer;
                return *this;
            }
            // The view's TLAS (RayTracingSceneFrame::Tlas), ray-queried by the pass's shader.
            BlockDeclaration& AccelerationStructure( RDG::AccelerationStructureParameter parameter,
                                                     RDG::BufferRef                      tlas )
            {
                Add( parameter, RDG::Access::AccelStructRead, RDG::SubresourceRange::All() ).Buffer = tlas;
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
            // One entry of the parameter's kind; the caller sets the resource (or the sampler state).
            template <RDG::ShaderResourceKind K>
            BlockEntry& Add( RDG::ShaderParameterOf<K> parameter, RDG::Access access, RDG::SubresourceRange range )
            {
                BlockEntry& entry   = Block().Entries.emplace_back();
                entry.ParameterName = parameter.Name;
                entry.Element       = parameter.Element;
                entry.Kind          = K;
                entry.Access        = access;
                entry.Range         = range;
                return entry;
            }
            static void AddImage( BlockEntry& entry, std::shared_ptr<Image> image, ImportNameArg importName )
            {
                entry.Image            = std::move( image );
                entry.StaticImportName = importName.Static;
                entry.FormedImportName = std::move( importName.Formed );
            }

            RenderPassDeclaration& m_Declaration;
            uint32_t               m_Index;
        };
        // The kept layout (ShaderBindingLayoutCache): shared, not copied per frame.
        BlockDeclaration Bindings( const std::shared_ptr<const RDG::ShaderBindingLayout>& layout,
                                   std::shared_ptr<const RDG::OtherRouteFill>             other )
        {
            m_Blocks.push_back( { layout, std::move( other ), BlockEntries( EntryAllocator() ), 0 } );
            return { *this, static_cast<uint32_t>( m_Blocks.size() - 1 ) };
        }
        // A layout made for this declaration only: the declaration takes it over.
        BlockDeclaration Bindings( RDG::ShaderBindingLayout layout, std::shared_ptr<const RDG::OtherRouteFill> other )
        {
            m_Blocks.push_back( { std::make_shared<const RDG::ShaderBindingLayout>( std::move( layout ) ),
                                  std::move( other ), BlockEntries( EntryAllocator() ), 0 } );
            return { *this, static_cast<uint32_t>( m_Blocks.size() - 1 ) };
        }

        // RENDER-THREADS M1 (RDG::PassBuilder::Prepares): a material (or another prerequisite) the node's exec
        // applies, made ready before the graph records; the exec of a node that declares any records sealed and
        // creates no per-view object. The object outlives the graph's Execute.
        void Prepares( const RDG::IRecordPrerequisite& prerequisite )
        {
            m_Prerequisites.push_back( &prerequisite );
        }
        [[nodiscard]] std::span<const RDG::IRecordPrerequisite* const> Prerequisites() const
        {
            return m_Prerequisites;
        }

        [[nodiscard]] std::span<const BlockUse> Blocks() const
        {
            return m_Blocks;
        }

        [[nodiscard]] std::span<const ImageUse> Images() const
        {
            return m_Images;
        }
        [[nodiscard]] std::span<const TextureUse> Textures() const
        {
            return m_Textures;
        }
        [[nodiscard]] std::span<const BufferUse> Buffers() const
        {
            return m_Buffers;
        }

        // THE NODE'S RENDER AREA in pixels: the extent of the attachments the graph binds for this raster node -
        // its target framebuffer's, or, after the temporal resolve, the OUTPUT-extent overlay set the graph puts
        // in place of the scene target (SceneRenderer::AddPassNode sets it before it calls Declare). A pass that
        // lays out in pixels (the UI canvas, the debug strings) lays out at THIS extent, never at its
        // framebuffer's: the scene target is the RENDER extent, smaller than the image below 100 % screen
        // percentage (UE: Slate and the debug canvas draw at the view's output size, after the upscale). (0, 0):
        // not a raster node.
        void SetTargetExtent( const glm::uvec2 extent )
        {
            m_TargetExtent = extent;
        }
        [[nodiscard]] glm::uvec2 TargetExtent() const
        {
            return m_TargetExtent;
        }

    private:
        [[nodiscard]] Common::Memory::ArenaAllocator<BlockEntry> EntryAllocator() const noexcept
        {
            if ( m_Arena != nullptr )
                return Common::Memory::ArenaAllocator<BlockEntry>( *m_Arena );
            return {};
        }

        glm::uvec2                                                   m_TargetExtent{ 0u, 0u };
        Common::Memory::FrameArena*                                  m_Arena = nullptr; // null: the heap
        Common::Memory::ArenaVector<ImageUse>                        m_Images;
        Common::Memory::ArenaVector<TextureUse>                      m_Textures;
        Common::Memory::ArenaVector<BufferUse>                       m_Buffers;
        Common::Memory::ArenaVector<BlockUse>                        m_Blocks;
        Common::Memory::ArenaVector<const RDG::IRecordPrerequisite*> m_Prerequisites;
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
        ComputeNodeDeclaration() = default;
        // CPU-RDG-ALLOC: the node's declaration lists in the frame graph's arena (RDG::Builder::GetFrameArena) -
        // for a system that declares its nodes while it holds the graph; the node is added before the graph
        // resets.
        explicit ComputeNodeDeclaration( Common::Memory::FrameArena& arena ) noexcept : Access( arena )
        {
        }

        std::string           Name;
        RenderPassDeclaration Access;
        NodeRecordFunc        Record;
        // The frame asks for the async compute queue (RDG::PassFlags::AsyncCompute): set by the frame code that
        // adds the nodes, which knows how far the first reader is, never by the system that declares them.
        bool AsyncCompute = false;
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
                // A sampler slot names no resource.
                if ( entry.Kind == RDG::ShaderResourceKind::Sampler )
                    continue;
                const bool texture = entry.Kind == RDG::ShaderResourceKind::Texture ||
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
        for ( const RDG::IRecordPrerequisite* prerequisite : declared.Prerequisites() )
            pass.Prepares( *prerequisite );
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
            // Each entry replays the parameter its declaration named (BlockEntry::ParameterName / Element), so the
            // replay re-forms the parameter of the entry's kind; the graph forms its slot name once, in its arena.
            RDG::BindingBlockBuilder bindings = pass.Bindings( block.Layout, block.Other );
            for ( const RenderPassDeclaration::BlockEntry& entry : block.Entries )
            {
                // A block image without a resolved ref is declared on an invalid ref, which the graph refuses by
                // name -- never silently skipped.
                RDG::TextureRef texture = entry.Texture;
                if ( entry.Image )
                    texture = nextImage < blockImages.size() ? blockImages[nextImage++] : RDG::TextureRef{};
                const std::string_view name    = entry.ParameterName;
                const uint32_t         element = entry.Element;
                switch ( entry.Kind )
                {
                    case RDG::ShaderResourceKind::Texture:
                        bindings.Texture( RDG::TextureParameter( name, element ), texture, entry.Access,
                                          entry.Range );
                        break;
                    case RDG::ShaderResourceKind::Sampler:
                        // BlockDeclaration::Sampler always carries the state. An entry without one is not
                        // replayed, and ValidatePassBindings names its slot as filled by neither route.
                        if ( entry.Sampler )
                            bindings.Sampler( RDG::SamplerParameter( name, element ), *entry.Sampler );
                        break;
                    case RDG::ShaderResourceKind::StorageTexture:
                        bindings.Storage( RDG::StorageTextureParameter( name, element ), texture, entry.Access,
                                          entry.Range.BaseMip );
                        break;
                    case RDG::ShaderResourceKind::UniformBuffer:
                        bindings.Uniform( RDG::UniformBufferParameter( name, element ), entry.Buffer );
                        break;
                    case RDG::ShaderResourceKind::StorageBuffer:
                        bindings.Storage( RDG::StorageBufferParameter( name, element ), entry.Buffer,
                                          entry.Access );
                        break;
                    case RDG::ShaderResourceKind::AccelerationStructure:
                        bindings.AccelerationStructure( RDG::AccelerationStructureParameter( name, element ),
                                                        entry.Buffer );
                        break;
                }
            }
            bindings.PushConstantBytes( block.PushConstantBytes );
        }
    }
} // namespace Desert::Graphic
