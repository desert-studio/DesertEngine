#include <Engine/Graphic/RenderPassDeclaration.hpp>

#include <Engine/Graphic/RDG/RDGBuilder.hpp>

namespace Desert::Graphic
{
    void DeclareRefsOn( RDG::PassBuilder& pass, const RenderPassDeclaration& declared,
                        std::span<const RDG::TextureRef> blockImages )
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
                const uint64_t         hash    = entry.NameHash;
                switch ( entry.Kind )
                {
                    case RDG::ShaderResourceKind::Texture:
                        bindings.Texture( RDG::TextureParameter( name, element, hash ), texture, entry.Access,
                                          entry.Range );
                        break;
                    case RDG::ShaderResourceKind::Sampler:
                        // BlockDeclaration::Sampler always carries the state. An entry without one is not
                        // replayed, and ValidatePassBindings names its slot as filled by neither route.
                        if ( entry.Sampler )
                            bindings.Sampler( RDG::SamplerParameter( name, element, hash ), *entry.Sampler );
                        break;
                    case RDG::ShaderResourceKind::StorageTexture:
                        bindings.Storage( RDG::StorageTextureParameter( name, element, hash ), texture,
                                          entry.Access, entry.Range.BaseMip );
                        break;
                    case RDG::ShaderResourceKind::UniformBuffer:
                        bindings.Uniform( RDG::UniformBufferParameter( name, element, hash ), entry.Buffer );
                        break;
                    case RDG::ShaderResourceKind::StorageBuffer:
                        bindings.Storage( RDG::StorageBufferParameter( name, element, hash ), entry.Buffer,
                                          entry.Access );
                        break;
                    case RDG::ShaderResourceKind::AccelerationStructure:
                        bindings.AccelerationStructure( RDG::AccelerationStructureParameter( name, element, hash ),
                                                        entry.Buffer );
                        break;
                }
            }
            bindings.PushConstants( block.PushConstants );
        }
    }
} // namespace Desert::Graphic
