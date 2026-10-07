#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace Desert::Graphic::RDG
{
    namespace
    {
        bool IsSampledAccess( Access access )
        {
            return access == Access::SampledCompute || access == Access::SampledGraphics;
        }

        bool IsStorageAccess( Access access )
        {
            return access == Access::StorageRead || access == Access::StorageWrite;
        }
    } // namespace

    // Every failure goes through here so the message has one shape: "<pass>: '<slot>' <- '<resource>': <why>".
    // Only the first one is kept; a later entry never overwrites it.
    static void KeepFirstError( std::string& firstError, std::string_view pass, std::string_view shaderName,
                                std::string_view resource, std::string_view why )
    {
        if ( firstError.empty() )
            firstError = fmt::format( "{}: '{}' <- '{}': {}", pass, shaderName, resource, why );
    }

    void PassBindings::ResolveSampled( std::string_view shaderName, TextureRef texture, Access declared,
                                       SubresourceRange range, SamplerDesc sampler )
    {
        if ( !m_FirstError.empty() )
            return;
        const std::string_view pass   = m_Context.GetPassName();
        const std::string      handle = fmt::format( "texture #{}", texture.Index );
        if ( !IsSampledAccess( declared ) )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle,
                            fmt::format( "a sampled entry is declared as {}, not SampledCompute / SampledGraphics",
                                         GetAccessName( declared ) ) );
            return *this;
        }
        const bool taken = std::any_of( m_Textures.begin(), m_Textures.end(),
                                        [&]( const BoundTexture& t ) { return t.ShaderName == shaderName; } ) ||
                           std::any_of( m_Buffers.begin(), m_Buffers.end(),
                                        [&]( const BoundBuffer& b ) { return b.ShaderName == shaderName; } );
        if ( taken )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle, "the slot is already bound in this block" );
            return *this;
        }
        const Common::ResultStr<TextureBinding> resolved = m_Context.GetTexture( texture, declared, range );
        if ( !resolved.IsSuccess() )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle, resolved.GetError() );
            return *this;
        }
        m_Textures.push_back( BoundTexture{
             .ShaderName = std::string( shaderName ),
             .Kind       = ShaderResourceKind::SampledTexture,
             .Texture    = resolved.GetValue(),
             // A FaultDefault read binds the 1x1 system texture whole.
             .Range    = resolved.GetValue().Resource == texture.Index ? range : SubresourceRange::All(),
             .Declared = declared,
             .Sampler  = sampler } );
    }

    void PassBindings::ResolveStorageImage( std::string_view shaderName, TextureRef texture, Access declared,
                                            uint32_t mip )
    {
        if ( !m_FirstError.empty() )
            return;
        const std::string_view pass   = m_Context.GetPassName();
        const std::string      handle = fmt::format( "texture #{}", texture.Index );
        if ( !IsStorageAccess( declared ) )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle,
                            fmt::format( "a storage image entry is declared as {}, not StorageRead / StorageWrite",
                                         GetAccessName( declared ) ) );
            return *this;
        }
        const bool taken = std::any_of( m_Textures.begin(), m_Textures.end(),
                                        [&]( const BoundTexture& t ) { return t.ShaderName == shaderName; } ) ||
                           std::any_of( m_Buffers.begin(), m_Buffers.end(),
                                        [&]( const BoundBuffer& b ) { return b.ShaderName == shaderName; } );
        if ( taken )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle, "the slot is already bound in this block" );
            return *this;
        }
        const SubresourceRange                  range    = SubresourceRange::Mip( mip );
        const Common::ResultStr<TextureBinding> resolved = m_Context.GetTexture( texture, declared, range );
        if ( !resolved.IsSuccess() )
        {
            KeepFirstError( m_FirstError, pass, shaderName, handle, resolved.GetError() );
            return *this;
        }
        m_Textures.push_back( BoundTexture{ .ShaderName = std::string( shaderName ),
                                            .Kind       = ShaderResourceKind::StorageTexture,
                                            .Texture    = resolved.GetValue(),
                                            .Range      = range,
                                            .Declared   = declared,
                                            .Sampler    = std::nullopt } );
        return *this;
    }

    // A declared uniform or storage buffer entry: the kind decides which accesses are legal.
    static void AddBuffer( const PassContext& context, std::vector<BoundTexture>& textures,
                           std::vector<BoundBuffer>& buffers, std::string& firstError, std::string_view shaderName,
                           BufferRef buffer, Access declared, ShaderResourceKind kind )
    {
        if ( !firstError.empty() )
            return;
        const std::string_view pass   = context.GetPassName();
        const std::string      handle = fmt::format( "buffer #{}", buffer.Index );
        const bool             legal  = kind == ShaderResourceKind::UniformBuffer ? declared == Access::UniformRead
                                                                                  : IsStorageAccess( declared );
        if ( !legal )
        {
            KeepFirstError(
                 firstError, pass, shaderName, handle,
                 fmt::format( "a {} entry cannot be declared as {}",
                              kind == ShaderResourceKind::UniformBuffer ? "uniform buffer" : "storage buffer",
                              GetAccessName( declared ) ) );
            return;
        }
        const bool taken = std::any_of( textures.begin(), textures.end(),
                                        [&]( const BoundTexture& t ) { return t.ShaderName == shaderName; } ) ||
                           std::any_of( buffers.begin(), buffers.end(),
                                        [&]( const BoundBuffer& b ) { return b.ShaderName == shaderName; } );
        if ( taken )
        {
            KeepFirstError( firstError, pass, shaderName, handle, "the slot is already bound in this block" );
            return;
        }
        const Common::ResultStr<BufferBinding> resolved = context.GetBuffer( buffer, declared );
        if ( !resolved.IsSuccess() )
        {
            KeepFirstError( firstError, pass, shaderName, handle, resolved.GetError() );
            return;
        }
        buffers.push_back( BoundBuffer{ .ShaderName = std::string( shaderName ),
                                        .Kind       = kind,
                                        .Buffer     = resolved.GetValue(),
                                        .Declared   = declared } );
    }

    PassBindings& PassBindings::PushConstants( const void* data, uint32_t size )
    {
        if ( !m_FirstError.empty() )
            return *this;
        if ( !m_PushConstants.empty() )
        {
            KeepFirstError( m_FirstError, m_Context.GetPassName(), "push constants", "block",
                            "the push-constant block is already set in this block" );
            return *this;
        }
        if ( size == 0 || data == nullptr )
        {
            KeepFirstError( m_FirstError, m_Context.GetPassName(), "push constants", "block",
                            "an empty push-constant block" );
            return *this;
        }
        m_PushConstants.resize( size );
        std::memcpy( m_PushConstants.data(), data, size );
        return *this;
    }

    Common::BoolResultStr PassBindings::GetStatus() const
    {
        if ( !m_FirstError.empty() )
            return Common::MakeError( m_FirstError );
        return Common::MakeSuccess( true );
    }

    const PassContext& PassBindings::GetContext() const
    {
        return m_Context;
    }

    std::span<const BoundTexture> PassBindings::GetTextures() const
    {
        return m_Textures;
    }

    std::span<const BoundBuffer> PassBindings::GetBuffers() const
    {
        return m_Buffers;
    }

    std::span<const std::byte> PassBindings::GetPushConstants() const
    {
        return m_PushConstants;
    }
    // ── RDG-FAULT1: setup-time binding blocks ─────────────────────────────────────────────────────────────────
    namespace
    {
        std::string_view GetShaderResourceKindName( ShaderResourceKind kind )
        {
            switch ( kind )
            {
                case ShaderResourceKind::SampledTexture:
                    return "sampled texture";
                case ShaderResourceKind::StorageTexture:
                    return "storage texture";
                case ShaderResourceKind::UniformBuffer:
                    return "uniform buffer";
                case ShaderResourceKind::StorageBuffer:
                    return "storage buffer";
            }
            return "unknown";
        }

        constexpr std::string_view kNotInShaderFormat  = "'{}' is not a resource of shader '{}'";
        constexpr std::string_view kKindFormat         = "'{}' is a {} in shader '{}', bound as {}";
        constexpr std::string_view kTwiceFormat        = "'{}' of shader '{}' is bound twice by the pass";
        constexpr std::string_view kBothRoutesFormat   = "'{}' of shader '{}' is bound by the pass and by the material";
        constexpr std::string_view kNeitherRouteFormat = "'{}' of shader '{}' is filled by neither the pass nor the "
                                                         "material";
        constexpr std::string_view kPushFormat = "push constants: shader '{}' declares {} bytes, the pass gives {}";
    } // namespace

    Common::BoolResultStr ValidatePassBindings( const DeclaredBindingBlock& block )
    {
        if ( !block.Layout )
            return Common::MakeError( std::string( "the block declares no shader binding layout" ) );
        const ShaderBindingLayout& layout   = *block.Layout;
        const auto                 byOther  = [&]( std::string_view name )
        { return std::find( block.Other.Slots.begin(), block.Other.Slots.end(), name ) != block.Other.Slots.end(); };
        for ( const DeclaredBindingEntry& entry : block.Entries )
        {
            const auto sameName = [&]( const DeclaredBindingEntry& e )
            { return e.ShaderName == entry.ShaderName; };
            if ( std::count_if( block.Entries.begin(), block.Entries.end(), sameName ) > 1 )
                return Common::MakeError( std::format( kTwiceFormat, entry.ShaderName, layout.ShaderName ) );
            const auto slot = std::find_if( layout.Slots.begin(), layout.Slots.end(),
                                            [&]( const ShaderSlot& s ) { return s.Name == entry.ShaderName; } );
            if ( slot == layout.Slots.end() )
                return Common::MakeError( std::format( kNotInShaderFormat, entry.ShaderName, layout.ShaderName ) );
            if ( slot->Kind != entry.Kind )
                return Common::MakeError( std::format( kKindFormat, entry.ShaderName,
                                                       GetShaderResourceKindName( slot->Kind ), layout.ShaderName,
                                                       GetShaderResourceKindName( entry.Kind ) ) );
            if ( byOther( entry.ShaderName ) )
                return Common::MakeError( std::format( kBothRoutesFormat, entry.ShaderName, layout.ShaderName ) );
        }
        for ( const ShaderSlot& slot : layout.Slots )
        {
            const bool byBlock = std::any_of( block.Entries.begin(), block.Entries.end(),
                                              [&]( const DeclaredBindingEntry& e ) { return e.ShaderName == slot.Name; } );
            if ( !byBlock && !byOther( slot.Name ) )
                return Common::MakeError( std::format( kNeitherRouteFormat, slot.Name, layout.ShaderName ) );
        }
        const uint32_t declared = layout.PushConstantBytes;
        const uint32_t given    = block.PushConstantBytes;
        const bool     refused  = declared == 0 ? given != 0 || block.Other.PushConstants
                                                : ( given == 0 ) == !block.Other.PushConstants ||
                                                      ( given != 0 && given != declared );
        if ( refused )
            return Common::MakeError( std::format( kPushFormat, layout.ShaderName, declared, given ) );
        return Common::MakeSuccess( true );
    }

    BindingBlockBuilder PassBuilder::Bindings( ShaderBindingLayout layout, OtherRouteFill other )
    {
        return Bindings( std::make_shared<const ShaderBindingLayout>( std::move( layout ) ), std::move( other ) );
    }

    BindingBlockBuilder PassBuilder::Bindings( const std::shared_ptr<const ShaderBindingLayout>& layout,
                                               OtherRouteFill                                    other )
    {
        std::vector<DeclaredBindingBlock>& blocks = m_Builder.m_Passes[m_Pass].Blocks;
        blocks.push_back( DeclaredBindingBlock{ layout, std::move( other ), {}, 0 } );
        return BindingBlockBuilder( *this, BindingBlockRef{ m_Pass, static_cast<uint32_t>( blocks.size() - 1 ) } );
    }

    BindingBlockBuilder& BindingBlockBuilder::Sampled( std::string_view shaderName, TextureRef texture,
                                                       Access declared, SubresourceRange range, SamplerDesc sampler )
    {
        m_Pass.DeclareTexture( texture, declared, range, false, "Bindings.Sampled" );
        m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block].Entries.push_back(
             { std::string( shaderName ), ShaderResourceKind::SampledTexture, ResourceKind::Texture, texture.Index,
               declared, range, sampler } );
        return *this;
    }

    BindingBlockBuilder& BindingBlockBuilder::Storage( std::string_view shaderName, TextureRef texture,
                                                       Access declared, uint32_t mip )
    {
        m_Pass.DeclareTexture( texture, declared, SubresourceRange::Mip( mip ), IsWriteAccess( declared ),
                               "Bindings.Storage" );
        m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block].Entries.push_back(
             { std::string( shaderName ), ShaderResourceKind::StorageTexture, ResourceKind::Texture, texture.Index,
               declared, SubresourceRange::Mip( mip ), std::nullopt } );
        return *this;
    }

    BindingBlockBuilder& BindingBlockBuilder::Uniform( std::string_view shaderName, BufferRef buffer )
    {
        m_Pass.DeclareBuffer( buffer, Access::UniformRead, false, "Bindings.Uniform" );
        m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block].Entries.push_back(
             { std::string( shaderName ), ShaderResourceKind::UniformBuffer, ResourceKind::Buffer, buffer.Index,
               Access::UniformRead, SubresourceRange::All(), std::nullopt } );
        return *this;
    }

    BindingBlockBuilder& BindingBlockBuilder::Storage( std::string_view shaderName, BufferRef buffer,
                                                       Access declared )
    {
        m_Pass.DeclareBuffer( buffer, declared, IsWriteAccess( declared ), "Bindings.Storage" );
        m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block].Entries.push_back(
             { std::string( shaderName ), ShaderResourceKind::StorageBuffer, ResourceKind::Buffer, buffer.Index,
               declared, SubresourceRange::All(), std::nullopt } );
        return *this;
    }

    BindingBlockBuilder& BindingBlockBuilder::PushConstantBytes( uint32_t bytes )
    {
        m_Pass.m_Builder.m_Passes[m_Ref.Pass].Blocks[m_Ref.Block].PushConstantBytes = bytes;
        return *this;
    }

    BindingBlockRef BindingBlockBuilder::GetRef() const
    {
        return m_Ref;
    }

    PassBindings::PassBindings( const PassContext& context, BindingBlockRef block ) : m_Context( context )
    {
        const auto& passes = context.m_Builder.m_Passes;
        if ( block.Pass != context.m_Pass || block.Block >= passes[block.Pass].Blocks.size() )
        {
            m_FirstError = std::format( "{}: binding block {} of pass #{} is not a block this pass declared",
                                        context.GetPassName(), block.Block, block.Pass );
            return;
        }
        for ( const DeclaredBindingEntry& entry : passes[block.Pass].Blocks[block.Block].Entries )
        {
            switch ( entry.Kind )
            {
                case ShaderResourceKind::SampledTexture:
                    ResolveSampled( entry.ShaderName, TextureRef{ entry.Index }, entry.Declared, entry.Range,
                                    entry.Sampler.value_or( SamplerDesc::LinearClamp() ) );
                    break;
                case ShaderResourceKind::StorageTexture:
                    ResolveStorageImage( entry.ShaderName, TextureRef{ entry.Index }, entry.Declared,
                                         entry.Range.BaseMip );
                    break;
                case ShaderResourceKind::UniformBuffer:
                    AddBuffer( m_Context, m_Textures, m_Buffers, m_FirstError, entry.ShaderName,
                               BufferRef{ entry.Index }, Access::UniformRead, ShaderResourceKind::UniformBuffer );
                    break;
                case ShaderResourceKind::StorageBuffer:
                    AddBuffer( m_Context, m_Textures, m_Buffers, m_FirstError, entry.ShaderName,
                               BufferRef{ entry.Index }, entry.Declared, ShaderResourceKind::StorageBuffer );
                    break;
            }
        }
    }
} // namespace Desert::Graphic::RDG
