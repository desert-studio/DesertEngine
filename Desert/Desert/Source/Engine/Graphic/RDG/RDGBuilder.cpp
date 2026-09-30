#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <Common/Core/DevInstruments.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <format>

namespace Desert::Graphic::RDG
{
    namespace
    {
        bool RdgIsAttachmentAccess( Access access )
        {
            return access == Access::ColorTarget || access == Access::DepthWrite || access == Access::DepthRead;
        }

        // Final states describe what happens AFTER the graph; no pass can be doing them.
        bool RdgIsFinalOnlyAccess( Access access )
        {
            return access == Access::None || access == Access::Present || access == Access::HostRead;
        }

        std::string_view RdgPassKindName( PassFlags flags )
        {
            if ( HasFlag( flags, PassFlags::Raster ) )
                return "raster";
            if ( HasFlag( flags, PassFlags::Compute ) )
                return "compute";
            return "copy";
        }

        // Which accesses each kind of pass can issue. Anything else is a declaration the Vulkan executor
        // could only honour by synchronising stages the pass never runs.
        bool RdgPassKindAllows( PassFlags flags, Access access )
        {
            if ( HasFlag( flags, PassFlags::Copy ) )
                return access == Access::CopySrc || access == Access::CopyDst;
            if ( HasFlag( flags, PassFlags::Compute ) )
            {
                return access != Access::SampledGraphics && access != Access::VertexIndex &&
                       access != Access::CopySrc && access != Access::CopyDst && !RdgIsAttachmentAccess( access );
            }
            return access != Access::SampledCompute && access != Access::AccelStructBuildInput &&
                   access != Access::AccelStructBuildWrite && access != Access::CopySrc &&
                   access != Access::CopyDst;
        }
    } // namespace

    // ── Builder: resources ─────────────────────────────────────────────────────────────────────────────

    ImportedFramebuffer Builder::ImportFramebuffer( std::span<ExternalTexture* const> colors,
                                                    ExternalTexture* depth, std::string_view name,
                                                    std::span<ExternalTexture* const> resolves )
    {
        ImportedFramebuffer imported;
        if ( !resolves.empty() && resolves.size() != colors.size() )
            RecordError( std::format( "graph '{}': ImportFramebuffer('{}') has {} colour(s) and {} resolve(s)",
                                      m_Name, name, colors.size(), resolves.size() ) );
        for ( size_t i = 0; i < resolves.size(); ++i )
        {
            if ( resolves[i] == nullptr )
            {
                RecordError(
                     std::format( "graph '{}': ImportFramebuffer('{}') resolve {} is null", m_Name, name, i ) );
                imported.Resolves.push_back( {} );
                continue;
            }
            imported.Resolves.push_back(
                 RegisterExternal( *resolves[i], std::format( "{}.Resolve{}", name, i ) ) );
        }
        for ( size_t i = 0; i < colors.size(); ++i )
        {
            if ( colors[i] == nullptr )
            {
                RecordError(
                     std::format( "graph '{}': ImportFramebuffer('{}') colour {} is null", m_Name, name, i ) );
                imported.Colors.push_back( {} );
                continue;
            }
            imported.Colors.push_back( RegisterExternal( *colors[i], std::format( "{}.Color{}", name, i ) ) );
        }
        if ( depth != nullptr )
            imported.Depth = RegisterExternal( *depth, std::format( "{}.Depth", name ) );
        return imported;
    }

    TextureRef Builder::CreateTexture( const TextureDesc& desc, std::string_view name )
    {
        const uint32_t maxMips = Core::Formats::MipChainLength( std::max(
             { desc.Size.Width, desc.Size.Height, desc.Dim == TextureDim::Tex3D ? desc.Size.Depth : 1u } ) );
        if ( desc.Size.Width == 0 || desc.Size.Height == 0 || desc.Size.Depth == 0 || desc.Mips == 0 ||
             desc.Layers == 0 )
            RecordError( fmt::format( "graph '{}': texture '{}' has a zero extent ({}x{}x{}, {} mips, {} layers)",
                                      m_Name, name, desc.Size.Width, desc.Size.Height, desc.Size.Depth, desc.Mips,
                                      desc.Layers ) );
        else if ( desc.Mips > maxMips )
            RecordError( fmt::format( "graph '{}': texture '{}' asks for {} mips, a {}x{} chain has {}", m_Name,
                                      name, desc.Mips, desc.Size.Width, desc.Size.Height, maxMips ) );
        else if ( desc.Dim == TextureDim::Cube && desc.Layers % 6 != 0 )
            RecordError( fmt::format( "graph '{}': cube texture '{}' has {} layers, not a multiple of 6", m_Name,
                                      name, desc.Layers ) );
        else if ( desc.Dim == TextureDim::Tex3D && desc.Layers != 1 )
            RecordError( fmt::format( "graph '{}': volume texture '{}' has {} layers; a volume has one", m_Name,
                                      name, desc.Layers ) );
        else if ( desc.Dim != TextureDim::Tex3D && desc.Size.Depth != 1 )
            RecordError( fmt::format( "graph '{}': texture '{}' is not a volume but has depth {}", m_Name, name,
                                      desc.Size.Depth ) );

        ResourceRecord record;
        record.Name    = std::string( name );
        record.Kind    = ResourceKind::Texture;
        record.Texture = desc;
        m_Resources.push_back( std::move( record ) );
        return TextureRef{ static_cast<uint32_t>( m_Resources.size() - 1 ) };
    }

    BufferRef Builder::CreateBuffer( const BufferDesc& desc, std::string_view name )
    {
        if ( desc.Bytes == 0 )
            RecordError( fmt::format( "graph '{}': buffer '{}' has zero bytes", m_Name, name ) );

        ResourceRecord record;
        record.Name   = std::string( name );
        record.Kind   = ResourceKind::Buffer;
        record.Buffer = desc;
        m_Resources.push_back( std::move( record ) );
        return BufferRef{ static_cast<uint32_t>( m_Resources.size() - 1 ) };
    }

    TextureRef Builder::RegisterExternal( ExternalTexture& texture, std::string_view name )
    {
        if ( texture.SubresourceStates.size() != texture.Desc.SubresourceCount() )
            RecordError( fmt::format( "graph '{}': external texture '{}' carries {} subresource states for {} "
                                      "subresources ({} mips x {} layers)",
                                      m_Name, name, texture.SubresourceStates.size(),
                                      texture.Desc.SubresourceCount(), texture.Desc.Mips, texture.Desc.Layers ) );

        ResourceRecord record;
        record.Name        = std::string( name );
        record.Kind        = ResourceKind::Texture;
        record.Texture     = texture.Desc;
        record.ExternalTex = &texture;
        m_Resources.push_back( std::move( record ) );
        return TextureRef{ static_cast<uint32_t>( m_Resources.size() - 1 ) };
    }

    BufferRef Builder::RegisterExternal( ExternalBuffer& buffer, std::string_view name )
    {
        ResourceRecord record;
        record.Name        = std::string( name );
        record.Kind        = ResourceKind::Buffer;
        record.Buffer      = buffer.Desc;
        record.ExternalBuf = &buffer;
        m_Resources.push_back( std::move( record ) );
        return BufferRef{ static_cast<uint32_t>( m_Resources.size() - 1 ) };
    }

    void Builder::Extract( TextureRef texture, ExternalTexture& into, Access final )
    {
        if ( !FindResource( texture.Index, ResourceKind::Texture ) )
            return RecordError(
                 fmt::format( "graph '{}': Extract of invalid texture handle {}", m_Name, texture.Index ) );

        ResourceRecord& record = m_Resources[texture.Index];
        if ( record.HasFinalAccess )
            return RecordError(
                 fmt::format( "graph '{}': texture '{}' is extracted twice", m_Name, record.Name ) );
        if ( ( GetAccessInfo( final ).Targets & AccessTarget_Texture ) == 0 )
            return RecordError( fmt::format( "graph '{}': texture '{}' extracted into {}, a buffer-only access",
                                             m_Name, record.Name, GetAccessName( final ) ) );
        if ( record.ExternalTex && record.ExternalTex != &into )
            return RecordError( fmt::format( "graph '{}': external texture '{}' extracted into a different "
                                             "ExternalTexture than it was registered from",
                                             m_Name, record.Name ) );
        record.ExtractTex     = &into;
        record.HasFinalAccess = true;
        record.FinalAccess    = final;
    }

    void Builder::Extract( BufferRef buffer, ExternalBuffer& into, Access final )
    {
        if ( !FindResource( buffer.Index, ResourceKind::Buffer ) )
            return RecordError(
                 fmt::format( "graph '{}': Extract of invalid buffer handle {}", m_Name, buffer.Index ) );

        ResourceRecord& record = m_Resources[buffer.Index];
        if ( record.HasFinalAccess )
            return RecordError( fmt::format( "graph '{}': buffer '{}' is extracted twice", m_Name, record.Name ) );
        if ( ( GetAccessInfo( final ).Targets & AccessTarget_Buffer ) == 0 )
            return RecordError( fmt::format( "graph '{}': buffer '{}' extracted into {}, a texture-only access",
                                             m_Name, record.Name, GetAccessName( final ) ) );
        if ( record.ExternalBuf && record.ExternalBuf != &into )
            return RecordError( fmt::format( "graph '{}': external buffer '{}' extracted into a different "
                                             "ExternalBuffer than it was registered from",
                                             m_Name, record.Name ) );
        record.ExtractBuf     = &into;
        record.HasFinalAccess = true;
        record.FinalAccess    = final;
    }

    PassBuilder Builder::BeginPass( std::string_view name, PassFlags flags )
    {
        const int kinds = ( HasFlag( flags, PassFlags::Raster ) ? 1 : 0 ) +
                          ( HasFlag( flags, PassFlags::Compute ) ? 1 : 0 ) +
                          ( HasFlag( flags, PassFlags::Copy ) ? 1 : 0 );
        if ( kinds != 1 )
            RecordError(
                 fmt::format( "graph '{}': pass '{}' names {} of Raster/Compute/Copy; exactly one is required",
                              m_Name, name, kinds ) );

        PassRecord record;
        record.Name  = std::string( name );
        record.Flags = flags;
        m_Passes.push_back( std::move( record ) );
        return PassBuilder( *this, static_cast<uint32_t>( m_Passes.size() - 1 ) );
    }

    void Builder::RecordError( std::string message )
    {
        if ( m_DeclarationError.empty() )
            m_DeclarationError = std::move( message );
    }

    const Builder::ResourceRecord* Builder::FindResource( uint32_t index, ResourceKind kind ) const
    {
        if ( index >= m_Resources.size() || m_Resources[index].Kind != kind )
            return nullptr;
        return &m_Resources[index];
    }

    // ── PassBuilder ────────────────────────────────────────────────────────────────────────────────────

    void PassBuilder::Read( TextureRef texture, Access access, SubresourceRange range )
    {
        DeclareTexture( texture, access, range, false, "Read" );
    }

    void PassBuilder::Write( TextureRef texture, Access access, SubresourceRange range )
    {
        DeclareTexture( texture, access, range, true, "Write" );
    }

    void PassBuilder::Read( BufferRef buffer, Access access )
    {
        DeclareBuffer( buffer, access, false, "Read" );
    }

    void PassBuilder::Write( BufferRef buffer, Access access )
    {
        DeclareBuffer( buffer, access, true, "Write" );
    }

    void PassBuilder::ColorTarget( uint32_t slot, TextureRef texture, const LoadOp& load, uint32_t mip,
                                   uint32_t layer, StoreAction store )
    {
        DeclareAttachment( slot, false, texture, Access::ColorTarget, load, mip, layer, store );
    }

    void PassBuilder::ResolveTarget( uint32_t slot, TextureRef texture )
    {
        DeclareAttachment( slot, false, texture, Access::ColorTarget, LoadOp::DontCare(), 0, 0, StoreAction::Store,
                           true );
    }

    void PassBuilder::DepthTarget( TextureRef texture, const LoadOp& load, bool write, uint32_t layer,
                                   StoreAction store )
    {
        DeclareAttachment( 0, true, texture, write ? Access::DepthWrite : Access::DepthRead, load, 0, layer,
                           store );
    }

    void PassBuilder::DeclareTexture( TextureRef texture, Access access, SubresourceRange range, bool asWrite,
                                      std::string_view call )
    {
        Builder::PassRecord&           pass     = m_Builder.m_Passes[m_Pass];
        const Builder::ResourceRecord* resource = m_Builder.FindResource( texture.Index, ResourceKind::Texture );
        const std::string&             graph    = m_Builder.m_Name;
        if ( !resource )
            return m_Builder.RecordError( fmt::format( "graph '{}' pass '{}': {}() of invalid texture handle {}",
                                                       graph, pass.Name, call, texture.Index ) );
        if ( RdgIsFinalOnlyAccess( access ) )
            return m_Builder.RecordError( fmt::format(
                 "graph '{}' pass '{}': {}('{}', {}) - {} is a state after the "
                 "graph (Extract), not something a pass does",
                 graph, pass.Name, call, resource->Name, GetAccessName( access ), GetAccessName( access ) ) );
        if ( ( GetAccessInfo( access ).Targets & AccessTarget_Texture ) == 0 )
            return m_Builder.RecordError(
                 fmt::format( "graph '{}' pass '{}': {}('{}', {}) - a buffer-only access on a "
                              "texture",
                              graph, pass.Name, call, resource->Name, GetAccessName( access ) ) );
        if ( RdgIsAttachmentAccess( access ) )
            return m_Builder.RecordError(
                 fmt::format( "graph '{}' pass '{}': {}('{}', {}) - attachments are declared "
                              "with ColorTarget()/DepthTarget(), which carry the load op",
                              graph, pass.Name, call, resource->Name, GetAccessName( access ) ) );
        if ( asWrite != IsWriteAccess( access ) )
            return m_Builder.RecordError( fmt::format(
                 "graph '{}' pass '{}': {}('{}', {}) - {} is a {} access", graph, pass.Name, call, resource->Name,
                 GetAccessName( access ), GetAccessName( access ), asWrite ? "read" : "write" ) );
        if ( !RdgPassKindAllows( pass.Flags, access ) )
            return m_Builder.RecordError( fmt::format( "graph '{}' pass '{}': {}('{}', {}) in a {} pass", graph,
                                                       pass.Name, call, resource->Name, GetAccessName( access ),
                                                       RdgPassKindName( pass.Flags ) ) );

        const TextureDesc& desc     = resource->Texture;
        SubresourceRange   resolved = range;
        if ( resolved.BaseMip >= desc.Mips || resolved.BaseLayer >= desc.Layers )
            return m_Builder.RecordError(
                 fmt::format( "graph '{}' pass '{}': {}('{}') mip {} layer {} is outside {} "
                              "mips x {} layers",
                              graph, pass.Name, call, resource->Name, resolved.BaseMip, resolved.BaseLayer,
                              desc.Mips, desc.Layers ) );
        if ( resolved.MipCount == kAllRemaining )
            resolved.MipCount = desc.Mips - resolved.BaseMip;
        if ( resolved.LayerCount == kAllRemaining )
            resolved.LayerCount = desc.Layers - resolved.BaseLayer;
        if ( resolved.MipCount == 0 || resolved.LayerCount == 0 ||
             resolved.BaseMip + resolved.MipCount > desc.Mips ||
             resolved.BaseLayer + resolved.LayerCount > desc.Layers )
            return m_Builder.RecordError(
                 fmt::format( "graph '{}' pass '{}': {}('{}') mips [{}, +{}) layers [{}, +{}) "
                              "do not fit {} mips x {} layers",
                              graph, pass.Name, call, resource->Name, resolved.BaseMip, resolved.MipCount,
                              resolved.BaseLayer, resolved.LayerCount, desc.Mips, desc.Layers ) );

        pass.Uses.push_back( { texture.Index, access, resolved, -1 } );
    }

    void PassBuilder::DeclareBuffer( BufferRef buffer, Access access, bool asWrite, std::string_view call )
    {
        Builder::PassRecord&           pass     = m_Builder.m_Passes[m_Pass];
        const Builder::ResourceRecord* resource = m_Builder.FindResource( buffer.Index, ResourceKind::Buffer );
        const std::string&             graph    = m_Builder.m_Name;
        if ( !resource )
            return m_Builder.RecordError( fmt::format( "graph '{}' pass '{}': {}() of invalid buffer handle {}",
                                                       graph, pass.Name, call, buffer.Index ) );
        if ( RdgIsFinalOnlyAccess( access ) )
            return m_Builder.RecordError(
                 fmt::format( "graph '{}' pass '{}': {}('{}', {}) - a state after the graph "
                              "(Extract), not something a pass does",
                              graph, pass.Name, call, resource->Name, GetAccessName( access ) ) );
        if ( ( GetAccessInfo( access ).Targets & AccessTarget_Buffer ) == 0 )
            return m_Builder.RecordError(
                 fmt::format( "graph '{}' pass '{}': {}('{}', {}) - a texture-only access on a "
                              "buffer",
                              graph, pass.Name, call, resource->Name, GetAccessName( access ) ) );
        if ( asWrite != IsWriteAccess( access ) )
            return m_Builder.RecordError( fmt::format(
                 "graph '{}' pass '{}': {}('{}', {}) - {} is a {} access", graph, pass.Name, call, resource->Name,
                 GetAccessName( access ), GetAccessName( access ), asWrite ? "read" : "write" ) );
        if ( !RdgPassKindAllows( pass.Flags, access ) )
            return m_Builder.RecordError( fmt::format( "graph '{}' pass '{}': {}('{}', {}) in a {} pass", graph,
                                                       pass.Name, call, resource->Name, GetAccessName( access ),
                                                       RdgPassKindName( pass.Flags ) ) );

        pass.Uses.push_back( { buffer.Index, access, SubresourceRange{ 0, 1, 0, 1 }, -1 } );
    }

    void PassBuilder::DeclareAttachment( uint32_t slot, bool isDepth, TextureRef texture, Access access,
                                         const LoadOp& load, uint32_t mip, uint32_t layer, StoreAction store,
                                         bool isResolve )
    {
        Builder::PassRecord&           pass     = m_Builder.m_Passes[m_Pass];
        const Builder::ResourceRecord* resource = m_Builder.FindResource( texture.Index, ResourceKind::Texture );
        const std::string&             graph    = m_Builder.m_Name;
        const std::string_view call = isResolve ? "ResolveTarget" : ( isDepth ? "DepthTarget" : "ColorTarget" );
        if ( !resource )
            return m_Builder.RecordError( fmt::format( "graph '{}' pass '{}': {}() of invalid texture handle {}",
                                                       graph, pass.Name, call, texture.Index ) );
        if ( !HasFlag( pass.Flags, PassFlags::Raster ) )
            return m_Builder.RecordError( fmt::format( "graph '{}' pass '{}': {}('{}') in a {} pass", graph,
                                                       pass.Name, call, resource->Name,
                                                       RdgPassKindName( pass.Flags ) ) );
        if ( access == Access::DepthRead && load.Action == LoadAction::Clear )
            return m_Builder.RecordError(
                 fmt::format( "graph '{}' pass '{}': DepthTarget('{}') clears a read-only "
                              "depth attachment",
                              graph, pass.Name, resource->Name ) );
        for ( const Builder::AttachmentRecord& existing : pass.Attachments )
        {
            if ( existing.IsDepth == isDepth && existing.IsResolve == isResolve &&
                 ( isDepth || existing.Slot == slot ) )
                return m_Builder.RecordError( fmt::format( "graph '{}' pass '{}': {}('{}') binds slot {} twice",
                                                           graph, pass.Name, call, resource->Name, slot ) );
        }
        // One render pass has one sample count: every colour and depth attachment shares it. A resolve target is
        // single-sample and matches the multisampled colour of its slot in format and size.
        const TextureDesc& self = resource->Texture;
        for ( const Builder::AttachmentRecord& existing : pass.Attachments )
        {
            const Builder::ResourceRecord& otherRecord = m_Builder.m_Resources[existing.Resource];
            const TextureDesc&             other       = otherRecord.Texture;
            if ( isResolve && !existing.IsResolve && !existing.IsDepth && existing.Slot == slot &&
                 ( other.Samples <= 1 || self.Samples != 1 || other.Format != self.Format ||
                   other.Size.Width != self.Size.Width || other.Size.Height != self.Size.Height ) )
                return m_Builder.RecordError( fmt::format(
                     "graph '{}' pass '{}': ResolveTarget('{}') cannot resolve colour slot {} ('{}'): the colour "
                     "has {} sample(s), format {}, {}x{}; the resolve target {} sample(s), format {}, {}x{}",
                     graph, pass.Name, resource->Name, slot, otherRecord.Name, other.Samples,
                     static_cast<uint32_t>( other.Format ), other.Size.Width, other.Size.Height, self.Samples,
                     static_cast<uint32_t>( self.Format ), self.Size.Width, self.Size.Height ) );
            if ( !isResolve && !existing.IsResolve && other.Samples != self.Samples )
                return m_Builder.RecordError(
                     fmt::format( "graph '{}' pass '{}': {}('{}') has {} sample(s), '{}' in the same pass has {}",
                                  graph, pass.Name, call, resource->Name, self.Samples, otherRecord.Name,
                                  other.Samples ) );
        }
        if ( isResolve &&
             std::none_of( pass.Attachments.begin(), pass.Attachments.end(),
                           [slot]( const Builder::AttachmentRecord& existing )
                           { return !existing.IsDepth && !existing.IsResolve && existing.Slot == slot; } ) )
            return m_Builder.RecordError(
                 fmt::format( "graph '{}' pass '{}': ResolveTarget('{}') for colour slot {}, which has no "
                              "ColorTarget declared before it",
                              graph, pass.Name, resource->Name, slot ) );

        const TextureDesc& desc       = resource->Texture;
        const uint32_t     baseLayer  = layer == kAllRemaining ? 0 : layer;
        const uint32_t     layerCount = layer == kAllRemaining ? desc.Layers : 1;
        if ( mip >= desc.Mips || baseLayer >= desc.Layers )
            return m_Builder.RecordError(
                 fmt::format( "graph '{}' pass '{}': {}('{}') mip {} layer {} is outside {} "
                              "mips x {} layers",
                              graph, pass.Name, call, resource->Name, mip, baseLayer, desc.Mips, desc.Layers ) );

        Builder::AttachmentRecord attachment;
        attachment.Slot       = slot;
        attachment.IsDepth    = isDepth;
        attachment.IsResolve  = isResolve;
        attachment.Resource   = texture.Index;
        attachment.Load       = load;
        attachment.Mip        = mip;
        attachment.BaseLayer  = baseLayer;
        attachment.LayerCount = layerCount;
        attachment.Store      = store;
        pass.Attachments.push_back( attachment );
        pass.Uses.push_back( { texture.Index, access, SubresourceRange{ mip, 1, baseLayer, layerCount },
                               static_cast<int32_t>( pass.Attachments.size() - 1 ) } );
    }

    // ── PassContext ────────────────────────────────────────────────────────────────────────────────────

    std::string_view PassContext::GetPassName() const
    {
        return m_Builder.m_Passes[m_Pass].Name;
    }

    Common::ResultStr<TextureBinding> PassContext::GetTexture( TextureRef texture, Access access,
                                                               SubresourceRange range ) const
    {
        const Builder::PassRecord&     pass     = m_Builder.m_Passes[m_Pass];
        const Builder::ResourceRecord* resource = m_Builder.FindResource( texture.Index, ResourceKind::Texture );
        if ( !resource )
            return Common::MakeFormattedError<TextureBinding>(
                 "graph '{}' pass '{}': GetTexture of invalid handle {}", m_Builder.m_Name, pass.Name,
                 texture.Index );
#if DESERT_DEV_INSTRUMENTS
        const TextureDesc& desc   = resource->Texture;
        const uint32_t     mipEnd = range.MipCount == kAllRemaining ? desc.Mips : range.BaseMip + range.MipCount;
        const uint32_t     layerEnd =
             range.LayerCount == kAllRemaining ? desc.Layers : range.BaseLayer + range.LayerCount;
        for ( uint32_t layer = range.BaseLayer; layer < layerEnd; ++layer )
        {
            for ( uint32_t mip = range.BaseMip; mip < mipEnd; ++mip )
            {
                bool declared = false;
                for ( const Builder::ResourceUse& use : pass.Uses )
                {
                    declared =
                         declared ||
                         ( use.Resource == texture.Index && use.Usage == access && mip >= use.Range.BaseMip &&
                           mip < use.Range.BaseMip + use.Range.MipCount && layer >= use.Range.BaseLayer &&
                           layer < use.Range.BaseLayer + use.Range.LayerCount );
                }
                if ( !declared )
                    return Common::MakeFormattedError<TextureBinding>(
                         "graph '{}' pass '{}' uses texture '{}' mip {} layer {} as {} without declaring it in "
                         "its setup",
                         m_Builder.m_Name, pass.Name, resource->Name, mip, layer, GetAccessName( access ) );
            }
        }
#endif
        TextureBinding binding;
        binding.Resource = texture.Index;
        binding.Name     = resource->Name;
        binding.Desc     = &resource->Texture;
        binding.External = resource->ExternalTex ? resource->ExternalTex : resource->ExtractTex;
        binding.Memory   = m_Result.FindAllocation( texture.Index );
        binding.Physical = m_Backend.GetPhysicalTexture( texture.Index ).get();
        return Common::MakeSuccess( binding );
    }

    Common::ResultStr<BufferBinding> PassContext::GetBuffer( BufferRef buffer, Access access ) const
    {
        const Builder::PassRecord&     pass     = m_Builder.m_Passes[m_Pass];
        const Builder::ResourceRecord* resource = m_Builder.FindResource( buffer.Index, ResourceKind::Buffer );
        if ( !resource )
            return Common::MakeFormattedError<BufferBinding>(
                 "graph '{}' pass '{}': GetBuffer of invalid handle {}", m_Builder.m_Name, pass.Name,
                 buffer.Index );
#if DESERT_DEV_INSTRUMENTS
        bool declared = false;
        for ( const Builder::ResourceUse& use : pass.Uses )
            declared = declared || ( use.Resource == buffer.Index && use.Usage == access );
        if ( !declared )
            return Common::MakeFormattedError<BufferBinding>(
                 "graph '{}' pass '{}' uses buffer '{}' as {} without declaring it in its setup", m_Builder.m_Name,
                 pass.Name, resource->Name, GetAccessName( access ) );
#endif
        BufferBinding binding;
        binding.Resource = buffer.Index;
        binding.Name     = resource->Name;
        binding.Desc     = &resource->Buffer;
        binding.External = resource->ExternalBuf ? resource->ExternalBuf : resource->ExtractBuf;
        binding.Memory   = m_Result.FindAllocation( buffer.Index );
        binding.Physical = m_Backend.GetPhysicalBuffer( buffer.Index ).get();
        return Common::MakeSuccess( binding );
    }

    // ── Execute ────────────────────────────────────────────────────────────────────────────────────────

    // Mirrors one barrier batch into the imported textures it touches: each covered subresource takes the
    // barrier's After state, then the owner's record hook sees the new states at once. The image's own layout
    // record (read by descriptor binds and by code outside the graph) therefore follows the GPU barrier by
    // barrier instead of catching up at the end of Execute.
    Common::BoolResultStr Builder::RecordExternalStates( std::span<const Barrier> barriers )
    {
        std::vector<uint32_t> touched; // resources, one per external texture
        for ( const Barrier& barrier : barriers )
        {
            if ( barrier.Kind != ResourceKind::Texture )
                continue;
            const ResourceRecord&  record   = m_Resources[barrier.Resource];
            ExternalTexture* const external = record.ExternalTex;
            if ( external == nullptr )
                continue;
            const TextureDesc& desc   = external->Desc;
            const uint32_t     mips   = barrier.Range.MipCount == kAllRemaining ? desc.Mips - barrier.Range.BaseMip
                                                                                : barrier.Range.MipCount;
            const uint32_t     layers = barrier.Range.LayerCount == kAllRemaining
                                             ? desc.Layers - barrier.Range.BaseLayer
                                             : barrier.Range.LayerCount;
            for ( uint32_t layer = barrier.Range.BaseLayer; layer < barrier.Range.BaseLayer + layers; ++layer )
                for ( uint32_t mip = barrier.Range.BaseMip; mip < barrier.Range.BaseMip + mips; ++mip )
                    external->SubresourceStates[desc.SubresourceIndex( mip, layer )] = barrier.After;
            if ( std::find( touched.begin(), touched.end(), barrier.Resource ) == touched.end() )
                touched.push_back( barrier.Resource );
        }
        for ( const uint32_t resource : touched )
        {
            const ResourceRecord& record = m_Resources[resource];
            if ( !record.ExternalTex->RecordStates )
                continue;
            Common::BoolResultStr recorded =
                 record.ExternalTex->RecordStates( record.ExternalTex->SubresourceStates, false );
            if ( !recorded )
                return Common::MakeFormattedError( "texture '{}': {}", record.Name, recorded.GetError() );
        }
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr Builder::Execute( IBackend& backend )
    {
        if ( m_Executed )
            return Common::MakeFormattedError( "graph '{}' was already executed; a graph is built per frame",
                                               m_Name );
        m_Executed = true;

        Common::ResultStr<CompileResult> compiled = Compile( backend.GetMemoryRequirements() );
        if ( !compiled )
            return Common::MakeError( compiled.GetError() );
        const CompileResult& result = compiled.GetValue();

        std::vector<ResourceView> views( m_Resources.size() );
        for ( uint32_t r = 0; r < m_Resources.size(); ++r )
        {
            const ResourceRecord& record = m_Resources[r];
            ResourceView&         view   = views[r];
            view.Resource                = r;
            view.Name                    = record.Name;
            view.Kind                    = record.Kind;
            view.Texture                 = record.Kind == ResourceKind::Texture ? &record.Texture : nullptr;
            view.Buffer                  = record.Kind == ResourceKind::Buffer ? &record.Buffer : nullptr;
            view.ExternalTex             = record.ExternalTex;
            view.ExternalBuf             = record.ExternalBuf;
            view.Extracted               = record.IsExtracted() && !record.IsExternal();
        }
        for ( const DerivedUsage& usage : result.Usages )
            views[usage.Resource].AccessMask = usage.AccessMask;
        for ( const CompiledPass& compiledPass : result.Passes )
        {
            for ( const ResourceUse& use : m_Passes[compiledPass.Pass].Uses )
                views[use.Resource].Used = true;
        }

        const GraphView       graph{ m_Name, views, &result };
        Common::BoolResultStr begun = backend.BeginGraph( graph );
        if ( !begun )
            return Common::MakeFormattedError( "graph '{}': {}", m_Name, begun.GetError() );

        for ( const CompiledPass& compiledPass : result.Passes )
        {
            backend.BeginPass( compiledPass );
            if ( !compiledPass.Barriers.empty() )
            {
                backend.RecordBarriers( compiledPass.Barriers );
                Common::BoolResultStr recorded = RecordExternalStates( compiledPass.Barriers );
                if ( !recorded )
                {
                    backend.AbandonGraph();
                    return Common::MakeFormattedError( "graph '{}' pass '{}': {}", m_Name, compiledPass.Name,
                                                       recorded.GetError() );
                }
            }
            const bool rendering =
                 HasFlag( compiledPass.Flags, PassFlags::Raster ) && !compiledPass.Attachments.empty();
            if ( rendering && !compiledPass.ContinuesRenderPass )
            {
                Common::BoolResultStr started = backend.BeginRenderPass( compiledPass );
                if ( !started )
                {
                    backend.AbandonGraph();
                    return Common::MakeFormattedError( "graph '{}' pass '{}': {}", m_Name, compiledPass.Name,
                                                       started.GetError() );
                }
            }
            PassContext           context( *this, result, backend, compiledPass.Pass );
            Common::BoolResultStr outcome = m_Passes[compiledPass.Pass].Exec( context );
            if ( !outcome )
            {
                backend.AbandonGraph();
                return Common::MakeFormattedError( "graph '{}' pass '{}' failed: {}", m_Name, compiledPass.Name,
                                                   outcome.GetError() );
            }
            if ( rendering && !compiledPass.KeepsRenderPassOpen )
                backend.EndRenderPass();
            backend.EndPass( compiledPass );
        }

        // An extracted transient's image is taken before EndGraph releases the graph's hold on it.
        std::vector<std::shared_ptr<IPhysicalTexture>> extractedTextures( m_Resources.size() );
        std::vector<std::shared_ptr<IPhysicalBuffer>>  extractedBuffers( m_Resources.size() );
        for ( const ExternalFinalState& final : result.ExternalFinalStates )
        {
            const ResourceRecord& record = m_Resources[final.Resource];
            if ( record.IsExternal() )
                continue;
            if ( record.Kind == ResourceKind::Texture )
                extractedTextures[final.Resource] = backend.GetPhysicalTexture( final.Resource );
            else
                extractedBuffers[final.Resource] = backend.GetPhysicalBuffer( final.Resource );
        }

        Common::BoolResultStr ended = backend.EndGraph( result.FinalBarriers );
        if ( !ended )
            return Common::MakeFormattedError( "graph '{}': {}", m_Name, ended.GetError() );

        std::string recordError; // the first failed layout write-back; every other one still runs
        for ( const ExternalFinalState& final : result.ExternalFinalStates )
        {
            const ResourceRecord& record = m_Resources[final.Resource];
            if ( record.Kind == ResourceKind::Texture )
            {
                ExternalTexture* target   = record.ExternalTex ? record.ExternalTex : record.ExtractTex;
                target->Desc              = record.Texture;
                target->SubresourceStates = final.SubresourceStates;
                // The layouts match the last barrier's (already recorded); the final states add the stages and
                // accesses of the reads merged after it, and the owner checks the image can record them.
                if ( target->RecordStates )
                {
                    Common::BoolResultStr recorded = target->RecordStates( target->SubresourceStates, true );
                    if ( !recorded && recordError.empty() )
                        recordError = std::format( "graph '{}' texture '{}': {}", m_Name, record.Name,
                                                   recorded.GetError() );
                }
                if ( !record.IsExternal() )
                    target->Physical = std::move( extractedTextures[final.Resource] );
            }
            else
            {
                ExternalBuffer* target = record.ExternalBuf ? record.ExternalBuf : record.ExtractBuf;
                target->Desc           = record.Buffer;
                target->State          = final.SubresourceStates.front();
                if ( target->RecordFinalState )
                {
                    Common::BoolResultStr recorded = target->RecordFinalState( target->State );
                    if ( !recorded && recordError.empty() )
                        recordError =
                             std::format( "graph '{}' buffer '{}': {}", m_Name, record.Name, recorded.GetError() );
                }
                if ( !record.IsExternal() )
                    target->Physical = std::move( extractedBuffers[final.Resource] );
            }
        }
        if ( !recordError.empty() )
            return Common::MakeError( recordError );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::RDG
