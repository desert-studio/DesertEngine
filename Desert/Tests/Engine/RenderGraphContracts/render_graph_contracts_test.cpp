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
    // Amendment B: an AsyncCompute pass at position 0 that consumes an external is not a compile error. The
    // graphics pipe opens with a prologue segment (no passes) that releases the external and signals the fork;
    // the async segment waits on it. Without the fork this graph has no legal order at all.
    TEST( RenderGraphContracts, AnAsyncPassAtTheGraphStartForksFromTheGraphicsPrologue )
    {
        Builder         graph( "AsyncAtStart" );
        ExternalTexture history;
        history.Desc = Tex2D( 64, 64 );
        history.SubresourceStates.assign( 1, AccessState{} );
        const TextureRef previous  = graph.RegisterExternal( history, "History" );
        const TextureRef simulated = graph.CreateTexture( Tex2D( 64, 64 ), "Simulated" );
        ExternalTexture  output;
        output.Desc = Tex2D( 64, 64 );
        output.SubresourceStates.assign( 1, AccessState{} );
        const TextureRef out = graph.RegisterExternal( output, "Output" );

        graph.AddPass(
             "Simulate", PassFlags::Compute | PassFlags::AsyncCompute,
             [&]( PassBuilder& p )
             {
                 p.Read( previous, Access::SampledCompute );
                 p.Write( simulated, Access::StorageWrite );
             },
             Ok );
        graph.AddPass(
             "Draw", PassFlags::Compute,
             [&]( PassBuilder& p )
             {
                 p.Read( simulated, Access::SampledCompute );
                 p.Write( out, Access::StorageWrite );
             },
             Ok );

        auto compiled = graph.Compile( kEstimate, PipeCapabilities{ true } );
        ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
        const CompileResult& result = compiled.GetValue();
        ASSERT_EQ( result.FindPass( "Simulate" )->OnPipe, Pipe::AsyncCompute );

        const auto fork = std::find_if( result.Syncs.begin(), result.Syncs.end(),
                                        []( const CrossPipeSync& s ) { return s.IsFork; } );
        ASSERT_NE( fork, result.Syncs.end() );
        EXPECT_EQ( fork->SignalPosition, CrossPipeSync::kForkAtGraphStart );
        EXPECT_EQ( fork->WaitPosition, 0u );
        const uint32_t forkIndex = static_cast<uint32_t>( fork - result.Syncs.begin() );

        // The prologue comes first, has no passes, signals the fork; the async segment waits on it.
        ASSERT_GE( result.Segments.size(), 3u );
        const PipeSegment& prologue = result.Segments[0];
        EXPECT_EQ( prologue.OnPipe, Pipe::Graphics );
        EXPECT_EQ( prologue.FirstPosition, CrossPipeSync::kForkAtGraphStart );
        EXPECT_EQ( prologue.SignalSyncs, std::vector<uint32_t>{ forkIndex } );
        EXPECT_EQ( result.Segments[1].OnPipe, Pipe::AsyncCompute );
        EXPECT_EQ( result.Segments[1].WaitSyncs, std::vector<uint32_t>{ forkIndex } );

        // History moves Graphics -> AsyncCompute: released in the prologue, acquired by Simulate.
        const auto transfer =
             std::find_if( result.OwnershipTransfers.begin(), result.OwnershipTransfers.end(),
                           [&]( const QueueOwnershipTransfer& t ) { return t.Resource == previous.Index; } );
        ASSERT_NE( transfer, result.OwnershipTransfers.end() );
        EXPECT_EQ( transfer->ReleasePosition, CrossPipeSync::kForkAtGraphStart );
        EXPECT_EQ( transfer->AcquirePosition, 0u );
        EXPECT_EQ( transfer->Sync, forkIndex );
        ASSERT_EQ( result.PrologueBarriers.size(), 1u );
        EXPECT_EQ( result.PrologueBarriers[0].BarrierType, BarrierKind::OwnershipRelease );
        EXPECT_EQ( result.PrologueBarriers[0].Resource, previous.Index );
        const Barrier* acquire = FindBarrier( *result.FindPass( "Simulate" ), previous.Index );
        ASSERT_NE( acquire, nullptr );
        EXPECT_EQ( acquire->BarrierType, BarrierKind::OwnershipAcquire );
    }

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

    const QueueOwnershipTransfer* FindTransfer( const CompileResult& result, uint32_t resource )
    {
        for ( const QueueOwnershipTransfer& transfer : result.OwnershipTransfers )
            if ( transfer.Resource == resource )
                return &transfer;
        return nullptr;
    }

    // B(2): contents the destination pipe discards (a cleared attachment) are not transferred; the transition
    // starts from Undefined. The same graph loading the attachment instead transfers it.
    TEST( RenderGraphContracts, OwnershipTransferIsSkippedWhenTheDestinationDiscards )
    {
        for ( const bool load : { false, true } )
        {
            Builder         graph( "Discard" );
            ExternalTexture output;
            output.Desc = Tex2D( 64, 64 );
            output.SubresourceStates.assign( 1, AccessState{} );
            const TextureRef out = graph.RegisterExternal( output, "Output" );
            const TextureRef t   = graph.CreateTexture( Tex2D( 64, 64 ), "T" );
            const TextureRef u   = graph.CreateTexture( Tex2D( 64, 64 ), "U" );
            graph.AddPass(
                 "Fill", PassFlags::Compute | PassFlags::AsyncCompute,
                 [&]( PassBuilder& p ) { p.Write( t, Access::StorageWrite ); }, Ok );
            graph.AddPass(
                 "Reduce", PassFlags::Compute | PassFlags::AsyncCompute,
                 [&]( PassBuilder& p )
                 {
                     p.Read( t, Access::SampledCompute );
                     p.Write( u, Access::StorageWrite );
                 },
                 Ok );
            graph.AddPass(
                 "Overwrite", PassFlags::Raster | PassFlags::NeverCull, [&]( PassBuilder& p )
                 { p.ColorTarget( 0, t, load ? LoadOp::Load() : LoadOp::ClearColor( 0, 0, 0, 0 ) ); }, Ok );
            graph.AddPass(
                 "Final", PassFlags::Compute,
                 [&]( PassBuilder& p )
                 {
                     p.Read( u, Access::SampledCompute );
                     p.Write( out, Access::StorageWrite );
                 },
                 Ok );

            auto compiled = graph.Compile( kEstimate, PipeCapabilities{ true } );
            ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
            const CompileResult& result    = compiled.GetValue();
            const CompiledPass*  overwrite = result.FindPass( "Overwrite" );
            ASSERT_NE( overwrite, nullptr );
            const uint32_t overwriteAt = static_cast<uint32_t>( overwrite - result.Passes.data() );
            // Reduce read T before Overwrite writes it: a join orders them either way.
            EXPECT_TRUE( std::any_of( result.Syncs.begin(), result.Syncs.end(), [&]( const CrossPipeSync& s )
                                      { return !s.IsFork && s.WaitPosition == overwriteAt; } ) );
            ASSERT_NE( FindTransfer( result, u.Index ), nullptr ); // U is read by Final: transferred

            const Barrier* barrier = FindBarrier( *overwrite, t.Index );
            ASSERT_NE( barrier, nullptr );
            if ( load )
            {
                ASSERT_NE( FindTransfer( result, t.Index ), nullptr );
                EXPECT_EQ( barrier->BarrierType, BarrierKind::OwnershipAcquire );
                continue;
            }
            EXPECT_EQ( FindTransfer( result, t.Index ), nullptr );
            EXPECT_EQ( barrier->BarrierType, BarrierKind::Transition );
            EXPECT_TRUE( barrier->DiscardContents );
            EXPECT_EQ( barrier->Before.Layout, ImageLayout::Undefined );
            for ( const CompiledPass& pass : result.Passes )
                for ( const Barrier& release : pass.EpilogueBarriers )
                    EXPECT_NE( release.Resource, t.Index );
        }
    }

    // B(2): two raster passes on the same attachments merge into one render pass - unless a sync lies between
    // them (here the first pass signals the fork of an async pass that reads its storage output).
    TEST( RenderGraphContracts, NoRenderPassMergeAcrossASync )
    {
        for ( const bool separate : { false, true } )
        {
            Builder         graph( "Merge" );
            ExternalTexture target;
            target.Desc = Tex2D( 64, 64 );
            target.SubresourceStates.assign( 1, AccessState{} );
            ExternalTexture reduced;
            reduced.Desc = Tex2D( 64, 64 );
            reduced.SubresourceStates.assign( 1, AccessState{} );
            const TextureRef color = graph.RegisterExternal( target, "Target" );
            const TextureRef res   = graph.RegisterExternal( reduced, "Reduced" );
            const BufferRef  s     = graph.CreateBuffer( BufferDesc{ 4096 }, "S" );
            graph.AddPass(
                 "Draw1", PassFlags::Raster,
                 [&]( PassBuilder& p )
                 {
                     p.ColorTarget( 0, color, LoadOp::ClearColor( 0, 0, 0, 0 ) );
                     p.Write( s, Access::StorageWrite );
                 },
                 Ok );
            graph.AddPass(
                 "Draw2", PassFlags::Raster, [&]( PassBuilder& p ) { p.ColorTarget( 0, color, LoadOp::Load() ); },
                 Ok );
            graph.AddPass(
                 "Reduce", PassFlags::Compute | PassFlags::AsyncCompute,
                 [&]( PassBuilder& p )
                 {
                     p.Read( s, Access::StorageRead );
                     p.Write( res, Access::StorageWrite );
                 },
                 Ok );

            auto compiled = graph.Compile( kEstimate, PipeCapabilities{ separate } );
            ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
            const CompileResult& result = compiled.GetValue();
            const CompiledPass*  draw1  = result.FindPass( "Draw1" );
            const CompiledPass*  draw2  = result.FindPass( "Draw2" );
            ASSERT_TRUE( draw1 && draw2 );
            EXPECT_EQ( draw1->SignalSyncs.empty(), !separate );
            EXPECT_EQ( draw2->ContinuesRenderPass, !separate );
            EXPECT_EQ( draw1->KeepsRenderPassOpen, !separate );
        }
    }

    // B(4): a demoted AsyncCompute pass compiles to exactly the plan the graph has without the flag.
    TEST( RenderGraphContracts, ADemotedPassGetsTheSamePlanAsNoAsync )
    {
        auto compile = []( bool async, PipeCapabilities pipes )
        {
            Builder         graph( "Demote" );
            ExternalTexture output;
            output.Desc = Tex2D( 64, 64 );
            output.SubresourceStates.assign( 1, AccessState{} );
            const TextureRef out   = graph.RegisterExternal( output, "Output" );
            const TextureRef depth = graph.CreateTexture( Tex2D( 64, 64 ), "Depth" );
            const TextureRef ao    = graph.CreateTexture( Tex2D( 64, 64 ), "AO" );
            const TextureRef tmp   = graph.CreateTexture( Tex2D( 64, 64 ), "Tmp" );
            graph.AddPass(
                 "Prepass", PassFlags::Compute, [&]( PassBuilder& p ) { p.Write( depth, Access::StorageWrite ); },
                 Ok );
            graph.AddPass(
                 "SSAO", async ? PassFlags::Compute | PassFlags::AsyncCompute : PassFlags::Compute,
                 [&]( PassBuilder& p )
                 {
                     p.Read( depth, Access::SampledCompute );
                     p.Write( ao, Access::StorageWrite );
                 },
                 Ok );
            graph.AddPass(
                 "Blur", PassFlags::Compute,
                 [&]( PassBuilder& p )
                 {
                     p.Read( ao, Access::SampledCompute );
                     p.Write( tmp, Access::StorageWrite );
                 },
                 Ok );
            graph.AddPass(
                 "Lighting", PassFlags::Compute,
                 [&]( PassBuilder& p )
                 {
                     p.Read( tmp, Access::SampledCompute );
                     p.Write( out, Access::StorageWrite );
                 },
                 Ok );
            auto compiled = graph.Compile( kEstimate, pipes );
            EXPECT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
            return compiled.GetValue();
        };
        auto samePlan = []( const CompileResult& a, const CompileResult& b )
        {
            auto sameBarriers = []( const std::vector<Barrier>& x, const std::vector<Barrier>& y )
            {
                if ( x.size() != y.size() )
                    return false;
                for ( size_t i = 0; i < x.size(); ++i )
                {
                    if ( x[i].BarrierType != y[i].BarrierType || x[i].Resource != y[i].Resource ||
                         x[i].Range.BaseMip != y[i].Range.BaseMip || x[i].Range.MipCount != y[i].Range.MipCount ||
                         x[i].Range.BaseLayer != y[i].Range.BaseLayer ||
                         x[i].Range.LayerCount != y[i].Range.LayerCount || !( x[i].Before == y[i].Before ) ||
                         !( x[i].After == y[i].After ) || x[i].DiscardContents != y[i].DiscardContents )
                        return false;
                }
                return true;
            };
            if ( a.Passes.size() != b.Passes.size() || a.Syncs.size() != b.Syncs.size() ||
                 a.OwnershipTransfers.size() != b.OwnershipTransfers.size() ||
                 a.Segments.size() != b.Segments.size() || a.Aliasing.Heaps.size() != b.Aliasing.Heaps.size() ||
                 a.Aliasing.Allocations.size() != b.Aliasing.Allocations.size() ||
                 a.Aliasing.TotalPeakBytes != b.Aliasing.TotalPeakBytes ||
                 !sameBarriers( a.FinalBarriers, b.FinalBarriers ) )
                return false;
            for ( size_t i = 0; i < a.Passes.size(); ++i )
            {
                const CompiledPass& x = a.Passes[i];
                const CompiledPass& y = b.Passes[i];
                if ( x.Name != y.Name || x.OnPipe != y.OnPipe || x.WaitSyncs != y.WaitSyncs ||
                     x.SignalSyncs != y.SignalSyncs || !sameBarriers( x.Barriers, y.Barriers ) ||
                     !sameBarriers( x.EpilogueBarriers, y.EpilogueBarriers ) )
                    return false;
            }
            for ( size_t i = 0; i < a.Aliasing.Allocations.size(); ++i )
            {
                const Allocation& x = a.Aliasing.Allocations[i];
                const Allocation& y = b.Aliasing.Allocations[i];
                if ( x.Resource != y.Resource || x.Heap != y.Heap || x.Offset != y.Offset ||
                     x.AliasPredecessors != y.AliasPredecessors )
                    return false;
            }
            for ( size_t i = 0; i < a.Segments.size(); ++i )
            {
                if ( a.Segments[i].OnPipe != b.Segments[i].OnPipe ||
                     a.Segments[i].FirstPosition != b.Segments[i].FirstPosition ||
                     a.Segments[i].LastPosition != b.Segments[i].LastPosition )
                    return false;
            }
            return true;
        };

        const CompileResult plain    = compile( false, PipeCapabilities{} );
        const CompileResult demoted  = compile( true, PipeCapabilities{ false } );
        const CompileResult promoted = compile( true, PipeCapabilities{ true } );
        EXPECT_TRUE( plain.DemotedAsyncPasses.empty() );
        ASSERT_EQ( demoted.DemotedAsyncPasses.size(), 1u );
        EXPECT_TRUE( samePlan( plain, demoted ) );
        // The comparison sees a real schedule: the same graph on a separate family differs.
        EXPECT_FALSE( samePlan( plain, promoted ) );
    }

    // A(1): allocations whose memory-type sets are disjoint go to separate heaps (and never alias); sets that
    // intersect share a heap whose type set is the intersection, and alias inside it.
    TEST( RenderGraphContracts, HeapsPackAcrossTwoMemoryTypes )
    {
        class TwoTypes final : public IMemoryRequirementsProvider
        {
        public:
            Common::ResultStr<MemoryRequirements> GetTextureRequirements( const TextureDesc& desc,
                                                                          uint32_t ) const override
            {
                const uint64_t bytes = uint64_t( desc.Size.Width ) * desc.Size.Height * 8u;
                return Common::MakeSuccess(
                     MemoryRequirements{ bytes, 0x10000u, desc.Size.Width == 256 ? 0x2u : 0x1u } );
            }
            Common::ResultStr<MemoryRequirements> GetBufferRequirements( const BufferDesc&,
                                                                         uint32_t ) const override
            {
                return Common::MakeError<MemoryRequirements>( "no buffers in this test" );
            }
        };

        Builder         graph( "Heaps" );
        ExternalTexture output1;
        output1.Desc = Tex2D( 64, 64 );
        output1.SubresourceStates.assign( 1, AccessState{} );
        ExternalTexture  output2 = output1;
        const TextureRef o1      = graph.RegisterExternal( output1, "Out1" );
        const TextureRef o2      = graph.RegisterExternal( output2, "Out2" );
        const TextureRef x       = graph.CreateTexture( Tex2D( 128, 128 ), "X" ); // type 0x1
        const TextureRef y       = graph.CreateTexture( Tex2D( 256, 256 ), "Y" ); // type 0x2
        const TextureRef z       = graph.CreateTexture( Tex2D( 128, 128 ), "Z" ); // type 0x1, after X
        graph.AddPass(
             "WriteX", PassFlags::Compute, [&]( PassBuilder& p ) { p.Write( x, Access::StorageWrite ); }, Ok );
        graph.AddPass(
             "WriteY", PassFlags::Compute, [&]( PassBuilder& p ) { p.Write( y, Access::StorageWrite ); }, Ok );
        graph.AddPass(
             "ReadX", PassFlags::Compute,
             [&]( PassBuilder& p )
             {
                 p.Read( x, Access::SampledCompute );
                 p.Write( o1, Access::StorageWrite );
             },
             Ok );
        graph.AddPass(
             "ReadYWriteZ", PassFlags::Compute,
             [&]( PassBuilder& p )
             {
                 p.Read( y, Access::SampledCompute );
                 p.Write( z, Access::StorageWrite );
             },
             Ok );
        graph.AddPass(
             "ReadZ", PassFlags::Compute,
             [&]( PassBuilder& p )
             {
                 p.Read( z, Access::SampledCompute );
                 p.Write( o2, Access::StorageWrite );
             },
             Ok );

        const TwoTypes provider;
        auto           compiled = graph.Compile( provider, PipeCapabilities{ true } );
        ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
        const CompileResult& result = compiled.GetValue();
        const Allocation*    ax     = result.FindAllocation( x.Index );
        const Allocation*    ay     = result.FindAllocation( y.Index );
        const Allocation*    az     = result.FindAllocation( z.Index );
        ASSERT_TRUE( ax && ay && az );
        ASSERT_EQ( result.Aliasing.Heaps.size(), 2u );
        EXPECT_NE( ax->Heap, ay->Heap );
        EXPECT_EQ( ax->Heap, az->Heap );
        // X and Y overlap in time but live in different heaps: both start at 0.
        EXPECT_EQ( ax->Offset, 0u );
        EXPECT_EQ( ay->Offset, 0u );
        // Z starts after X ended, in X's heap: it reuses X's bytes.
        EXPECT_EQ( az->Offset, ax->Offset );
        EXPECT_EQ( az->AliasPredecessors, std::vector<uint32_t>{ x.Index } );
        const TransientHeapDesc& heapX = result.Aliasing.Heaps[ax->Heap];
        const TransientHeapDesc& heapY = result.Aliasing.Heaps[ay->Heap];
        EXPECT_EQ( heapX.MemoryTypeBits, 0x1u );
        EXPECT_EQ( heapY.MemoryTypeBits, 0x2u );
        EXPECT_EQ( heapX.Bytes, 128u * 128u * 8u );
        EXPECT_EQ( heapY.Bytes, 256u * 256u * 8u );
        EXPECT_EQ( result.Aliasing.TotalPeakBytes, heapX.Bytes + heapY.Bytes );
    }
} // namespace

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
