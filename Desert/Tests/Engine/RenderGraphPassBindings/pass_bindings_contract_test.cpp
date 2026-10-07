// RenderGraphPassBindings - the contract tests of RDG-A2 (how a renderer binds a graph resource: one per-exec
// RDG::PassBindings, RDGPassBindings.hpp). Written BEFORE the implementation, against the declarations only:
// until RDG-A2-1 adds RDGPassBindings.cpp to this suite, it does not link, and after that every test here must
// pass unchanged. Device-free: the backend is the no-op mock below. The consumer-side rules (a name the shader
// does not declare, a kind that does not match the reflected descriptor type, an unfilled or doubly filled
// slot) need a shader and a device and belong to the RenderGraphVulkan suite, written with Renderer::
// DispatchCompute / DrawFullscreen in RDG-A2-1.

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGSystemTextures.hpp>

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
        PassFaultReporter& GetPassFaultReporter() override
        {
            return m_FaultReporter;
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
        Common::BoolResultStr UploadBuffer( uint32_t, std::span<const std::byte> ) override
        {
            return Common::MakeSuccess( true );
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
        PassFaultReporter       m_FaultReporter{ []( PassFaultReporter::Severity, std::string_view ) {} };
    };

    TextureDesc Tex2D( uint32_t width, uint32_t height, uint32_t mips = 1 )
    {
        TextureDesc desc;
        desc.Size   = { width, height, 1 };
        desc.Format = ImageFormat::RGBA16F;
        desc.Mips   = mips;
        return desc;
    }

    // The bloom upsample shader as its layout describes it: u_Source sampled, u_Output a storage image, 12 bytes
    // of push constants.
    ShaderBindingLayout BloomLayout()
    {
        return ShaderBindingLayout{ .ShaderName        = "BloomUpsample",
                                    .Slots             = { { "u_Source", ShaderResourceKind::SampledTexture },
                                                           { "u_Output", ShaderResourceKind::StorageTexture } },
                                    .PushConstantBytes = 12 };
    }

    // The upsample's correct block: samples mip 1 of @p chain, writes mip 0, gives 12 bytes of push constants.
    BindingBlockBuilder DeclareBloomBlock( PassBuilder& pass, TextureRef chain )
    {
        BindingBlockBuilder block = pass.Bindings( BloomLayout(), OtherRouteFill{} );
        block.Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ),
                       SamplerDesc::LinearClamp() )
             .Storage( "u_Output", chain, Access::StorageWrite, 0 )
             .PushConstantBytes( 12 );
        return block;
    }

    struct BloomRun
    {
        Common::BoolResultStr Executed;
        ExecuteReport         Report;
        bool                  ExecRan = false;
    };

    // One compute pass "PostFX: BloomUpsample1" over a two-mip chain: its setup runs @p declare (which declares
    // the pass's binding block), its exec runs @p exec. Returns the graph's verdict, its report and whether the
    // exec ran at all (a block refused by ValidatePassBindings faults the pass before its exec).
    template <class Declare, class Exec>
    BloomRun RunBloomLikePass( Declare&& declare, Exec&& exec )
    {
        ExternalTexture  chainImage( Tex2D( 64, 64, 2 ), Access::None );
        Builder          graph( "pass-bindings" );
        const TextureRef chain = graph.RegisterExternal( chainImage, "Bloom" );
        bool             ran   = false;
        graph.AddPass(
             "PostFX: BloomUpsample1", PassFlags::Compute, [&]( PassBuilder& pass ) { declare( pass, chain ); },
             [&]( PassContext& context ) -> Common::BoolResultStr
             {
                 ran = true;
                 return exec( context, chain );
             } );
        NoOpBackend                 backend;
        const Common::BoolResultStr executed = graph.Execute( backend );
        return BloomRun{ executed, graph.GetExecuteReport(), ran };
    }

    // The one fault of @p run, or a fault with an empty reason when there is not exactly one.
    PassFault OnlyFault( const BloomRun& run )
    {
        return run.Report.Faults.size() == 1u ? run.Report.Faults[0] : PassFault{};
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

// The entries declared in setup resolve to THIS execution's binding, keep the declared range and access, and name
// the slot by its shader name; the exec adds only the push constants. Storage is exactly one mip over every layer.
TEST( RenderGraphPassBindings, DeclaredEntriesResolveWithTheirRangeAndAccess )
{
    const BloomRun run = RunBloomLikePass(
         []( PassBuilder& pass, TextureRef chain ) { DeclareBloomBlock( pass, chain ); },
         []( PassContext& context, TextureRef chain ) -> Common::BoolResultStr
         {
             const float  push[3] = { 1.0f / 32.0f, 1.0f / 32.0f, 1.0f };
             PassBindings bindings( context, context.GetBindingBlock( 0 ) );
             bindings.PushConstants( push, sizeof( push ) );
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
    EXPECT_TRUE( run.Executed.IsSuccess() ) << run.Executed.GetError();
    EXPECT_TRUE( run.ExecRan );
    EXPECT_TRUE( run.Report.Faults.empty() );
}

// The exec opens only a block ITS setup declared: an index the setup never declared is refused when the block is
// opened, naming the pass - nothing of it is resolved.
TEST( RenderGraphPassBindings, ABlockTheSetupDidNotDeclareIsRefusedNamingThePass )
{
    std::string error;
    RunBloomLikePass( []( PassBuilder& pass, TextureRef chain ) { DeclareBloomBlock( pass, chain ); },
                      [&]( PassContext& context, TextureRef ) -> Common::BoolResultStr
                      {
                          PassBindings bindings( context, context.GetBindingBlock( 1 ) );
                          error =
                               bindings.GetStatus().IsSuccess() ? std::string() : bindings.GetStatus().GetError();
                          EXPECT_TRUE( bindings.GetTextures().empty() );
                          return Common::MakeSuccess( true );
                      } );
    EXPECT_NE( error.find( "PostFX: BloomUpsample1" ), std::string::npos ) << error;
    EXPECT_NE( error.find( "is not a block this pass declared" ), std::string::npos ) << error;
}

// A slot name the shader does not declare, or a kind other than the shader's, is refused BEFORE the exec runs:
// ValidatePassBindings faults the pass at Compile (Validation), naming the slot and the shader - not a missing
// barrier and a wrong picture later. (Before FAULT1 these were exec-time refusals of the name-taking API.)
TEST( RenderGraphPassBindings, ASlotOrKindTheShaderDoesNotDeclareFaultsThePassBeforeItsExec )
{
    const auto failIfRun = []( PassContext&, TextureRef ) -> Common::BoolResultStr
    { return Common::MakeError( std::string( "the exec of a refused block ran" ) ); };

    const BloomRun unknown = RunBloomLikePass(
         []( PassBuilder& pass, TextureRef chain )
         {
             pass.Bindings( BloomLayout(), OtherRouteFill{} )
                  .Sampled( "u_Sorce", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ),
                            SamplerDesc::LinearClamp() )
                  .Storage( "u_Output", chain, Access::StorageWrite, 0 )
                  .PushConstantBytes( 12 );
         },
         failIfRun );
    EXPECT_FALSE( unknown.ExecRan );
    const PassFault unknownFault = OnlyFault( unknown );
    EXPECT_EQ( unknownFault.Stage, PassFaultStage::Validation );
    EXPECT_EQ( unknownFault.PassName, "PostFX: BloomUpsample1" );
    EXPECT_NE( unknownFault.Reason.find( "'u_Sorce' is not a resource of shader 'BloomUpsample'" ),
               std::string::npos )
         << unknownFault.Reason;

    const BloomRun wrongKind = RunBloomLikePass(
         []( PassBuilder& pass, TextureRef chain )
         {
             pass.Bindings( BloomLayout(), OtherRouteFill{} )
                  .Sampled( "u_Output", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ),
                            SamplerDesc::LinearClamp() )
                  .Storage( "u_Source", chain, Access::StorageWrite, 0 )
                  .PushConstantBytes( 12 );
         },
         failIfRun );
    EXPECT_FALSE( wrongKind.ExecRan );
    const PassFault kindFault = OnlyFault( wrongKind );
    EXPECT_EQ( kindFault.Stage, PassFaultStage::Validation );
    EXPECT_NE( kindFault.Reason.find( "'u_Output' is a" ), std::string::npos ) << kindFault.Reason;
    EXPECT_NE( kindFault.Reason.find( "in shader 'BloomUpsample', bound as" ), std::string::npos )
         << kindFault.Reason;
}

// A slot name is declared once per block. A second entry for the same slot is refused at setup validation, before
// the exec (mutation: drop the kTwiceFormat check in ValidatePassBindings -> the exec runs and this goes red).
TEST( RenderGraphPassBindings, ASlotDeclaredTwiceFaultsThePassBeforeItsExec )
{
    const BloomRun run = RunBloomLikePass(
         []( PassBuilder& pass, TextureRef chain )
         {
             DeclareBloomBlock( pass, chain )
                  .Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ),
                            SamplerDesc::LinearClamp() );
         },
         []( PassContext&, TextureRef ) -> Common::BoolResultStr
         { return Common::MakeError( std::string( "the exec of a refused block ran" ) ); } );
    EXPECT_FALSE( run.ExecRan );
    const PassFault fault = OnlyFault( run );
    EXPECT_EQ( fault.Stage, PassFaultStage::Validation );
    EXPECT_NE( fault.Reason.find( "'u_Source' of shader 'BloomUpsample' is bound twice by the pass" ),
               std::string::npos )
         << fault.Reason;
}

// A sampled entry carries the sampler its declaration named, and only a sampled entry has one: there is no
// implicit sampler on the graph route (SamplerDesc cannot be default-constructed), and distinct descriptions
// are distinct keys of the backend's sampler cache.
TEST( RenderGraphPassBindings, ASampledEntryCarriesItsSampler )
{
    static_assert( !std::is_default_constructible_v<SamplerDesc> );
    static_assert( SamplerDesc::LinearClamp().GetKey() != SamplerDesc::PointClamp().GetKey() );
    static_assert( SamplerDesc::LinearClamp().GetKey() != SamplerDesc::LinearRepeat().GetKey() );
    const BloomRun run = RunBloomLikePass(
         []( PassBuilder& pass, TextureRef chain )
         {
             pass.Bindings( BloomLayout(), OtherRouteFill{} )
                  .Sampled( "u_Source", chain, Access::SampledCompute, SubresourceRange::Mip( 1 ),
                            SamplerDesc::PointClamp() )
                  .Storage( "u_Output", chain, Access::StorageWrite, 0 )
                  .PushConstantBytes( 12 );
         },
         []( PassContext& context, TextureRef ) -> Common::BoolResultStr
         {
             PassBindings bindings( context, context.GetBindingBlock( 0 ) );
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
    EXPECT_TRUE( run.Executed.IsSuccess() ) << run.Executed.GetError();
    EXPECT_TRUE( run.ExecRan );
}

// The system textures are graph resources of a fresh graph (UE: FRDGSystemTextures): registered once, valid,
// distinct, and a pass whose block declares a read of System.Black binds it like any graph texture -
// the tonemap's "bloom did not run" input, with no default texture substituted anywhere else.
TEST( RenderGraphPassBindings, SystemBlackIsAValidImportedRefOfAFreshGraph )
{
    ExternalTexture blackImage( Tex2D( 1, 1 ), Access::None );
    ExternalTexture whiteImage( Tex2D( 1, 1 ), Access::None );
    ExternalTexture blackCubeImage( Tex2D( 1, 1 ), Access::None );
    Builder         graph( "system-textures" );
    graph.SetPassCulling( false ); // the test pass reads only; nothing reads what it writes
    const SystemTextures system = RegisterSystemTextures( graph, blackImage, whiteImage, blackCubeImage );
    ASSERT_TRUE( system.Black.IsValid() );
    ASSERT_TRUE( system.White.IsValid() );
    EXPECT_NE( system.Black.Index, system.White.Index );

    bool bound = false;
    graph.AddPass(
         "Reads System.Black", PassFlags::Compute,
         [&]( PassBuilder& pass )
         {
             pass.Bindings( ShaderBindingLayout{ .ShaderName = "ReadsBlack",
                                                 .Slots = { { "u_Source", ShaderResourceKind::SampledTexture } } },
                            OtherRouteFill{} )
                  .Sampled( "u_Source", system.Black, Access::SampledCompute, SubresourceRange::Mip( 0 ),
                            SamplerDesc::LinearClamp() );
         },
         [&]( PassContext& context ) -> Common::BoolResultStr
         {
             PassBindings bindings( context, context.GetBindingBlock( 0 ) );
             EXPECT_TRUE( bindings.GetStatus().IsSuccess() ) << bindings.GetStatus().GetError();
             EXPECT_EQ( bindings.GetTextures().size(), 1u );
             if ( bindings.GetTextures().size() == 1u )
                 EXPECT_EQ( bindings.GetTextures()[0].Texture.Resource, system.Black.Index );
             bound = true;
             return Common::MakeSuccess( true );
         } );
    NoOpBackend                 backend;
    const Common::BoolResultStr executed = graph.Execute( backend );
    EXPECT_TRUE( executed.IsSuccess() ) << executed.GetError();
    EXPECT_TRUE( bound );
}
