// RenderGraphPassBindings - the contract tests of RDG-A2 (how a renderer binds a graph resource: one per-exec
// RDG::PassBindings, RDGPassBindings.hpp). Written BEFORE the implementation, against the declarations only:
// until RDG-A2-1 adds RDGPassBindings.cpp to this suite, it does not link, and after that every test here must
// pass unchanged. Device-free: the backend is the no-op mock below. The consumer-side rules (a name the shader
// does not declare, a kind that does not match the reflected descriptor type, an unfilled or doubly filled
// slot) need a shader and a device and belong to the RenderGraphVulkan suite, written with Renderer::
// DispatchCompute / DrawFullscreen in RDG-A2-1.

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <type_traits>

using namespace Desert::Graphic::RDG;
using Desert::Core::Formats::ImageFormat;

namespace
{
    class FixedEstimate final : public IMemoryRequirementsProvider
    {
    public:
        Common::ResultStr<MemoryRequirements> GetTextureRequirements( const TextureDesc& desc,
                                                                      uint32_t ) const override
        {
            const uint64_t bytes = uint64_t( desc.Size.Width ) * desc.Size.Height * 8u * desc.Layers;
            return Common::MakeSuccess(
                 MemoryRequirements{ ( bytes + 0xFFFFu ) & ~uint64_t( 0xFFFFu ), 0x10000u, ~0u } );
        }
        Common::ResultStr<MemoryRequirements> GetBufferRequirements( const BufferDesc& desc,
                                                                     uint32_t ) const override
        {
            return Common::MakeSuccess(
                 MemoryRequirements{ ( desc.Bytes + 255u ) & ~uint64_t( 255u ), 256u, ~0u } );
        }
    };

    class NoPlacementAllocator final : public ITransientAllocator
    {
    public:
        void BeginFrameSlot( uint32_t ) override
        {
        }
        Common::BoolResultStr ReserveHeaps( std::string_view, std::span<const TransientHeapDesc> ) override
        {
            return Common::MakeSuccess( true );
        }
        Common::ResultStr<std::shared_ptr<IPhysicalTexture>> PlaceTexture( const Allocation&, const TextureDesc&,
                                                                           uint32_t, std::string_view ) override
        {
            return Common::MakeError<std::shared_ptr<IPhysicalTexture>>( "the mock places nothing" );
        }
        Common::ResultStr<std::shared_ptr<IPhysicalBuffer>> PlaceBuffer( const Allocation&, const BufferDesc&,
                                                                         uint32_t, std::string_view ) override
        {
            return Common::MakeError<std::shared_ptr<IPhysicalBuffer>>( "the mock places nothing" );
        }
        void EndGraph( std::string_view ) override
        {
        }
        TransientAllocatorStats GetStats() const override
        {
            return {};
        }
    };

    // Every call succeeds and records nothing: these tests look only at what the exec lambda sees.
    class NoOpBackend final : public IBackend
    {
    public:
        BackendKind GetKind() const override
        {
            return BackendKind::Recording;
        }
        const IMemoryRequirementsProvider& GetMemoryRequirements() const override
        {
            return m_Memory;
        }
        ITransientAllocator& GetTransientAllocator() override
        {
            return m_Allocator;
        }
        PipeCapabilities GetPipeCapabilities() const override
        {
            return {};
        }
        AsyncComputeFallbackLog& GetAsyncComputeFallbackLog() override
        {
            return m_FallbackLog;
        }
        Common::BoolResultStr BeginPipeSegment( const PipeSegment& ) override
        {
            return Common::MakeSuccess( true );
        }
        Common::BoolResultStr EndPipeSegment( const PipeSegment& ) override
        {
            return Common::MakeSuccess( true );
        }
        void RecordEpilogueBarriers( std::span<const Barrier> ) override
        {
        }
        Common::BoolResultStr BeginGraph( const GraphView& ) override
        {
            return Common::MakeSuccess( true );
        }
        void BeginPass( const CompiledPass& ) override
        {
        }
        void RecordBarriers( std::span<const Barrier> ) override
        {
        }
        Common::BoolResultStr BeginRenderPass( const CompiledPass& ) override
        {
            return Common::MakeSuccess( true );
        }
        void EndRenderPass() override
        {
        }
        void EndPass( const CompiledPass& ) override
        {
        }
        Common::BoolResultStr EndGraph( std::span<const Barrier> ) override
        {
            return Common::MakeSuccess( true );
        }
        void AbandonGraph() override
        {
        }
        std::shared_ptr<IPhysicalTexture> GetPhysicalTexture( uint32_t ) const override
        {
            return nullptr;
        }
        std::shared_ptr<IPhysicalBuffer> GetPhysicalBuffer( uint32_t ) const override
        {
            return nullptr;
        }

    private:
        FixedEstimate           m_Memory;
        NoPlacementAllocator    m_Allocator;
        AsyncComputeFallbackLog m_FallbackLog{ []( std::string_view ) {} };
    };

    TextureDesc Tex2D( uint32_t width, uint32_t height, uint32_t mips = 1 )
    {
        TextureDesc desc;
        desc.Size   = { width, height, 1 };
        desc.Format = ImageFormat::RGBA16F;
        desc.Mips   = mips;
        return desc;
    }

    // One compute pass "Bloom" that samples mip 0 of @p chain and writes mip 1; @p exec runs inside it.
    template <class Exec>
    Common::BoolResultStr RunBloomLikePass( Exec&& exec )
    {
        ExternalTexture  chainImage( Tex2D( 64, 64, 2 ), Access::None );
        ExternalTexture  otherImage( Tex2D( 8, 8 ), Access::None );
        Builder          graph( "pass-bindings" );
        const TextureRef chain = graph.RegisterExternal( chainImage, "Bloom" );
        const TextureRef other = graph.RegisterExternal( otherImage, "Undeclared" );
        graph.AddPass(
             "PostFX: BloomUpsample1", PassFlags::Compute,
             [&]( PassBuilder& pass )
             {
                 pass.Read( chain, Access::SampledCompute, SubresourceRange::Mip( 1 ) );
                 pass.Write( chain, Access::StorageWrite, SubresourceRange::Mip( 0 ) );
             },
             [&]( PassContext& context ) -> Common::BoolResultStr { return exec( context, chain, other ); } );
        NoOpBackend backend;
        return graph.Execute( backend );
    }
} // namespace

// The block lives and dies inside one exec lambda: it cannot be copied into a renderer or moved out of the
// lambda (RDG-CONTRACTS A(3): nothing written from a graph binding outlives the exec).
TEST( RenderGraphPassBindings, ABlockCannotLeaveTheExecThatBuiltIt )
{
    static_assert( !std::is_copy_constructible_v<PassBindings> );
    static_assert( !std::is_copy_assignable_v<PassBindings> );
    static_assert( !std::is_move_constructible_v<PassBindings> );
    static_assert( !std::is_move_assignable_v<PassBindings> );
    static_assert( !std::is_default_constructible_v<PassBindings> );
    SUCCEED();
}

// Declared entries resolve to THIS execution's binding, keep the declared range and access, and name the slot
// by its shader name. Storage is exactly one mip over every layer.
TEST( RenderGraphPassBindings, DeclaredEntriesResolveWithTheirRangeAndAccess )
{
    const Common::BoolResultStr executed = RunBloomLikePass(
         []( PassContext& context, TextureRef chain, TextureRef ) -> Common::BoolResultStr
         {
             const float  push[3] = { 1.0f / 32.0f, 1.0f / 32.0f, 1.0f };
             PassBindings bindings( context );
             bindings.Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ), SamplerDesc::LinearClamp())
                  .Storage( "u_Output", chain, Access::StorageWrite, 0 )
                  .PushConstants( push, sizeof( push ) );
             EXPECT_TRUE( bindings.GetStatus().IsSuccess() ) << bindings.GetStatus().GetError();
             EXPECT_EQ( &bindings.GetContext(), &context );
             EXPECT_EQ( bindings.GetTextures().size(), 2u );
             if ( bindings.GetTextures().size() != 2u )
                 return Common::MakeSuccess( true );
             const BoundTexture& source = bindings.GetTextures()[0];
             const BoundTexture& output = bindings.GetTextures()[1];
             EXPECT_EQ( source.ShaderName, "u_Source" );
             EXPECT_EQ( source.Kind, ShaderResourceKind::SampledTexture );
             EXPECT_EQ( source.Texture.Resource, chain.Index );
             EXPECT_EQ( source.Range, SubresourceRange::Mip( 1 ) );
             EXPECT_EQ( source.Declared, Access::SampledCompute );
             EXPECT_EQ( output.Kind, ShaderResourceKind::StorageTexture );
             EXPECT_EQ( output.Range, SubresourceRange::Mip( 0 ) );
             EXPECT_EQ( output.Declared, Access::StorageWrite );
             EXPECT_EQ( bindings.GetPushConstants().size(), sizeof( push ) );
             EXPECT_EQ( std::memcmp( bindings.GetPushConstants().data(), push, sizeof( push ) ), 0 );
             return Common::MakeSuccess( true );
         } );
    EXPECT_TRUE( executed.IsSuccess() ) << executed.GetError();
}

#if DESERT_DEV_INSTRUMENTS
// An undeclared resource fails at the entry, naming the pass, the shader slot and the resource - not later as
// a missing barrier and a wrong picture.
TEST( RenderGraphPassBindings, AnUndeclaredResourceFailsNamingPassSlotAndResource )
{
    std::string error;
    RunBloomLikePass(
         [&]( PassContext& context, TextureRef, TextureRef other ) -> Common::BoolResultStr
         {
             PassBindings bindings( context );
             bindings.Sampled( "u_Source", other, Access::SampledCompute, SubresourceRange::All(), SamplerDesc::LinearClamp());
             error = bindings.GetStatus().IsSuccess() ? std::string() : bindings.GetStatus().GetError();
             return Common::MakeSuccess( true );
         } );
    EXPECT_NE( error.find( "PostFX: BloomUpsample1" ), std::string::npos ) << error;
    EXPECT_NE( error.find( "u_Source" ), std::string::npos ) << error;
    EXPECT_NE( error.find( "Undeclared" ), std::string::npos ) << error;
}

// The access and range bound are the ones declared: sampling the mip the pass writes, or binding the sampled mip
// as storage, is refused.
TEST( RenderGraphPassBindings, AnAccessOrRangeOtherThanTheDeclaredOneFails )
{
    std::string wrongRange;
    std::string wrongAccess;
    RunBloomLikePass(
         [&]( PassContext& context, TextureRef chain, TextureRef ) -> Common::BoolResultStr
         {
             PassBindings a( context );
             a.Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 0 ), SamplerDesc::LinearClamp());
             wrongRange = a.GetStatus().IsSuccess() ? std::string() : a.GetStatus().GetError();
             PassBindings b( context );
             b.Storage( "u_Output", chain, Access::StorageWrite, 1 );
             wrongAccess = b.GetStatus().IsSuccess() ? std::string() : b.GetStatus().GetError();
             return Common::MakeSuccess( true );
         } );
    EXPECT_NE( wrongRange.find( "u_Source" ), std::string::npos ) << wrongRange;
    EXPECT_NE( wrongAccess.find( "u_Output" ), std::string::npos ) << wrongAccess;
}
#endif

// A slot name is bound once per block; the first failure is the one reported, later good entries do not hide it.
TEST( RenderGraphPassBindings, ASlotBoundTwiceFailsAndTheFirstFailureIsKept )
{
    std::string error;
    RunBloomLikePass(
         [&]( PassContext& context, TextureRef chain, TextureRef ) -> Common::BoolResultStr
         {
             PassBindings bindings( context );
             bindings.Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ), SamplerDesc::LinearClamp())
                  .Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ), SamplerDesc::LinearClamp())
                  .Storage( "u_Output", chain, Access::StorageWrite, 0 );
             error = bindings.GetStatus().IsSuccess() ? std::string() : bindings.GetStatus().GetError();
             return Common::MakeSuccess( true );
         } );
    EXPECT_NE( error.find( "u_Source" ), std::string::npos ) << error;
    EXPECT_EQ( error.find( "u_Output" ), std::string::npos ) << error;
}

// A sampled entry carries the sampler its call site named, and only a sampled entry has one: there is no
// implicit sampler on the graph route (SamplerDesc cannot be default-constructed), and distinct descriptions
// are distinct keys of the backend's sampler cache.
TEST( RenderGraphPassBindings, ASampledEntryCarriesItsSampler )
{
    static_assert( !std::is_default_constructible_v<SamplerDesc> );
    static_assert( SamplerDesc::LinearClamp().GetKey() != SamplerDesc::PointClamp().GetKey() );
    static_assert( SamplerDesc::LinearClamp().GetKey() != SamplerDesc::LinearRepeat().GetKey() );
    const Common::BoolResultStr executed = RunBloomLikePass(
         []( PassContext& context, TextureRef chain, TextureRef ) -> Common::BoolResultStr
         {
             PassBindings bindings( context );
             bindings.Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ),
                               SamplerDesc::PointClamp() )
                  .Storage( "u_Output", chain, Access::StorageWrite, 0 );
             EXPECT_TRUE( bindings.GetStatus().IsSuccess() ) << bindings.GetStatus().GetError();
             EXPECT_EQ( bindings.GetTextures().size(), 2u );
             if ( bindings.GetTextures().size() != 2u )
                 return Common::MakeSuccess( true );
             const BoundTexture& source = bindings.GetTextures()[0];
             EXPECT_TRUE( source.Sampler.has_value() );
             if ( source.Sampler )
             {
                 EXPECT_EQ( *source.Sampler, SamplerDesc::PointClamp() );
                 EXPECT_NE( *source.Sampler, SamplerDesc::LinearClamp() );
             }
             EXPECT_FALSE( bindings.GetTextures()[1].Sampler.has_value() );
             return Common::MakeSuccess( true );
         } );
    EXPECT_TRUE( executed.IsSuccess() ) << executed.GetError();
}
