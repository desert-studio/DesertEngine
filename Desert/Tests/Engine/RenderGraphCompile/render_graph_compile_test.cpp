// RenderGraphCompile - the device-free core of the render graph (RDG1). Every test builds a graph the way
// a frame would, compiles it WITHOUT a device and checks the data an executor would record: which passes
// run, the barrier batch before each, load/store decisions, lifetimes and the aliasing plan. The suite
// compiles the RDG sources directly with no Vulkan include path, and the census below proves the sources
// never ask for one.

#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Graphic::RDG;
using Desert::Core::Formats::ImageFormat;

namespace
{
    namespace fs = std::filesystem;

    Common::BoolResultStr Ok( PassContext& )
    {
        return Common::MakeSuccess( true );
    }

    TextureDesc Tex2D( uint32_t width, uint32_t height, ImageFormat format, uint32_t mips = 1,
                       uint32_t layers = 1 )
    {
        TextureDesc desc;
        desc.Size   = { width, height, 1 };
        desc.Format = format;
        desc.Mips   = mips;
        desc.Layers = layers;
        return desc;
    }

    CompileResult CompileOrFail( const Builder& builder )
    {
        Common::ResultStr<CompileResult> result = builder.Compile();
        EXPECT_TRUE( result.IsSuccess() ) << result.GetError();
        return result ? result.GetValue() : CompileResult{};
    }

    std::vector<Barrier> BarriersOn( const CompiledPass* pass, uint32_t resource )
    {
        std::vector<Barrier> out;
        if ( !pass )
            return out;
        for ( const Barrier& barrier : pass->Barriers )
        {
            if ( barrier.Resource == resource )
                out.push_back( barrier );
        }
        return out;
    }

    const Barrier* BarrierOnMip( const std::vector<Barrier>& barriers, uint32_t mip )
    {
        for ( const Barrier& barrier : barriers )
        {
            if ( barrier.Range.BaseMip <= mip && mip < barrier.Range.BaseMip + barrier.Range.MipCount )
                return &barrier;
        }
        return nullptr;
    }

    bool HasEdge( const CompileResult& result, uint32_t from, uint32_t to, DependencyKind kind )
    {
        return std::any_of( result.Edges.begin(), result.Edges.end(), [&]( const DependencyEdge& edge )
                            { return edge.From == from && edge.To == to && edge.Kind == kind; } );
    }

    fs::path RepoRoot()
    {
        fs::path prefix = ".";
        for ( int up = 0; up < 6; ++up )
        {
            if ( std::ifstream( ( prefix / "Desert/Common/Source/Common/Core/DevInstruments.hpp" ).string() ) )
                return prefix;
            prefix /= "..";
        }
        return {};
    }
} // namespace

// ── Vulkan-free ─────────────────────────────────────────────────────────────────────────────────────────

// The graph core must compile and be tested without a device. The suite already builds with no Vulkan
// include path; this census makes the rule explicit per file: every #include in Graphic/RDG is either
// another RDG header, a standard header, or one of a short list of engine headers that are themselves
// Vulkan-free. A new include has to be added here on purpose.
TEST( RenderGraphCompile, SourcesIncludeNothingVulkan )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const fs::path dir = root / "Desert/Desert/Source/Engine/Graphic/RDG";
    ASSERT_TRUE( fs::is_directory( dir ) ) << dir.string();

    const std::vector<std::string> allowed = { "Engine/Graphic/RDG/", "Engine/Core/Formats/ImageFormat.hpp",
                                               "Common/Core/ResultStr.hpp", "Common/Core/DevInstruments.hpp",
                                               "spdlog/fmt/fmt.h" };
    int                            files   = 0;
    for ( const fs::directory_entry& entry : fs::directory_iterator( dir ) )
    {
        ++files;
        std::ifstream file( entry.path() );
        std::string   line;
        int           lineNumber = 0;
        while ( std::getline( file, line ) )
        {
            ++lineNumber;
            const size_t hash = line.find_first_not_of( " \t" );
            if ( hash == std::string::npos || line.compare( hash, 8, "#include" ) != 0 )
                continue;
            const size_t open  = line.find_first_of( "<\"" );
            const size_t close = line.find_first_of( ">\"", open + 1 );
            ASSERT_NE( open, std::string::npos ) << entry.path().string() << ":" << lineNumber;
            const std::string target = line.substr( open + 1, close - open - 1 );
            const bool        standard =
                 target.find( '/' ) == std::string::npos && target.find( '.' ) == std::string::npos;
            const bool listed = std::any_of( allowed.begin(), allowed.end(), [&]( const std::string& prefix )
                                             { return target.rfind( prefix, 0 ) == 0; } );
            EXPECT_TRUE( standard || listed )
                 << entry.path().filename().string() << ":" << lineNumber << " includes <" << target << ">";
        }
    }
    EXPECT_GE( files, 6 ) << "the RDG directory lost files the census expected to read";
}

// ── Culling ─────────────────────────────────────────────────────────────────────────────────────────────

TEST( RenderGraphCompile, CullsPassesNobodyConsumesAndKeepsEveryRoot )
{
    ExternalTexture backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
    ExternalTexture history;
    Builder         graph( "cull" );

    const TextureRef orphan   = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Orphan" );
    const TextureRef lit      = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Lit" );
    const TextureRef debug    = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA8F ), "Debug" );
    const TextureRef velocity = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Velocity" );
    const TextureRef back     = graph.RegisterExternal( backbuffer, "Backbuffer" );
    graph.Extract( velocity, history, Access::SampledGraphics );

    std::vector<std::string> ran;
    auto                     record = [&]( const char* name )
    {
        return [&ran, name]( PassContext& )
        {
            ran.emplace_back( name );
            return Common::MakeSuccess( true );
        };
    };
    graph.AddPass(
         "WritesOrphan", PassFlags::Raster, [&]( PassBuilder& pass )
         { pass.ColorTarget( 0, orphan, LoadOp::ClearColor( 0, 0, 0, 0 ) ); }, record( "WritesOrphan" ) );
    graph.AddPass(
         "Lighting", PassFlags::Raster, [&]( PassBuilder& pass )
         { pass.ColorTarget( 0, lit, LoadOp::ClearColor( 0, 0, 0, 1 ) ); }, record( "Lighting" ) );
    graph.AddPass(
         "DebugCapture", PassFlags::Raster | PassFlags::NeverCull, [&]( PassBuilder& pass )
         { pass.ColorTarget( 0, debug, LoadOp::DontCare() ); }, record( "DebugCapture" ) );
    graph.AddPass(
         "Velocity", PassFlags::Raster,
         [&]( PassBuilder& pass ) { pass.ColorTarget( 0, velocity, LoadOp::DontCare() ); }, record( "Velocity" ) );
    graph.AddPass(
         "Tonemap", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( lit, Access::SampledGraphics );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         record( "Tonemap" ) );

    const CompileResult result = CompileOrFail( graph );
    EXPECT_EQ( result.CulledPasses, std::vector<uint32_t>{ 0 } );
    ASSERT_EQ( result.Passes.size(), 4u );
    EXPECT_EQ( result.Passes[0].Name, "Lighting" );     // producer of a root's input
    EXPECT_EQ( result.Passes[1].Name, "DebugCapture" ); // NeverCull
    EXPECT_EQ( result.Passes[2].Name, "Velocity" );     // writes an extracted texture
    EXPECT_EQ( result.Passes[3].Name, "Tonemap" );      // writes an external texture
    // A culled pass allocates nothing.
    EXPECT_EQ( result.FindAllocation( orphan.Index ), nullptr );
    EXPECT_TRUE( std::none_of( result.Lifetimes.begin(), result.Lifetimes.end(),
                               [&]( const ResourceLifetime& l ) { return l.Resource == orphan.Index; } ) );

    ASSERT_TRUE( graph.Execute().IsSuccess() );
    EXPECT_EQ( ran, ( std::vector<std::string>{ "Lighting", "DebugCapture", "Velocity", "Tonemap" } ) );
    // The extraction target received the description and the final access.
    EXPECT_EQ( history.Desc.Format, ImageFormat::RGBA16F );
    ASSERT_EQ( history.SubresourceStates.size(), 1u );
    EXPECT_EQ( history.SubresourceStates[0], GetAccessState( Access::SampledGraphics ) );
}

TEST( RenderGraphCompile, CullingFollowsAChainBackFromTheRootOnly )
{
    ExternalTexture  backbuffer( Tex2D( 32, 32, ImageFormat::BGRA8F ), Access::None );
    Builder          graph( "chain" );
    const TextureRef a    = graph.CreateTexture( Tex2D( 32, 32, ImageFormat::RGBA16F ), "A" );
    const TextureRef b    = graph.CreateTexture( Tex2D( 32, 32, ImageFormat::RGBA16F ), "B" );
    const TextureRef dead = graph.CreateTexture( Tex2D( 32, 32, ImageFormat::RGBA16F ), "Dead" );
    const TextureRef back = graph.RegisterExternal( backbuffer, "Backbuffer" );

    graph.AddPass(
         "WriteA", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( a, Access::StorageWrite ); }, Ok );
    graph.AddPass(
         "AtoB", PassFlags::Compute,
         [&]( PassBuilder& pass )
         {
             pass.Read( a, Access::SampledCompute );
             pass.Write( b, Access::StorageWrite );
         },
         Ok );
    // Reads B but only feeds a texture nobody reads: dead, and it must not keep AtoB alive by itself.
    graph.AddPass(
         "BtoDead", PassFlags::Compute,
         [&]( PassBuilder& pass )
         {
             pass.Read( b, Access::SampledCompute );
             pass.Write( dead, Access::StorageWrite );
         },
         Ok );
    graph.AddPass(
         "Present", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( b, Access::SampledGraphics );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    EXPECT_EQ( result.CulledPasses, std::vector<uint32_t>{ 2 } );
    ASSERT_EQ( result.Passes.size(), 3u );
    EXPECT_TRUE( HasEdge( result, 0, 1, DependencyKind::ReadAfterWrite ) );
    EXPECT_TRUE( HasEdge( result, 1, 3, DependencyKind::ReadAfterWrite ) );
}

// ── Barriers ────────────────────────────────────────────────────────────────────────────────────────────

// Bloom: pass i writes mip i while sampling mip i-1 of the SAME texture. Per-subresource state is what
// makes that legal - one barrier moves mip i-1 to shader-read, another brings mip i up from undefined.
TEST( RenderGraphCompile, BloomMipChainGetsOneBarrierPerTouchedMip )
{
    constexpr uint32_t kMips = 5;
    ExternalTexture    scene( Tex2D( 256, 256, ImageFormat::RGBA16F ), Access::SampledGraphics );
    ExternalTexture    backbuffer( Tex2D( 256, 256, ImageFormat::BGRA8F ), Access::None );
    Builder            graph( "bloom" );
    const TextureRef   sceneRef = graph.RegisterExternal( scene, "Scene" );
    const TextureRef   back     = graph.RegisterExternal( backbuffer, "Backbuffer" );
    const TextureRef   bloom    = graph.CreateTexture( Tex2D( 256, 256, ImageFormat::RGBA16F, kMips ), "Bloom" );

    graph.AddPass(
         "Down0", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( sceneRef, Access::SampledGraphics );
             pass.ColorTarget( 0, bloom, LoadOp::DontCare(), 0 );
         },
         Ok );
    for ( uint32_t mip = 1; mip < kMips; ++mip )
    {
        graph.AddPass(
             "Down" + std::to_string( mip ), PassFlags::Raster,
             [&, mip]( PassBuilder& pass )
             {
                 pass.Read( bloom, Access::SampledGraphics, SubresourceRange::Mip( mip - 1 ) );
                 pass.ColorTarget( 0, bloom, LoadOp::DontCare(), mip );
             },
             Ok );
    }
    graph.AddPass(
         "Composite", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( bloom, Access::SampledGraphics );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         Ok );
    graph.Extract( back, backbuffer, Access::Present );

    const CompileResult result = CompileOrFail( graph );
    ASSERT_TRUE( result.CulledPasses.empty() );

    const AccessState target  = GetAccessState( Access::ColorTarget );
    const AccessState sampled = GetAccessState( Access::SampledGraphics );

    const std::vector<Barrier> first = BarriersOn( result.FindPass( "Down0" ), bloom.Index );
    ASSERT_EQ( first.size(), 1u );
    EXPECT_EQ( first[0].Range, ( SubresourceRange{ 0, 1, 0, 1 } ) );
    EXPECT_EQ( first[0].Before.Layout, ImageLayout::Undefined );
    EXPECT_TRUE( first[0].DiscardContents );
    // The scene was left sampled by the previous frame: no barrier on it at all.
    EXPECT_TRUE( BarriersOn( result.FindPass( "Down0" ), sceneRef.Index ).empty() );

    for ( uint32_t mip = 1; mip < kMips; ++mip )
    {
        const std::vector<Barrier> barriers =
             BarriersOn( result.FindPass( "Down" + std::to_string( mip ) ), bloom.Index );
        ASSERT_EQ( barriers.size(), 2u ) << "Down" << mip;
        const Barrier* read  = BarrierOnMip( barriers, mip - 1 );
        const Barrier* write = BarrierOnMip( barriers, mip );
        ASSERT_TRUE( read && write ) << "Down" << mip;
        EXPECT_EQ( read->Range.MipCount, 1u );
        EXPECT_EQ( read->Before, target );
        EXPECT_EQ( read->After, sampled );
        EXPECT_FALSE( read->DiscardContents );
        EXPECT_EQ( write->Range.MipCount, 1u );
        EXPECT_EQ( write->Before.Layout, ImageLayout::Undefined );
        EXPECT_EQ( write->After, target );
        EXPECT_TRUE( write->DiscardContents );
    }

    // Composite samples every mip: 0..3 are already in the sampled state (merged into the group the
    // Down passes opened), only the last mip still has to leave the attachment layout.
    const std::vector<Barrier> composite = BarriersOn( result.FindPass( "Composite" ), bloom.Index );
    ASSERT_EQ( composite.size(), 1u );
    EXPECT_EQ( composite[0].Range, ( SubresourceRange{ kMips - 1, 1, 0, 1 } ) );
    EXPECT_EQ( composite[0].Before, target );

    // Every mip is read later, so every Down pass keeps its store; the load was asked as DontCare.
    for ( const CompiledPass& pass : result.Passes )
    {
        for ( const AttachmentDecision& decision : pass.Attachments )
        {
            if ( decision.Resource != bloom.Index )
                continue;
            EXPECT_EQ( decision.Load, LoadAction::DontCare ) << pass.Name;
            EXPECT_EQ( decision.Store, StoreAction::Store ) << pass.Name;
        }
    }
    // The backbuffer leaves the graph for presentation.
    ASSERT_EQ( result.FinalBarriers.size(), 1u );
    EXPECT_EQ( result.FinalBarriers[0].Resource, back.Index );
    EXPECT_EQ( result.FinalBarriers[0].After.Layout, ImageLayout::Present );
}

// Shadow cascades are layers of one depth texture: each cascade pass brings its own layer up, and the
// lighting pass that samples all of them gets ONE barrier over the four layers.
TEST( RenderGraphCompile, CascadesAreLayersAndMergeIntoOneRange )
{
    constexpr uint32_t kCascades = 4;
    ExternalTexture    backbuffer( Tex2D( 128, 128, ImageFormat::BGRA8F ), Access::None );
    Builder            graph( "cascades" );
    const TextureRef   shadow =
         graph.CreateTexture( Tex2D( 1024, 1024, ImageFormat::DEPTH32F, 1, kCascades ), "Shadow" );
    const TextureRef back = graph.RegisterExternal( backbuffer, "Backbuffer" );

    for ( uint32_t cascade = 0; cascade < kCascades; ++cascade )
    {
        graph.AddPass(
             "Cascade" + std::to_string( cascade ), PassFlags::Raster, [&, cascade]( PassBuilder& pass )
             { pass.DepthTarget( shadow, LoadOp::ClearDepth( 1.0f ), true, cascade ); }, Ok );
    }
    graph.AddPass(
         "Lighting", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( shadow, Access::SampledGraphics );
             pass.ColorTarget( 0, back, LoadOp::ClearColor( 0, 0, 0, 1 ) );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    ASSERT_EQ( result.Passes.size(), kCascades + 1 );
    for ( uint32_t cascade = 0; cascade < kCascades; ++cascade )
    {
        const std::vector<Barrier> barriers =
             BarriersOn( result.FindPass( "Cascade" + std::to_string( cascade ) ), shadow.Index );
        ASSERT_EQ( barriers.size(), 1u ) << cascade;
        EXPECT_EQ( barriers[0].Range, ( SubresourceRange{ 0, 1, cascade, 1 } ) );
        EXPECT_EQ( barriers[0].After, GetAccessState( Access::DepthWrite ) );
        EXPECT_TRUE( barriers[0].DiscardContents );
        ASSERT_EQ( result.FindPass( "Cascade" + std::to_string( cascade ) )->Attachments.size(), 1u );
        EXPECT_EQ( result.FindPass( "Cascade" + std::to_string( cascade ) )->Attachments[0].Load,
                   LoadAction::Clear );
    }
    const std::vector<Barrier> lighting = BarriersOn( result.FindPass( "Lighting" ), shadow.Index );
    ASSERT_EQ( lighting.size(), 1u );
    EXPECT_EQ( lighting[0].Range, ( SubresourceRange{ 0, 1, 0, kCascades } ) );
    EXPECT_EQ( lighting[0].Before, GetAccessState( Access::DepthWrite ) );
    EXPECT_EQ( lighting[0].After, GetAccessState( Access::SampledGraphics ) );
}

// A write after a read must wait for the read: the barrier exists even though nothing is flushed.
TEST( RenderGraphCompile, WriteAfterReadGetsABarrier )
{
    ExternalBuffer   out( BufferDesc{ 4096 }, Access::CopyDst );
    ExternalTexture  backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
    Builder          graph( "war" );
    const TextureRef work   = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Work" );
    const BufferRef  outRef = graph.RegisterExternal( out, "Out" );
    const TextureRef back   = graph.RegisterExternal( backbuffer, "Backbuffer" );

    graph.AddPass(
         "Produce", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( work, Access::StorageWrite ); },
         Ok );
    graph.AddPass(
         "Consume", PassFlags::Compute,
         [&]( PassBuilder& pass )
         {
             pass.Read( work, Access::SampledCompute );
             pass.Write( outRef, Access::StorageWrite );
         },
         Ok );
    graph.AddPass(
         "Overwrite", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( work, Access::StorageWrite ); },
         Ok );
    graph.AddPass(
         "Show", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( work, Access::SampledGraphics );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    ASSERT_EQ( result.Passes.size(), 4u );
    EXPECT_TRUE( HasEdge( result, 1, 2, DependencyKind::WriteAfterRead ) );
    EXPECT_TRUE( HasEdge( result, 0, 2, DependencyKind::WriteAfterWrite ) );

    const std::vector<Barrier> war = BarriersOn( result.FindPass( "Overwrite" ), work.Index );
    ASSERT_EQ( war.size(), 1u );
    EXPECT_EQ( war[0].Before, GetAccessState( Access::SampledCompute ) );
    EXPECT_EQ( war[0].After, GetAccessState( Access::StorageWrite ) );
    // The external buffer arrives from an upload (CopyDst): the first write waits on that copy (WAW).
    const std::vector<Barrier> outBarrier = BarriersOn( result.FindPass( "Consume" ), outRef.Index );
    ASSERT_EQ( outBarrier.size(), 1u );
    EXPECT_EQ( outBarrier[0].Kind, ResourceKind::Buffer );
    EXPECT_EQ( outBarrier[0].Before, GetAccessState( Access::CopyDst ) );
}

// Reads in different stages merge into ONE state entered by ONE barrier before the first reader.
TEST( RenderGraphCompile, ReadsInDifferentStagesMergeIntoOneState )
{
    ExternalTexture  outA( Tex2D( 64, 64, ImageFormat::RGBA8F ), Access::None );
    ExternalTexture  outB( Tex2D( 64, 64, ImageFormat::RGBA8F ), Access::None );
    Builder          graph( "merge" );
    const TextureRef gbuffer = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "GBuffer" );
    const TextureRef a       = graph.RegisterExternal( outA, "OutA" );
    const TextureRef b       = graph.RegisterExternal( outB, "OutB" );

    graph.AddPass(
         "Base", PassFlags::Raster,
         [&]( PassBuilder& pass ) { pass.ColorTarget( 0, gbuffer, LoadOp::ClearColor( 0, 0, 0, 0 ) ); }, Ok );
    graph.AddPass(
         "RasterRead", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( gbuffer, Access::SampledGraphics );
             pass.ColorTarget( 0, a, LoadOp::DontCare() );
         },
         Ok );
    graph.AddPass(
         "ComputeRead", PassFlags::Compute,
         [&]( PassBuilder& pass )
         {
             pass.Read( gbuffer, Access::SampledCompute );
             pass.Write( b, Access::StorageWrite );
         },
         Ok );

    const CompileResult        result = CompileOrFail( graph );
    const std::vector<Barrier> first  = BarriersOn( result.FindPass( "RasterRead" ), gbuffer.Index );
    ASSERT_EQ( first.size(), 1u );
    EXPECT_EQ( first[0].Before, GetAccessState( Access::ColorTarget ) );
    EXPECT_EQ( first[0].After.Stages, kGraphicsShaderStages | PipelineStage_ComputeShader );
    EXPECT_EQ( first[0].After.Layout, ImageLayout::ShaderReadOnly );
    EXPECT_TRUE( BarriersOn( result.FindPass( "ComputeRead" ), gbuffer.Index ).empty() );
}

// ── Externals ───────────────────────────────────────────────────────────────────────────────────────────

// An external texture's per-subresource state is read at the start and written back at the end, so the
// next graph starts from the truth.
TEST( RenderGraphCompile, ExternalStatesAreReadInAndWrittenBack )
{
    ExternalTexture history( Tex2D( 64, 64, ImageFormat::RGBA16F, 2 ), Access::SampledGraphics );
    {
        Builder          graph( "frame0" );
        const TextureRef ref = graph.RegisterExternal( history, "History" );
        graph.AddPass(
             "Resolve", PassFlags::Raster,
             [&]( PassBuilder& pass )
             {
                 pass.Read( ref, Access::SampledGraphics, SubresourceRange::Mip( 0 ) );
                 pass.ColorTarget( 0, ref, LoadOp::Load(), 1 );
             },
             Ok );
        const CompileResult result = CompileOrFail( graph );
        ASSERT_EQ( result.Passes.size(), 1u );
        const std::vector<Barrier> barriers = BarriersOn( &result.Passes[0], ref.Index );
        // Mip 0 is already sampled: no barrier. Mip 1 leaves the sampled state, keeping its contents.
        ASSERT_EQ( barriers.size(), 1u );
        EXPECT_EQ( barriers[0].Range, ( SubresourceRange{ 1, 1, 0, 1 } ) );
        EXPECT_EQ( barriers[0].Before, GetAccessState( Access::SampledGraphics ) );
        EXPECT_FALSE( barriers[0].DiscardContents );
        // An external attachment is loaded and stored as asked: its contents live outside the graph.
        EXPECT_EQ( result.Passes[0].Attachments[0].Load, LoadAction::Load );
        EXPECT_EQ( result.Passes[0].Attachments[0].Store, StoreAction::Store );
        ASSERT_TRUE( graph.Execute().IsSuccess() );
    }
    ASSERT_EQ( history.SubresourceStates.size(), 2u );
    EXPECT_EQ( history.SubresourceStates[0], GetAccessState( Access::SampledGraphics ) );
    EXPECT_EQ( history.SubresourceStates[1], GetAccessState( Access::ColorTarget ) );

    // Next frame: a compute read of both mips starts from what frame 0 left behind.
    ExternalBuffer   sink( BufferDesc{ 256 }, Access::None );
    Builder          graph( "frame1" );
    const TextureRef ref     = graph.RegisterExternal( history, "History" );
    const BufferRef  sinkRef = graph.RegisterExternal( sink, "Sink" );
    graph.AddPass(
         "Reproject", PassFlags::Compute,
         [&]( PassBuilder& pass )
         {
             pass.Read( ref, Access::SampledCompute );
             pass.Write( sinkRef, Access::StorageWrite );
         },
         Ok );
    const CompileResult        result   = CompileOrFail( graph );
    const std::vector<Barrier> barriers = BarriersOn( &result.Passes[0], ref.Index );
    ASSERT_EQ( barriers.size(), 2u );
    const Barrier* mip0 = BarrierOnMip( barriers, 0 );
    const Barrier* mip1 = BarrierOnMip( barriers, 1 );
    ASSERT_TRUE( mip0 && mip1 );
    // Sampled in graphics before, now in compute: same layout, but a stage the old barrier never covered.
    EXPECT_EQ( mip0->Before, GetAccessState( Access::SampledGraphics ) );
    EXPECT_EQ( mip1->Before, GetAccessState( Access::ColorTarget ) );
    EXPECT_EQ( mip1->After, GetAccessState( Access::SampledCompute ) );
}

// ── Load / store ────────────────────────────────────────────────────────────────────────────────────────

TEST( RenderGraphCompile, LoadAndStoreBecomeDontCareWhenNothingNeedsTheContents )
{
    ExternalTexture  backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
    Builder          graph( "loadstore" );
    const TextureRef color   = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Color" );
    const TextureRef scratch = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Scratch" );
    const TextureRef depth   = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::DEPTH32F ), "Depth" );
    const TextureRef back    = graph.RegisterExternal( backbuffer, "Backbuffer" );

    graph.AddPass(
         "Base", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.ColorTarget( 0, color, LoadOp::Load() ); // first use of a transient: nothing to load
             pass.DepthTarget( depth, LoadOp::ClearDepth( 0.0f ) );
         },
         Ok );
    graph.AddPass(
         "Post", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( color, Access::SampledGraphics );
             pass.ColorTarget( 0, back, LoadOp::Load() );
             pass.ColorTarget( 1, scratch, LoadOp::ClearColor( 0, 0, 0, 0 ) );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    ASSERT_EQ( result.Passes.size(), 2u );
    const std::vector<AttachmentDecision>& base = result.Passes[0].Attachments;
    ASSERT_EQ( base.size(), 2u );
    EXPECT_EQ( base[0].Load, LoadAction::DontCare );
    EXPECT_EQ( base[0].Store, StoreAction::Store ); // Post samples it
    EXPECT_TRUE( base[1].IsDepth );
    EXPECT_EQ( base[1].Load, LoadAction::Clear );
    EXPECT_EQ( base[1].Store, StoreAction::DontCare ); // nobody reads depth afterwards

    const std::vector<AttachmentDecision>& post = result.Passes[1].Attachments;
    ASSERT_EQ( post.size(), 2u );
    EXPECT_EQ( post[0].Load, LoadAction::Load ); // external: its contents come from outside
    EXPECT_EQ( post[0].Store, StoreAction::Store );
    EXPECT_EQ( post[1].Load, LoadAction::Clear );
    EXPECT_EQ( post[1].Store, StoreAction::DontCare );
}

// ── Aliasing ────────────────────────────────────────────────────────────────────────────────────────────

TEST( RenderGraphCompile, DisjointTransientsShareMemoryAndOverlappingOnesDoNot )
{
    // 1024 x 1024 RGBA16F is exactly 8 MiB, a multiple of the 64 KiB placement.
    constexpr uint64_t kTarget = 1024ull * 1024ull * 8ull;
    ExternalTexture    out1( Tex2D( 64, 64, ImageFormat::RGBA8F ), Access::None );
    ExternalTexture    out2( Tex2D( 64, 64, ImageFormat::RGBA8F ), Access::None );

    auto build = [&]( Builder& graph, bool withLongLived, TextureRef& a, TextureRef& b, TextureRef& c )
    {
        a = graph.CreateTexture( Tex2D( 1024, 1024, ImageFormat::RGBA16F ), "A" );
        b = graph.CreateTexture( Tex2D( 1024, 1024, ImageFormat::RGBA16F ), "B" );
        c = withLongLived ? graph.CreateTexture( Tex2D( 1024, 1024, ImageFormat::RGBA16F ), "C" ) : TextureRef{};
        const TextureRef o1 = graph.RegisterExternal( out1, "Out1" );
        const TextureRef o2 = graph.RegisterExternal( out2, "Out2" );
        graph.AddPass(
             "WriteA", PassFlags::Raster,
             [&]( PassBuilder& pass )
             {
                 pass.ColorTarget( 0, a, LoadOp::DontCare() );
                 if ( withLongLived )
                     pass.ColorTarget( 1, c, LoadOp::DontCare() );
             },
             Ok );
        graph.AddPass(
             "ReadA", PassFlags::Raster,
             [&]( PassBuilder& pass )
             {
                 pass.Read( a, Access::SampledGraphics );
                 pass.ColorTarget( 0, o1, LoadOp::DontCare() );
             },
             Ok );
        graph.AddPass(
             "WriteB", PassFlags::Raster,
             [&]( PassBuilder& pass ) { pass.ColorTarget( 0, b, LoadOp::DontCare() ); }, Ok );
        graph.AddPass(
             "ReadB", PassFlags::Raster,
             [&]( PassBuilder& pass )
             {
                 pass.Read( b, Access::SampledGraphics );
                 if ( withLongLived )
                     pass.Read( c, Access::SampledGraphics );
                 pass.ColorTarget( 0, o2, LoadOp::DontCare() );
             },
             Ok );
    };

    {
        Builder    graph( "disjoint" );
        TextureRef a, b, c;
        build( graph, false, a, b, c );
        const CompileResult result = CompileOrFail( graph );
        const Allocation*   allocA = result.FindAllocation( a.Index );
        const Allocation*   allocB = result.FindAllocation( b.Index );
        ASSERT_TRUE( allocA && allocB );
        EXPECT_EQ( allocA->Size, kTarget );
        EXPECT_EQ( allocA->Offset, allocB->Offset );
        EXPECT_EQ( allocB->AliasPredecessors, std::vector<uint32_t>{ a.Index } );
        EXPECT_EQ( result.Aliasing.TotalPeakBytes, kTarget );
        EXPECT_EQ( result.Aliasing.UnaliasedBytes, 2 * kTarget );
        // B enters A's bytes: its first barrier discards and waits on A's last reader.
        const std::vector<Barrier> enter = BarriersOn( result.FindPass( "WriteB" ), b.Index );
        ASSERT_EQ( enter.size(), 1u );
        EXPECT_TRUE( enter[0].DiscardContents );
        EXPECT_EQ( enter[0].Before.Layout, ImageLayout::Undefined );
        EXPECT_EQ( enter[0].Before.Stages, GetAccessState( Access::SampledGraphics ).Stages );
    }
    {
        Builder    graph( "overlapping" );
        TextureRef a, b, c;
        build( graph, true, a, b, c );
        const CompileResult result = CompileOrFail( graph );
        const Allocation*   allocA = result.FindAllocation( a.Index );
        const Allocation*   allocB = result.FindAllocation( b.Index );
        const Allocation*   allocC = result.FindAllocation( c.Index );
        ASSERT_TRUE( allocA && allocB && allocC );
        // C lives across both: it may share with neither, while A and B still share with each other.
        EXPECT_NE( allocC->Offset, allocA->Offset );
        EXPECT_NE( allocC->Offset, allocB->Offset );
        EXPECT_EQ( allocA->Offset, allocB->Offset );
        EXPECT_EQ( result.Aliasing.TotalPeakBytes, 2 * kTarget );
        EXPECT_EQ( result.Aliasing.UnaliasedBytes, 3 * kTarget );
    }
}

// ── Declarations and PassContext ────────────────────────────────────────────────────────────────────────

TEST( RenderGraphCompile, PassContextRefusesAnUndeclaredResourceNamingPassAndResource )
{
#if DESERT_DEV_INSTRUMENTS
    ExternalTexture  backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
    Builder          graph( "undeclared" );
    const TextureRef declared   = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Declared" );
    const TextureRef undeclared = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Sneaky" );
    const TextureRef back       = graph.RegisterExternal( backbuffer, "Backbuffer" );
    graph.AddPass(
         "Fill", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.ColorTarget( 0, declared, LoadOp::DontCare() );
             pass.ColorTarget( 1, undeclared, LoadOp::DontCare() );
         },
         Ok );
    std::string wrongAccess;
    graph.AddPass(
         "Tonemap", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( declared, Access::SampledGraphics );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         [&]( PassContext& context ) -> Common::BoolResultStr
         {
             if ( !context.GetTexture( declared, Access::SampledGraphics ).IsSuccess() )
                 return Common::MakeError( "declared texture refused" );
             // Declared, but as a sampled read: asking for it as storage is refused too.
             wrongAccess = context.GetTexture( declared, Access::StorageRead ).GetError();
             Common::ResultStr<TextureBinding> sneaky = context.GetTexture( undeclared, Access::SampledGraphics );
             if ( !sneaky )
                 return Common::MakeError( sneaky.GetError() );
             return Common::MakeSuccess( true );
         } );

    const Common::BoolResultStr executed = graph.Execute();
    ASSERT_FALSE( executed.IsSuccess() );
    EXPECT_NE( executed.GetError().find( "Tonemap" ), std::string::npos ) << executed.GetError();
    EXPECT_NE( executed.GetError().find( "Sneaky" ), std::string::npos ) << executed.GetError();
    EXPECT_NE( wrongAccess.find( "StorageRead" ), std::string::npos ) << wrongAccess;
#else
    GTEST_SKIP() << "the declaration check is a development instrument, compiled out of Shipping";
#endif
}

TEST( RenderGraphCompile, MalformedDeclarationsAreRefusedWithNames )
{
    {
        ExternalTexture  backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
        Builder          graph( "garbage" );
        const TextureRef never = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "NeverWritten" );
        const TextureRef back  = graph.RegisterExternal( backbuffer, "Backbuffer" );
        graph.AddPass(
             "Reader", PassFlags::Raster,
             [&]( PassBuilder& pass )
             {
                 pass.Read( never, Access::SampledGraphics );
                 pass.ColorTarget( 0, back, LoadOp::DontCare() );
             },
             Ok );
        const Common::ResultStr<CompileResult> result = graph.Compile();
        ASSERT_FALSE( result.IsSuccess() );
        EXPECT_NE( result.GetError().find( "Reader" ), std::string::npos ) << result.GetError();
        EXPECT_NE( result.GetError().find( "NeverWritten" ), std::string::npos ) << result.GetError();
    }
    {
        Builder          graph( "conflict" );
        const TextureRef t = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Target" );
        graph.AddPass(
             "Both", PassFlags::Raster | PassFlags::NeverCull,
             [&]( PassBuilder& pass )
             {
                 pass.ColorTarget( 0, t, LoadOp::ClearColor( 0, 0, 0, 0 ) );
                 pass.Read( t, Access::SampledGraphics );
             },
             Ok );
        const Common::ResultStr<CompileResult> result = graph.Compile();
        ASSERT_FALSE( result.IsSuccess() );
        EXPECT_NE( result.GetError().find( "Both" ), std::string::npos ) << result.GetError();
        EXPECT_NE( result.GetError().find( "Target" ), std::string::npos ) << result.GetError();
    }
    {
        Builder          graph( "kinds" );
        const TextureRef t = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F, 3 ), "Chain" );
        graph.AddPass(
             "ComputeWithAttachment", PassFlags::Compute,
             [&]( PassBuilder& pass ) { pass.ColorTarget( 0, t, LoadOp::DontCare() ); }, Ok );
        const Common::ResultStr<CompileResult> result = graph.Compile();
        ASSERT_FALSE( result.IsSuccess() );
        EXPECT_NE( result.GetError().find( "ComputeWithAttachment" ), std::string::npos ) << result.GetError();
    }
    {
        Builder          graph( "range" );
        const TextureRef t = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F, 3 ), "Chain" );
        graph.AddPass(
             "OutOfRange", PassFlags::Compute,
             [&]( PassBuilder& pass ) { pass.Write( t, Access::StorageWrite, SubresourceRange::Mip( 3 ) ); }, Ok );
        const Common::ResultStr<CompileResult> result = graph.Compile();
        ASSERT_FALSE( result.IsSuccess() );
        EXPECT_NE( result.GetError().find( "mip 3" ), std::string::npos ) << result.GetError();
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
