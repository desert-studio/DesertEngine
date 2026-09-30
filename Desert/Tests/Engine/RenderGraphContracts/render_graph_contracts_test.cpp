// RenderGraphContracts - the contract tests of RDG-CONTRACTS (transient aliasing, async compute). Written
// BEFORE the implementation, against the declarations in RDGResources.hpp / RDGCompileResult.hpp /
// RDGBackend.hpp / RDGBuilder.hpp: until A1/B1 land, this suite does not link (Compile(memory, pipes) and
// AsyncComputeFallbackLog have no definitions), and after that every test here must pass unchanged. Device-free:
// the only backend is the mock below.

#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace Desert::Graphic::RDG;
using Desert::Core::Formats::ImageFormat;

namespace
{
    Common::BoolResultStr Ok( PassContext& )
    {
        return Common::MakeSuccess( true );
    }

    // 64 KiB-aligned textures, 256 B buffers: the same estimate RenderGraphCompile uses.
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

    const FixedEstimate kEstimate;

    TextureDesc Tex2D( uint32_t width, uint32_t height )
    {
        TextureDesc desc;
        desc.Size   = { width, height, 1 };
        desc.Format = ImageFormat::RGBA16F;
        return desc;
    }

    const Barrier* FindBarrier( const CompiledPass& pass, uint32_t resource )
    {
        for ( const Barrier& barrier : pass.Barriers )
            if ( barrier.Resource == resource )
                return &barrier;
        return nullptr;
    }

    // A: two transients whose lifetimes do not overlap share bytes, and the second one's first barrier is the
    // acquire: from Undefined, discarding, with the first one's last access as its source scope.
    TEST( RenderGraphContracts, DisjointTransientsShareAnOffsetAndTheSecondGetsAnAcquire )
    {
        Builder         graph( "Aliasing" );
        ExternalTexture output;
        output.Desc = Tex2D( 64, 64 );
        output.SubresourceStates.assign( 1, AccessState{} );
        const TextureRef out = graph.RegisterExternal( output, "Output" );
        const TextureRef a   = graph.CreateTexture( Tex2D( 256, 256 ), "A" );
        const TextureRef b   = graph.CreateTexture( Tex2D( 256, 256 ), "B" );
        TextureRef       mid = graph.CreateTexture( Tex2D( 64, 64 ), "Mid" );

        graph.AddPass(
             "WriteA", PassFlags::Compute, [&]( PassBuilder& p ) { p.Write( a, Access::StorageWrite ); }, Ok );
        graph.AddPass(
             "ReadA", PassFlags::Compute,
             [&]( PassBuilder& p )
             {
                 p.Read( a, Access::SampledCompute );
                 p.Write( mid, Access::StorageWrite );
             },
             Ok );
        graph.AddPass(
             "WriteB", PassFlags::Compute,
             [&]( PassBuilder& p )
             {
                 p.Read( mid, Access::SampledCompute );
                 p.Write( b, Access::StorageWrite );
             },
             Ok );
        graph.AddPass(
             "ReadB", PassFlags::Compute,
             [&]( PassBuilder& p )
             {
                 p.Read( b, Access::SampledCompute );
                 p.Write( out, Access::StorageWrite );
             },
             Ok );

        auto compiled = graph.Compile( kEstimate );
        ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
        const CompileResult& result = compiled.GetValue();

        const Allocation* allocA = result.FindAllocation( a.Index );
        const Allocation* allocB = result.FindAllocation( b.Index );
        ASSERT_NE( allocA, nullptr );
        ASSERT_NE( allocB, nullptr );
        EXPECT_EQ( allocA->Heap, allocB->Heap );
        EXPECT_EQ( allocA->Offset, allocB->Offset );
        ASSERT_FALSE( result.Aliasing.Heaps.empty() );
        EXPECT_GE( result.Aliasing.Heaps[allocB->Heap].Bytes, allocB->Offset + allocB->Size );

        const CompiledPass* writeB = result.FindPass( "WriteB" );
        ASSERT_NE( writeB, nullptr );
        const Barrier* acquire = FindBarrier( *writeB, b.Index );
        ASSERT_NE( acquire, nullptr );
        EXPECT_EQ( acquire->BarrierType, BarrierKind::AliasAcquire );
        EXPECT_TRUE( acquire->DiscardContents );
        EXPECT_EQ( acquire->Before.Layout, ImageLayout::Undefined );
        EXPECT_NE( acquire->Before.Stages & PipelineStage_ComputeShader, 0u ); // ReadA's last access
    }

    // B: an AsyncCompute pass between graphics work gets a fork after its producer, a join before its consumer,
    // and a queue-family ownership transfer for the texture it reads and the one it writes.
    TEST( RenderGraphContracts, AsyncComputePassForksJoinsAndTransfersOwnership )
    {
        Builder         graph( "Async" );
        ExternalTexture output;
        output.Desc = Tex2D( 64, 64 );
        output.SubresourceStates.assign( 1, AccessState{} );
        const TextureRef out   = graph.RegisterExternal( output, "Output" );
        const TextureRef depth = graph.CreateTexture( Tex2D( 64, 64 ), "Depth" );
        const TextureRef ao    = graph.CreateTexture( Tex2D( 64, 64 ), "AO" );

        graph.AddPass(
             "Prepass", PassFlags::Compute, [&]( PassBuilder& p ) { p.Write( depth, Access::StorageWrite ); },
             Ok );
        graph.AddPass(
             "SSAO", PassFlags::Compute | PassFlags::AsyncCompute,
             [&]( PassBuilder& p )
             {
                 p.Read( depth, Access::SampledCompute );
                 p.Write( ao, Access::StorageWrite );
             },
             Ok );
        graph.AddPass( "Unrelated", PassFlags::Compute | PassFlags::NeverCull, []( PassBuilder& ) {}, Ok );
        graph.AddPass(
             "Lighting", PassFlags::Compute,
             [&]( PassBuilder& p )
             {
                 p.Read( ao, Access::SampledCompute );
                 p.Write( out, Access::StorageWrite );
             },
             Ok );

        auto compiled = graph.Compile( kEstimate, PipeCapabilities{ true } );
        ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
        const CompileResult& result = compiled.GetValue();

        const CompiledPass* ssao     = result.FindPass( "SSAO" );
        const CompiledPass* lighting = result.FindPass( "Lighting" );
        ASSERT_NE( ssao, nullptr );
        ASSERT_NE( lighting, nullptr );
        EXPECT_EQ( ssao->OnPipe, Pipe::AsyncCompute );
        EXPECT_EQ( result.FindPass( "Unrelated" )->OnPipe, Pipe::Graphics );
        EXPECT_TRUE( result.DemotedAsyncPasses.empty() );

        ASSERT_EQ( result.Syncs.size(), 2u );
        const auto fork = std::find_if( result.Syncs.begin(), result.Syncs.end(),
                                        []( const CrossPipeSync& s ) { return s.IsFork; } );
        const auto join = std::find_if( result.Syncs.begin(), result.Syncs.end(),
                                        []( const CrossPipeSync& s ) { return !s.IsFork; } );
        ASSERT_NE( fork, result.Syncs.end() );
        ASSERT_NE( join, result.Syncs.end() );
        EXPECT_EQ( result.Passes[fork->SignalPosition].Name, "Prepass" );
        EXPECT_EQ( result.Passes[join->WaitPosition].Name, "Lighting" );
        EXPECT_FALSE( ssao->WaitSyncs.empty() );
        EXPECT_FALSE( lighting->WaitSyncs.empty() );

        // Depth: Graphics -> AsyncCompute (release in Prepass's epilogue, acquire in SSAO's batch). AO is first
        // written on AsyncCompute (discarded, no transfer in), then AsyncCompute -> Graphics for Lighting.
        ASSERT_EQ( result.OwnershipTransfers.size(), 2u );
        for ( const QueueOwnershipTransfer& transfer : result.OwnershipTransfers )
        {
            const CompiledPass& releaser = result.Passes[transfer.ReleasePosition];
            const CompiledPass& acquirer = result.Passes[transfer.AcquirePosition];
            EXPECT_TRUE( std::any_of(
                 releaser.EpilogueBarriers.begin(), releaser.EpilogueBarriers.end(), [&]( const Barrier& b )
                 { return b.Resource == transfer.Resource && b.BarrierType == BarrierKind::OwnershipRelease; } ) );
            const Barrier* acquire = FindBarrier( acquirer, transfer.Resource );
            ASSERT_NE( acquire, nullptr );
            EXPECT_EQ( acquire->BarrierType, BarrierKind::OwnershipAcquire );
            EXPECT_EQ( acquire->SrcPipe, transfer.From );
            EXPECT_EQ( acquire->DstPipe, transfer.To );
        }

        // Aliasing sees AO live over the whole fork..join window, not just SSAO's position.
        const auto aoLife = std::find_if( result.Lifetimes.begin(), result.Lifetimes.end(),
                                          [&]( const ResourceLifetime& l ) { return l.Resource == ao.Index; } );
        ASSERT_NE( aoLife, result.Lifetimes.end() );
        EXPECT_TRUE( aoLife->UsedOnAsyncCompute );
        EXPECT_LE( aoLife->AliasFirstPosition, fork->SignalPosition );
        EXPECT_GE( aoLife->AliasLastPosition, join->WaitPosition );

        // Segments: Graphics [Prepass], AsyncCompute [SSAO], Graphics [Unrelated], Graphics [Lighting ...].
        ASSERT_GE( result.Segments.size(), 3u );
        EXPECT_EQ( result.Segments[1].OnPipe, Pipe::AsyncCompute );
    }

    // B(4): no separate compute family - everything on Graphics, no syncs, no transfers, the pass listed as
    // demoted, and the fallback log writes ONE line however many graphs demote passes.
    TEST( RenderGraphContracts, NoSeparateComputeFamilyRunsEverythingOnGraphicsAndLogsOnce )
    {
        std::vector<std::string> lines;
        AsyncComputeFallbackLog  log( [&]( std::string_view line ) { lines.emplace_back( line ); } );

        for ( int frame = 0; frame < 3; ++frame )
        {
            Builder         graph( "Fallback" );
            ExternalTexture output;
            output.Desc = Tex2D( 64, 64 );
            output.SubresourceStates.assign( 1, AccessState{} );
            const TextureRef out = graph.RegisterExternal( output, "Output" );
            graph.AddPass(
                 "SSAO", PassFlags::Compute | PassFlags::AsyncCompute,
                 [&]( PassBuilder& p ) { p.Write( out, Access::StorageWrite ); }, Ok );

            auto compiled = graph.Compile( kEstimate, PipeCapabilities{ false } );
            ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
            const CompileResult& result = compiled.GetValue();
            EXPECT_EQ( result.FindPass( "SSAO" )->OnPipe, Pipe::Graphics );
            EXPECT_TRUE( result.Syncs.empty() );
            EXPECT_TRUE( result.OwnershipTransfers.empty() );
            ASSERT_EQ( result.Segments.size(), 1u );
            EXPECT_EQ( result.Segments[0].OnPipe, Pipe::Graphics );
            ASSERT_EQ( result.DemotedAsyncPasses.size(), 1u );

            const std::string_view names[] = { "SSAO" };
            EXPECT_EQ( log.Report( names ), frame == 0 );
        }
        EXPECT_EQ( log.GetLinesLogged(), 1u );
        ASSERT_EQ( lines.size(), 1u );
        EXPECT_NE( lines[0].find( "SSAO" ), std::string::npos );
    }

    // B(1): AsyncCompute is a request on a compute pass only.
    TEST( RenderGraphContracts, AsyncComputeOnARasterPassIsADeclarationError )
    {
        Builder          graph( "BadFlag" );
        const TextureRef target = graph.CreateTexture( Tex2D( 64, 64 ), "Target" );
        graph.AddPass(
             "Raster", PassFlags::Raster | PassFlags::AsyncCompute | PassFlags::NeverCull,
             [&]( PassBuilder& p ) { p.ColorTarget( 0, target, LoadOp::DontCare() ); }, Ok );
        auto compiled = graph.Compile( kEstimate, PipeCapabilities{ true } );
        EXPECT_FALSE( compiled.IsSuccess() );
    }
} // namespace
