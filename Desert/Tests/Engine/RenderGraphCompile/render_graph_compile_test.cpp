// RenderGraphCompile - the device-free core of the render graph (RDG1). Every test builds a graph the way
// a frame would, compiles it WITHOUT a device and checks the data an executor would record: which passes
// run, the barrier batch before each, load/store decisions, lifetimes and the aliasing plan. The suite
// compiles the RDG sources directly with no Vulkan include path, and the census below proves the sources
// never ask for one.

#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/DeferredFrameNodes.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <span>
#include <format>
#include <functional>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Graphic::RDG;
using Desert::Core::Formats::ImageFormat;

namespace
{
    namespace fs = std::filesystem;

    // The recorder and the expectation built from a compiled graph spell the same calls.
    constexpr std::string_view kBeginPassFormat = "BeginPass {}";
    constexpr std::string_view kBarriersFormat  = "Barriers {}";
    constexpr std::string_view kEndPassFormat   = "EndPass {}";
    constexpr std::string_view kEndGraphFormat  = "EndGraph {}";

    std::string DownPassName( auto mip )
    {
        return std::format( "Down{}", mip );
    }

    std::string CascadePassName( auto cascade )
    {
        return std::format( "Cascade{}", cascade );
    }

    Common::BoolResultStr Ok( PassContext& )
    {
        return Common::MakeSuccess( true );
    }

    constexpr uint64_t AlignUp( uint64_t value, uint64_t alignment )
    {
        return ( value + alignment - 1 ) / alignment * alignment;
    }

    // The suite's memory requirements (lead, 2026-09-27): textures placed on 64 KiB, the large-page size
    // desktop drivers use for render targets; buffers on 256 B, the largest uniform/storage offset
    // alignment in the field. The product answers from the device (RDG2 Vulkan backend).
    class FixedEstimate final : public IMemoryRequirementsProvider
    {
    public:
        Common::ResultStr<MemoryRequirements> GetTextureRequirements( const TextureDesc& desc,
                                                                      uint32_t ) const override
        {
            uint64_t bytes = 0;
            for ( uint32_t mip = 0; mip < desc.Mips; ++mip )
            {
                const uint32_t width  = std::max( 1u, desc.Size.Width >> mip );
                const uint32_t height = std::max( 1u, desc.Size.Height >> mip );
                const uint32_t depth = desc.Dim == TextureDim::Tex3D ? std::max( 1u, desc.Size.Depth >> mip ) : 1u;
                bytes += Desert::Core::Formats::CalculateImageSize( width, height, depth, desc.Format );
            }
            bytes *= desc.Layers;
            return Common::MakeSuccess(
                 MemoryRequirements{ AlignUp( bytes, kTextureAlignment ), kTextureAlignment, ~0u } );
        }
        Common::ResultStr<MemoryRequirements> GetBufferRequirements( const BufferDesc& desc,
                                                                     uint32_t ) const override
        {
            return Common::MakeSuccess(
                 MemoryRequirements{ AlignUp( desc.Bytes, kBufferAlignment ), kBufferAlignment, ~0u } );
        }

        static constexpr uint64_t kTextureAlignment = 64ull * 1024ull;
        static constexpr uint64_t kBufferAlignment  = 256ull;
    };

    const FixedEstimate kEstimate;

    // Records the call sequence Execute drives, one line per call; touches no device.
    class RecordingBackend final : public IBackend
    {
    public:
        explicit RecordingBackend( const IMemoryRequirementsProvider& memory = kEstimate ) : m_Memory( memory )
        {
        }

        BackendKind GetKind() const override
        {
            return BackendKind::Recording;
        }
        const IMemoryRequirementsProvider& GetMemoryRequirements() const override
        {
            return m_Memory;
        }
        Common::BoolResultStr BeginGraph( const GraphView& graph ) override
        {
            std::string used;
            for ( const ResourceView& view : graph.Resources )
            {
                if ( view.Used )
                    std::format_to( std::back_inserter( used ), "{}{}", used.empty() ? "" : ",", view.Name );
            }
            Calls.push_back( std::format( "BeginGraph {} [{}]", graph.Name, used ) );
            return Common::MakeSuccess( true );
        }
        void BeginPass( const CompiledPass& pass ) override
        {
            Calls.push_back( std::format( kBeginPassFormat, pass.Name ) );
        }
        void RecordBarriers( std::span<const Barrier> barriers ) override
        {
            Calls.push_back( std::format( kBarriersFormat, barriers.size() ) );
        }
        Common::BoolResultStr BeginRenderPass( const CompiledPass& pass ) override
        {
            Calls.push_back( std::format( "BeginRenderPass {}", pass.Attachments.size() ) );
            return Common::MakeSuccess( true );
        }
        void EndRenderPass() override
        {
            Calls.push_back( "EndRenderPass" );
        }
        void EndPass( const CompiledPass& pass ) override
        {
            Calls.push_back( std::format( kEndPassFormat, pass.Name ) );
        }
        Common::BoolResultStr EndGraph( std::span<const Barrier> finalBarriers ) override
        {
            Calls.push_back( std::format( kEndGraphFormat, finalBarriers.size() ) );
            return Common::MakeSuccess( true );
        }
        void AbandonGraph() override
        {
            Calls.push_back( "AbandonGraph" );
        }
        std::shared_ptr<IPhysicalTexture> GetPhysicalTexture( uint32_t ) const override
        {
            return nullptr;
        }
        std::shared_ptr<IPhysicalBuffer> GetPhysicalBuffer( uint32_t ) const override
        {
            return nullptr;
        }

        std::vector<std::string> Calls;

    private:
        const IMemoryRequirementsProvider& m_Memory;
    };

    Common::BoolResultStr ExecuteRecorded( Builder& graph )
    {
        RecordingBackend backend;
        return graph.Execute( backend );
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
        Common::ResultStr<CompileResult> result = builder.Compile( kEstimate );
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

    ASSERT_TRUE( ExecuteRecorded( graph ).IsSuccess() );
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
             DownPassName( mip ), PassFlags::Raster,
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
        const std::vector<Barrier> barriers = BarriersOn( result.FindPass( DownPassName( mip ) ), bloom.Index );
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
             CascadePassName( cascade ), PassFlags::Raster, [&, cascade]( PassBuilder& pass )
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
             BarriersOn( result.FindPass( CascadePassName( cascade ) ), shadow.Index );
        ASSERT_EQ( barriers.size(), 1u ) << cascade;
        EXPECT_EQ( barriers[0].Range, ( SubresourceRange{ 0, 1, cascade, 1 } ) );
        EXPECT_EQ( barriers[0].After, GetAccessState( Access::DepthWrite ) );
        EXPECT_TRUE( barriers[0].DiscardContents );
        ASSERT_EQ( result.FindPass( CascadePassName( cascade ) )->Attachments.size(), 1u );
        EXPECT_EQ( result.FindPass( CascadePassName( cascade ) )->Attachments[0].Load, LoadAction::Clear );
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
        ASSERT_TRUE( ExecuteRecorded( graph ).IsSuccess() );
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

    const Common::BoolResultStr executed = ExecuteRecorded( graph );
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
        const Common::ResultStr<CompileResult> result = graph.Compile( kEstimate );
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
        const Common::ResultStr<CompileResult> result = graph.Compile( kEstimate );
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
        const Common::ResultStr<CompileResult> result = graph.Compile( kEstimate );
        ASSERT_FALSE( result.IsSuccess() );
        EXPECT_NE( result.GetError().find( "ComputeWithAttachment" ), std::string::npos ) << result.GetError();
    }
    {
        Builder          graph( "range" );
        const TextureRef t = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F, 3 ), "Chain" );
        graph.AddPass(
             "OutOfRange", PassFlags::Compute,
             [&]( PassBuilder& pass ) { pass.Write( t, Access::StorageWrite, SubresourceRange::Mip( 3 ) ); }, Ok );
        const Common::ResultStr<CompileResult> result = graph.Compile( kEstimate );
        ASSERT_FALSE( result.IsSuccess() );
        EXPECT_NE( result.GetError().find( "mip 3" ), std::string::npos ) << result.GetError();
    }
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// ── Backend seam (RDG2) ─────────────────────────────────────────────────────────────────────────────────

// Execute owns the order; the backend only records. The sequence is: acquire what executed passes use,
// then per executed pass label -> its ONE barrier batch -> begin rendering (raster with attachments only)
// -> exec -> end rendering -> label close, and the final barriers last. A culled pass leaves no trace.
TEST( RenderGraphCompile, ExecuteDrivesTheBackendInPassOrder )
{
    ExternalTexture  out( Tex2D( 64, 64, ImageFormat::RGBA8F ), Access::None );
    RecordingBackend backend;
    Builder          graph( "sequence" );
    const TextureRef a    = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA8F ), "A" );
    const TextureRef dead = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA8F ), "Dead" );
    const TextureRef o    = graph.RegisterExternal( out, "Out" );
    auto             exec = [&]( const char* name )
    {
        return [&backend, name]( PassContext& ) -> Common::BoolResultStr
        {
            backend.Calls.push_back( std::string( "Exec " ) + name );
            return Common::MakeSuccess( true );
        };
    };
    graph.AddPass(
         "Clear", PassFlags::Raster, [&]( PassBuilder& pass )
         { pass.ColorTarget( 0, a, LoadOp::ClearColor( 1, 0, 0, 1 ) ); }, exec( "Clear" ) );
    graph.AddPass(
         "Unused", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( dead, Access::StorageWrite ); },
         exec( "Unused" ) );
    graph.AddPass(
         "Blur", PassFlags::Compute,
         [&]( PassBuilder& pass )
         {
             pass.Read( a, Access::SampledCompute );
             pass.Write( o, Access::StorageWrite );
         },
         exec( "Blur" ) );
    graph.Extract( o, out, Access::SampledGraphics );

    const CompileResult result = CompileOrFail( graph );
    ASSERT_EQ( result.Passes.size(), 2u );
    std::vector<std::string> expected = { "BeginGraph sequence [A,Out]" };
    for ( const CompiledPass& pass : result.Passes )
    {
        expected.push_back( std::format( kBeginPassFormat, pass.Name ) );
        if ( !pass.Barriers.empty() )
            expected.push_back( std::format( kBarriersFormat, pass.Barriers.size() ) );
        if ( pass.Name == "Clear" )
            expected.push_back( "BeginRenderPass 1" );
        expected.push_back( std::format( "Exec {}", pass.Name ) );
        if ( pass.Name == "Clear" )
            expected.push_back( "EndRenderPass" );
        expected.push_back( std::format( kEndPassFormat, pass.Name ) );
    }
    expected.push_back( std::format( kEndGraphFormat, result.FinalBarriers.size() ) );
    // Both passes need transitions and the extraction needs one: the counts above are not all zero.
    EXPECT_FALSE( result.Passes[0].Barriers.empty() );
    EXPECT_FALSE( result.Passes[1].Barriers.empty() );
    EXPECT_EQ( result.FinalBarriers.size(), 1u );

    ASSERT_TRUE( graph.Execute( backend ).IsSuccess() );
    EXPECT_EQ( backend.Calls, expected );
    EXPECT_EQ( out.SubresourceStates.front(), GetAccessState( Access::SampledGraphics ) );
}

// A failing pass stops the graph: the backend is told to abandon it, nothing after the pass is recorded
// and the error names the graph and the pass.
TEST( RenderGraphCompile, AFailingPassAbandonsTheGraph )
{
    ExternalTexture  out( Tex2D( 64, 64, ImageFormat::RGBA8F ), Access::None );
    RecordingBackend backend;
    Builder          graph( "failing" );
    const TextureRef o = graph.RegisterExternal( out, "Out" );
    graph.AddPass(
         "Broken", PassFlags::Raster, [&]( PassBuilder& pass ) { pass.ColorTarget( 0, o, LoadOp::DontCare() ); },
         []( PassContext& ) -> Common::BoolResultStr { return Common::MakeError( "pipeline missing" ); } );
    const Common::BoolResultStr executed = graph.Execute( backend );
    ASSERT_FALSE( executed.IsSuccess() );
    EXPECT_NE( executed.GetError().find( "failing" ), std::string::npos ) << executed.GetError();
    EXPECT_NE( executed.GetError().find( "Broken" ), std::string::npos ) << executed.GetError();
    ASSERT_FALSE( backend.Calls.empty() );
    EXPECT_EQ( backend.Calls.back(), "AbandonGraph" );
    EXPECT_EQ( std::count( backend.Calls.begin(), backend.Calls.end(), "EndRenderPass" ), 0 );
    // The external keeps the state it had: nothing was written back.
    EXPECT_EQ( out.SubresourceStates.front(), GetAccessState( Access::None ) );
}

// The aliasing plan takes size, alignment and memory types from the provider, asked with the usage the
// graph derived; bytes are shared only between resources whose memory type sets intersect.
TEST( RenderGraphCompile, AliasingPlanUsesTheProvidersRequirements )
{
    class TypedProvider final : public IMemoryRequirementsProvider
    {
    public:
        Common::ResultStr<MemoryRequirements> GetTextureRequirements( const TextureDesc&,
                                                                      uint32_t accessMask ) const override
        {
            Masks.push_back( accessMask );
            const bool storage = ( accessMask & ( 1u << static_cast<uint32_t>( Access::StorageWrite ) ) ) != 0;
            // 1000 bytes on a 4 KiB boundary: the plan must round the size up to the alignment.
            return Common::MakeSuccess( MemoryRequirements{ 1000, 4096, storage ? 0x1u : 0x2u } );
        }
        Common::ResultStr<MemoryRequirements> GetBufferRequirements( const BufferDesc&, uint32_t ) const override
        {
            return Common::MakeError<MemoryRequirements>( "no buffers in this test" );
        }
        mutable std::vector<uint32_t> Masks;
    };

    ExternalTexture  out1( Tex2D( 64, 64, ImageFormat::RGBA8F ), Access::None );
    ExternalTexture  out2( Tex2D( 64, 64, ImageFormat::RGBA8F ), Access::None );
    ExternalTexture  out3( Tex2D( 64, 64, ImageFormat::RGBA8F ), Access::None );
    Builder          graph( "typed" );
    const TextureRef a  = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA8F ), "A" );
    const TextureRef b  = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA8F ), "B" );
    const TextureRef c  = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA8F ), "C" );
    const TextureRef o1 = graph.RegisterExternal( out1, "Out1" );
    const TextureRef o2 = graph.RegisterExternal( out2, "Out2" );
    const TextureRef o3 = graph.RegisterExternal( out3, "Out3" );
    graph.AddPass(
         "WriteA", PassFlags::Raster,
         [&]( PassBuilder& pass ) { pass.ColorTarget( 0, a, LoadOp::ClearColor( 0, 0, 0, 0 ) ); }, Ok );
    graph.AddPass(
         "ReadA", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( a, Access::SampledGraphics );
             pass.ColorTarget( 0, o1, LoadOp::DontCare() );
         },
         Ok );
    graph.AddPass(
         "WriteB", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( b, Access::StorageWrite ); }, Ok );
    graph.AddPass(
         "ReadB", PassFlags::Compute,
         [&]( PassBuilder& pass )
         {
             pass.Read( b, Access::SampledCompute );
             pass.Write( o2, Access::StorageWrite );
         },
         Ok );
    graph.AddPass(
         "WriteC", PassFlags::Raster,
         [&]( PassBuilder& pass ) { pass.ColorTarget( 0, c, LoadOp::ClearColor( 0, 0, 0, 0 ) ); }, Ok );
    graph.AddPass(
         "ReadC", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( c, Access::SampledGraphics );
             pass.ColorTarget( 0, o3, LoadOp::DontCare() );
         },
         Ok );

    TypedProvider                    provider;
    Common::ResultStr<CompileResult> compiled = graph.Compile( provider );
    ASSERT_TRUE( compiled.IsSuccess() ) << compiled.GetError();
    const CompileResult& result = compiled.GetValue();
    const Allocation*    allocA = result.FindAllocation( a.Index );
    const Allocation*    allocB = result.FindAllocation( b.Index );
    const Allocation*    allocC = result.FindAllocation( c.Index );
    ASSERT_TRUE( allocA && allocB && allocC );
    EXPECT_EQ( allocA->Size, 4096u );
    EXPECT_EQ( allocA->Alignment, 4096u );
    EXPECT_EQ( allocA->MemoryTypeBits, 0x2u );
    EXPECT_EQ( allocB->MemoryTypeBits, 0x1u );
    // B's lifetime is disjoint from A's, but no memory type holds both: it may not take A's bytes.
    EXPECT_EQ( allocA->Offset, 0u );
    EXPECT_EQ( allocB->Offset, 4096u );
    // C shares a type with A and starts after A ended: it reuses A's bytes.
    EXPECT_EQ( allocC->Offset, 0u );
    EXPECT_EQ( allocC->AliasPredecessors, std::vector<uint32_t>{ a.Index } );
    EXPECT_EQ( result.Aliasing.TotalPeakBytes, 2u * 4096u );

    const uint32_t colourAndSampled = ( 1u << static_cast<uint32_t>( Access::ColorTarget ) ) |
                                      ( 1u << static_cast<uint32_t>( Access::SampledGraphics ) );
    ASSERT_EQ( provider.Masks.size(), 3u );
    EXPECT_EQ( provider.Masks[0], colourAndSampled ) << "the provider is asked with the derived usage";

    // A provider that cannot answer fails Compile, naming the transient.
    Builder         buffers( "buffers" );
    ExternalBuffer  sink( BufferDesc{ 256 }, Access::None );
    const BufferRef scratch = buffers.CreateBuffer( BufferDesc{ 256 }, "Scratch" );
    const BufferRef target  = buffers.RegisterExternal( sink, "Sink" );
    buffers.AddPass(
         "Fill", PassFlags::Copy, [&]( PassBuilder& pass ) { pass.Write( scratch, Access::CopyDst ); }, Ok );
    buffers.AddPass(
         "Move", PassFlags::Copy,
         [&]( PassBuilder& pass )
         {
             pass.Read( scratch, Access::CopySrc );
             pass.Write( target, Access::CopyDst );
         },
         Ok );
    Common::ResultStr<CompileResult> refused = buffers.Compile( provider );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "Scratch" ), std::string::npos ) << refused.GetError();
}

// AddLegacyPass: old code gets its images in SHADER_READ_ONLY and leaves them there; the graph neither
// opens a rendering scope for it nor lets graph passes use the legacy accesses.
TEST( RenderGraphCompile, LegacyPassSeesShaderReadOnlyAndLeavesItThere )
{
    ExternalTexture  history( Tex2D( 64, 64, ImageFormat::RGBA16F ), Access::None );
    ExternalTexture  back( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
    RecordingBackend backend;
    Builder          graph( "legacy" );
    const TextureRef scene = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Scene" );
    const TextureRef hist  = graph.RegisterExternal( history, "History" );
    const TextureRef bb    = graph.RegisterExternal( back, "Backbuffer" );
    graph.AddPass(
         "Scene", PassFlags::Raster,
         [&]( PassBuilder& pass ) { pass.ColorTarget( 0, scene, LoadOp::ClearColor( 0, 0, 0, 1 ) ); }, Ok );
    graph.AddLegacyPass( "OldBloom", { scene }, { hist }, Ok );
    graph.AddPass(
         "Tonemap", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( hist, Access::SampledGraphics );
             pass.ColorTarget( 0, bb, LoadOp::DontCare() );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    const CompiledPass* legacy = result.FindPass( "OldBloom" );
    ASSERT_NE( legacy, nullptr );
    const std::vector<Barrier> onScene = BarriersOn( legacy, scene.Index );
    const std::vector<Barrier> onHist  = BarriersOn( legacy, hist.Index );
    ASSERT_EQ( onScene.size(), 1u );
    ASSERT_EQ( onHist.size(), 1u );
    EXPECT_EQ( onScene[0].Before.Layout, ImageLayout::ColorAttachment );
    EXPECT_EQ( onScene[0].After.Layout, ImageLayout::ShaderReadOnly );
    EXPECT_EQ( onHist[0].After.Layout, ImageLayout::ShaderReadOnly );
    // After the legacy pass the texture is considered in SHADER_READ_ONLY: the reader's barrier waits on
    // everything legacy code may have done but changes no layout.
    const std::vector<Barrier> tonemap = BarriersOn( result.FindPass( "Tonemap" ), hist.Index );
    ASSERT_EQ( tonemap.size(), 1u );
    EXPECT_EQ( tonemap[0].Before, GetAccessState( Access::LegacyWrite ) );
    EXPECT_EQ( tonemap[0].Before.Layout, ImageLayout::ShaderReadOnly );
    EXPECT_EQ( tonemap[0].After.Layout, ImageLayout::ShaderReadOnly );

    ASSERT_TRUE( graph.Execute( backend ).IsSuccess() );
    const auto legacyBegin = std::find( backend.Calls.begin(), backend.Calls.end(), "BeginPass OldBloom" );
    ASSERT_NE( legacyBegin, backend.Calls.end() );
    ASSERT_GE( backend.Calls.end() - legacyBegin, 3 );
    EXPECT_EQ( *( legacyBegin + 1 ), "Barriers 2" );
    EXPECT_EQ( *( legacyBegin + 2 ), "EndPass OldBloom" ) << "a legacy pass records its own render passes";

    // The legacy accesses belong to legacy passes only, and legacy passes take nothing else.
    Builder          misuse( "misuse" );
    const TextureRef t = misuse.CreateTexture( Tex2D( 8, 8, ImageFormat::RGBA8F ), "T" );
    misuse.AddPass(
         "Sneaky", PassFlags::Raster, [&]( PassBuilder& pass ) { pass.Read( t, Access::LegacyRead ); }, Ok );
    Common::ResultStr<CompileResult> refused = misuse.Compile( kEstimate );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "Sneaky" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "raster" ), std::string::npos ) << refused.GetError();
}

// ── SceneRenderer's frame as legacy passes (RDG3) ───────────────────────────────────────────────────────

// SceneRenderer records its frame as AddLegacyPass wrappers whose real hazards the graph does not see
// (most declare nothing, some declare only the SceneColor they read). The frame is correct only if the graph
// then runs them exactly in the order they were added and drops none. Here the declarations point the wrong
// way on purpose: a later pass writes what an earlier one reads, and two passes declare nothing and feed
// nobody, which is what culling removes and a dependency sort would move.
TEST( RenderGraphCompile, LegacyPassesRunInTheOrderAddedAndNoneIsCulled )
{
    ExternalTexture          color( Tex2D( 64, 64, ImageFormat::RGBA16F ), Access::None );
    RecordingBackend         backend;
    Builder                  graph( "legacyFrame" );
    const TextureRef         scene = graph.RegisterExternal( color, "SceneColor" );
    std::vector<std::string> executed;
    const auto               body = [&executed]( const char* name )
    {
        return [&executed, name]( PassContext& ) -> Common::BoolResultStr
        {
            executed.emplace_back( name );
            return Common::MakeSuccess( true );
        };
    };
    graph.AddLegacyPass( "Clear", {}, {}, body( "Clear" ) );
    graph.AddLegacyPass( "Tonemap", { scene }, {}, body( "Tonemap" ) );
    graph.AddLegacyPass( "Composite", {}, { scene }, body( "Composite" ) );
    graph.AddLegacyPass( "Particles", {}, {}, body( "Particles" ) );
    graph.AddLegacyPass( "Bloom", {}, { scene }, body( "Bloom" ) );

    const std::vector<std::string> added  = { "Clear", "Tonemap", "Composite", "Particles", "Bloom" };
    const CompileResult            result = CompileOrFail( graph );
    std::vector<std::string>       compiled;
    for ( const CompiledPass& pass : result.Passes )
        compiled.push_back( pass.Name );
    EXPECT_EQ( compiled, added );
    ASSERT_TRUE( graph.Execute( backend ).IsSuccess() );
    EXPECT_EQ( executed, added );
}

// WHICH ORDER SceneRenderer ADDS. The pass sequence of SceneRenderer::OnUpdate, read from the source: the
// name of every AddLegacy wrapper and, for AddGraphPhasePasses, its phase selector (it adds one wrapper per
// RenderGraphBuilder::GetSortedPasses entry the selector admits, in that order). The table is the frame
// order before RDG3 (c303909f9, SceneRenderer::OnUpdate): its DESERT_PROFILE_PASS scopes and direct calls in
// sequence, ExecuteRenderGraph = every phase but the deferred overlays, then ExecuteTransparency,
// ExecuteDebugOverlay and ExecuteUI one phase each. With the test above, the frame the graph runs is this
// table: moving a pass changes the picture and has to change the table on purpose.
TEST( RenderGraphCompile, SceneRendererAddsItsPassesInTheLegacyFrameOrder )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    // OnUpdate calls one AddFrame<Pass> member per pass; they live in SceneRendererFrame*.cpp and are
    // followed in call order.
    std::string source;
    for ( const char* name : { "SceneRenderer.cpp", "SceneRendererFrameMesh.cpp", "SceneRendererFrameDeferred.cpp",
                               "SceneRendererFrameAtmosphere.cpp", "SceneRendererFramePostFX.cpp" } )
    {
        std::ifstream file( root / "Desert/Desert/Source/Engine/Graphic" / name );
        ASSERT_TRUE( file ) << name << " is gone";
        std::string line;
        while ( std::getline( file, line ) )
        {
            const size_t comment = line.find( "//" );
            std::format_to( std::back_inserter( source ), "{}\n",
                            comment == std::string::npos ? line : line.substr( 0, comment ) );
        }
    }
    const auto bodyOf = [&source]( std::string_view function ) -> std::string
    {
        // A definition returns void, or the graph handle it made (AddFrameBackdropBlur hands the UI its pyramid).
        const auto definition = [&source]( std::string_view name, size_t from )
        {
            return std::min( source.find( std::format( "void SceneRenderer::{}", name ), from ),
                             source.find( std::format( "RDG::TextureRef SceneRenderer::{}", name ), from ) );
        };
        const size_t begin = definition( std::format( "{}(", function ), 0 );
        if ( begin == std::string::npos )
            return {};
        const size_t end = definition( "", begin + 1 );
        return source.substr( begin, end == std::string::npos ? std::string::npos : end - begin );
    };
    const std::string body = bodyOf( "OnUpdate" );
    ASSERT_FALSE( body.empty() );

    const auto squeeze = []( std::string text )
    {
        text.erase(
             std::remove_if( text.begin(), text.end(), []( unsigned char c ) { return std::isspace( c ) != 0; } ),
             text.end() );
        return text;
    };
    std::vector<std::string>                          added;
    std::function<void( const std::string&, size_t )> collect = [&]( const std::string& text, size_t from )
    {
        for ( size_t at = from;; )
        {
            const size_t legacy = std::min( text.find( "AddLegacy(", at ), text.find( "graph.AddPass(", at ) );
            const size_t phases = text.find( "AddGraphPhasePasses(", at );
            const size_t frame  = text.find( "AddFrame", at );
            size_t       raster = text.find( "AddRaster(", at );
            // A system's compute nodes (AddComputeNodes): the entry names the system call that declares them.
            const size_t compute = text.find( "AddComputeNodes(", at );
            // A graph node: its name is the first string literal of the call (a std::format loop name keeps
            // its "{}", one entry per call site).
            size_t node = text.find( "graph.AddPass(", at );
            while ( node != std::string::npos && text.find( '"', node ) > text.find( ')', node ) )
                node = text.find( "graph.AddPass(", node + 1 );
            // A call names its node first (a quote before the call's first ')'); the helper's definition does not.
            while ( raster != std::string::npos && text.find( '"', raster ) > text.find( ')', raster ) )
                raster = text.find( "AddRaster(", raster + 1 );
            const size_t first = std::min( { legacy, phases, frame, raster, node, compute } );
            if ( first == std::string::npos )
                return;
            if ( first == frame )
            {
                const size_t      open   = text.find( '(', frame );
                const std::string callee = text.substr( frame, open - frame );
                const std::string called = bodyOf( callee );
                ASSERT_FALSE( called.empty() ) << "no definition of SceneRenderer::" << callee;
                collect( called, called.find( '(' ) + 1 );
                at = open + 1;
            }
            else if ( first == compute )
            {
                const size_t semi = text.find( ';', compute );
                ASSERT_NE( semi, std::string::npos );
                std::string            call   = squeeze( text.substr( compute, semi - compute ) );
                const std::string_view prefix = "AddComputeNodes(graph,textures,";
                ASSERT_EQ( call.rfind( prefix, 0 ), 0u ) << call;
                added.push_back(
                     std::format( "compute[{}]", call.substr( prefix.size(), call.size() - prefix.size() - 1 ) ) );
                at = semi + 1;
            }
            else if ( first == legacy || first == raster || first == node )
            {
                const size_t open  = text.find( '"', first );
                const size_t close = text.find( '"', open + 1 );
                ASSERT_NE( close, std::string::npos );
                added.push_back( text.substr( open + 1, close - open - 1 ) );
                at = close + 1;
            }
            else
            {
                const size_t ret  = text.find( "return ", phases );
                const size_t semi = text.find( ';', ret );
                ASSERT_NE( semi, std::string::npos );
                added.push_back( std::format( "phases[{}]", squeeze( text.substr( ret + 7, semi - ret - 7 ) ) ) );
                at = semi + 1;
            }
        }
    };
    collect( body, 0 );

    const std::vector<std::string> legacyOrder = {
         "ClearMainFramebuffer",
         "Particles: Simulate",
         "compute[clouds->DeclareShadowMapNodes()]",
         "phases[!RenderPhase::IsDeferredOverlay(phase)]",
         "Deferred: GBuffer",
         "TerrainGBuffer",
         "Deferred: DepthResolve",
         "Deferred: SSAO",
         "Deferred: RSM",
         "Deferred: GIResolve",
         "Deferred: GITemporal",
         "Deferred: Composite",
         "Deferred: Generic",
         "Deferred: Skinned",
         "Deferred: SceneCopy",
         "Deferred: SSR",
         "Deferred: SSRResolve",
         "Deferred: SSRComposite",
         "Deferred: Glass",
         "compute[sky->DeclareAtmosphereLutNodes()]",
         "compute[fog->DeclareFrameNodes()]",
         "compute[clouds->DeclareFrameNodes()]",
         "phases[phase==RenderPhase::Transparency]",
         "Debug: Overdraw",
         "Debug: Overdraw Resolve",
         "phases[phase==RenderPhase::Debug]",
         "UI: BackdropBlur{}",
         "phases[phase==RenderPhase::UI]",
         "PostFX: JumpFloodInit",
         "PostFX: JumpFloodStep{}",
         "PostFX: JumpFloodFinal",
         "PostFX: AutoExposureClear",
         "PostFX: AutoExposureHistogram",
         "PostFX: AutoExposureAverage",
         "PostFX: BloomDownsample{}",
         "PostFX: BloomUpsample{}",
         "PostFX: LightShaftMask",
         "PostFX: LightShaftBlur{}",
         "PostFX: LensFlareBright{}",
         "PostFX: LensFlareFeatures",
         "PostFX: Tonemap",
         "PostFX: FXAA",
         "PostFX: SMAAEdges",
         "PostFX: SMAAWeights",
         "PostFX: SMAABlend",
    };
    EXPECT_EQ( added, legacyOrder );

    // RDG-LEG1-L2: the deferred passes are graph nodes with declared accesses, not legacy wrappers.
    const auto declares = [&]( std::string_view function, std::initializer_list<std::string_view> needles )
    {
        const std::string text = squeeze( bodyOf( function ) );
        ASSERT_FALSE( text.empty() ) << function;
        EXPECT_EQ( text.find( "AddLegacy(" ), std::string::npos ) << function << " still adds a legacy pass";
        for ( const std::string_view needle : needles )
            EXPECT_NE( text.find( needle ), std::string::npos ) << function << " does not declare " << needle;
    };
    declares( "AddFrameClearMainFramebuffer", { "PassFlags::Raster", "ColorTarget(", "LoadOp::ClearDepth(" } );
    declares( "AddFrameSSAO", { "PassFlags::Raster", "Access::SampledGraphics", "ColorTarget(0,ao," } );
    declares( "AddFrameGIResolve", { "PassFlags::Raster", "ColorTarget(0,gather,", "ColorTarget(0,accum," } );
    declares( "AddFrameComposite", { "PassFlags::Raster", "Access::SampledGraphics", "LoadTarget(pass,target)" } );
    declares( "AddFrameSceneCopy", { "PassFlags::Raster", "Access::SampledGraphics", "ColorTarget(0,copyReads" } );
    declares( "AddFrameSSR", { "PassFlags::Compute", "Access::StorageWrite", "LoadTarget(pass,target)" } );
}

// DepthResolve is a Copy node: G-buffer depth CopySrc -> target depth CopyDst, so the graph plans the barriers
// into TRANSFER_SRC / TRANSFER_DST before it (the old AddLegacy wrapper declared nothing and got none), and the
// G-buffer depth ends the graph in the attachment layout. The same declarations compiled on recorded images:
TEST( RenderGraphCompile, DepthResolveIsACopyNodeWithCopySrcCopyDstAndPlannedBarriers )
{
    const fs::path root = RepoRoot();
    std::ifstream               file( root / "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameDeferred.cpp" );
    ASSERT_TRUE( file );
    std::string text( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
    text.erase( std::remove_if( text.begin(), text.end(), []( unsigned char c ) { return std::isspace( c ) != 0; } ),
                text.end() );
    const size_t begin = text.find( "voidSceneRenderer::AddFrameDepthResolve(" );
    ASSERT_NE( begin, std::string::npos );
    const std::string body = text.substr( begin, text.find( "voidSceneRenderer::", begin + 1 ) - begin );
    // The node is DeferredFrameNodes::AddDepthToScene's (Copy at one sample, DepthExpand at MSAA), which
    // DepthToSceneIsACopyAtOneSampleAndARasterDepthExpandAtMsaa compiles at both sample counts.
    EXPECT_NE(
         body.find( "DeferredFrameNodes::AddDepthToScene(graph,m_TargetFramebuffer->GetSpecification().Samples," ),
         std::string::npos );
    EXPECT_NE( body.find( "\"GBuffer.Depth\",DeferredFrameNodes::kGBufferDepthFinal" ), std::string::npos );
    EXPECT_EQ( body.find( "AddLegacy(" ), std::string::npos );
}

// THE POST-PROCESS AND BACKDROP PASSES ARE GRAPH NODES (RDG-LEG1-L4). Every pass SceneRendererFramePostFX.cpp
// adds is a Compute or Raster node whose setup declares what it samples and writes, so the graph places every
// barrier and opens every render pass: no AddLegacy wrapper is left, and neither the file nor the renderers it
// drives record a manual image transition, a ComputeImageBegin/EndWrite bracket or their own render pass. A
// node that declares nothing must be a culling root (NeverCull): the auto-exposure histogram clear writes only
// a buffer the graph does not import yet.
TEST( RenderGraphCompile, PostFxPassesAreRealGraphNodesWithDeclaredAccess )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const auto read = [&root]( const std::string& relative )
    {
        std::ifstream file( root / relative );
        std::string   text;
        std::string   line;
        while ( std::getline( file, line ) )
        {
            const size_t comment = line.find( "//" );
            std::format_to( std::back_inserter( text ), "{}\n",
                            comment == std::string::npos ? line : line.substr( 0, comment ) );
        }
        return text;
    };
    const std::string postFx = read( "Desert/Desert/Source/Engine/Graphic/SceneRendererFramePostFX.cpp" );
    ASSERT_FALSE( postFx.empty() ) << "SceneRendererFramePostFX.cpp is gone";

    EXPECT_EQ( postFx.find( "AddLegacy(" ), std::string::npos ) << "a PostFX/UI pass is still a legacy wrapper";

    size_t nodes = 0;
    for ( size_t at = postFx.find( "graph.AddPass(" ); at != std::string::npos;
          at        = postFx.find( "graph.AddPass(", at + 1 ) )
    {
        ++nodes;
        const size_t open  = postFx.find( '"', at );
        const size_t close = postFx.find( '"', open + 1 );
        ASSERT_NE( close, std::string::npos );
        const std::string name  = postFx.substr( open + 1, close - open - 1 );
        const size_t      setup = postFx.find( "RDG::PassBuilder&", close );
        const size_t      exec  = postFx.find( "RDG::PassContext&", close );
        ASSERT_NE( setup, std::string::npos ) << name << ": no setup lambda";
        ASSERT_NE( exec, std::string::npos ) << name << ": no exec lambda";
        const std::string flags = postFx.substr( close, setup - close );
        EXPECT_TRUE( flags.find( "RDG::PassFlags::Compute" ) != std::string::npos ||
                     flags.find( "RDG::PassFlags::Raster" ) != std::string::npos )
             << name << " is neither a Compute nor a Raster node";
        EXPECT_EQ( flags.find( "PassFlags::Legacy" ), std::string::npos ) << name << " is a legacy node";

        const std::string declarations = postFx.substr( setup, exec - setup );
        const bool        declares     = declarations.find( "pass.Read(" ) != std::string::npos ||
                              declarations.find( "pass.Write(" ) != std::string::npos ||
                              declarations.find( "pass.ColorTarget(" ) != std::string::npos ||
                              declarations.find( "ReadEach(" ) != std::string::npos;
        EXPECT_TRUE( declares || flags.find( "PassFlags::NeverCull" ) != std::string::npos )
             << name << " declares no access and is not a culling root";
        if ( flags.find( "RDG::PassFlags::Raster" ) != std::string::npos )
            EXPECT_NE( declarations.find( "pass.ColorTarget(" ), std::string::npos )
                 << name << " is a Raster node without a colour target: the graph has no render pass to open";
    }
    // JumpFlood 3, AutoExposure 3, Bloom 2, LightShafts 2, LensFlare 2, Tonemap 1, FXAA 1, SMAA 3, BackdropBlur 1.
    EXPECT_EQ( nodes, 18u );

    const std::string dir = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/";
    for ( const std::string& file :
          { std::string( "SceneRendererFramePostFX.cpp" ), dir + "JumpFloodOutlineRenderer.cpp",
            dir + "LensFlareRenderer.cpp", dir + "BackdropBlurRenderer.hpp", dir + "BloomRenderer.cpp",
            dir + "AutoExposureRenderer.cpp", dir + "LightShaftRenderer.cpp", dir + "TonemapRenderer.cpp",
            dir + "FXAARenderer.cpp", dir + "SMAARenderer.cpp" } )
    {
        const std::string text = file == "SceneRendererFramePostFX.cpp" ? postFx : read( file );
        ASSERT_FALSE( text.empty() ) << file << " is gone";
        for ( const char* manual : { "ComputeImageBeginWrite(", "ComputeImageEndWrite(", "TransitionLayout(",
                                     "BeginRenderPass(", "EndRenderPass(", "RenderPass::Create(" } )
            EXPECT_EQ( text.find( manual ), std::string::npos )
                 << file << " still records " << manual << " itself; the graph owns barriers and render passes";
    }
}

// ── Imported framebuffers and render-pass merging (RDG-LEG1-L0) ───────────────────────────────────

namespace
{
    // An engine image as ImportImage hands it over: one layout recorded, nothing known of who used it last.
    ExternalTexture Recorded( ImageFormat format, ImageLayout layout, std::vector<ImageLayout>* writtenBack )
    {
        ExternalTexture texture;
        texture.Desc              = Tex2D( 64, 64, format );
        texture.SubresourceStates = { RecordedLayoutState( layout ) };
        if ( writtenBack )
        {
            texture.RecordStates = [writtenBack]( const std::vector<AccessState>& states, bool graphEnded )
            {
                if ( !graphEnded )
                    return Common::BoolResultStr( Common::MakeSuccess( true ) );
                for ( const AccessState& state : states )
                    writtenBack->push_back( state.Layout );
                return Common::BoolResultStr( Common::MakeSuccess( true ) );
            };
        }
        return texture;
    }

    size_t CountCalls( const std::vector<std::string>& calls, std::string_view call )
    {
        return static_cast<size_t>( std::count( calls.begin(), calls.end(), call ) );
    }
} // namespace

TEST( RenderGraphCompile, ImportedFramebufferStartsFromTheRecordedLayoutsAndWritesTheFinalOnesBack )
{
    std::vector<ImageLayout> colorBack;
    std::vector<ImageLayout> depthBack;
    ExternalTexture          color = Recorded( ImageFormat::RGBA8F, ImageLayout::ShaderReadOnly, &colorBack );
    ExternalTexture depth = Recorded( ImageFormat::DEPTH32F, ImageLayout::DepthStencilAttachment, &depthBack );
    Builder         graph( "import" );
    ExternalTexture* const    colors[] = { &color };
    const ImportedFramebuffer target   = graph.ImportFramebuffer( colors, &depth, "Target" );
    ASSERT_EQ( target.Colors.size(), 1u );
    ASSERT_TRUE( target.Depth.IsValid() );
    graph.AddPass(
         "Draw", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.ColorTarget( 0, target.Colors[0], LoadOp::Load() );
             pass.DepthTarget( target.Depth, LoadOp::Load(), false );
         },
         Ok );
    const CompileResult result = CompileOrFail( graph );
    ASSERT_EQ( result.Passes.size(), 1u );
    // The first barrier leaves the RECORDED layout, waiting on everything that may have left it there.
    const std::vector<Barrier> onColor = BarriersOn( &result.Passes[0], target.Colors[0].Index );
    ASSERT_EQ( onColor.size(), 1u );
    EXPECT_EQ( onColor[0].Before, RecordedLayoutState( ImageLayout::ShaderReadOnly ) );
    EXPECT_EQ( onColor[0].After, GetAccessState( Access::ColorTarget ) );
    EXPECT_FALSE( onColor[0].DiscardContents );
    const std::vector<Barrier> onDepth = BarriersOn( &result.Passes[0], target.Depth.Index );
    ASSERT_EQ( onDepth.size(), 1u );
    EXPECT_EQ( onDepth[0].Before.Layout, ImageLayout::DepthStencilAttachment );
    EXPECT_EQ( onDepth[0].After, GetAccessState( Access::DepthRead ) );

    RecordingBackend backend;
    ASSERT_TRUE( graph.Execute( backend ).IsSuccess() );
    // Written back into the image's own record, once per subresource, and into the external.
    EXPECT_EQ( colorBack, std::vector<ImageLayout>{ ImageLayout::ColorAttachment } );
    EXPECT_EQ( depthBack, std::vector<ImageLayout>{ ImageLayout::DepthStencilReadOnly } );
    EXPECT_EQ( color.SubresourceStates[0], GetAccessState( Access::ColorTarget ) );
}

TEST( RenderGraphCompile, AFailedLayoutWriteBackFailsExecuteNamingTheTexture )
{
    ExternalTexture color   = Recorded( ImageFormat::RGBA8F, ImageLayout::ShaderReadOnly, nullptr );
    color.RecordStates = []( const std::vector<AccessState>&, bool )
    { return Common::BoolResultStr( Common::MakeError( "record gone" ) ); };
    Builder                   graph( "import" );
    ExternalTexture* const    colors[] = { &color };
    const ImportedFramebuffer target   = graph.ImportFramebuffer( colors, nullptr, "Target" );
    EXPECT_FALSE( target.Depth.IsValid() );
    graph.AddPass(
         "Draw", PassFlags::Raster,
         [&]( PassBuilder& pass ) { pass.ColorTarget( 0, target.Colors[0], LoadOp::Load() ); }, Ok );
    RecordingBackend            backend;
    const Common::BoolResultStr executed = graph.Execute( backend );
    ASSERT_FALSE( executed.IsSuccess() );
    EXPECT_NE( executed.GetError().find( "Target.Color0" ), std::string::npos ) << executed.GetError();
    EXPECT_NE( executed.GetError().find( "record gone" ), std::string::npos ) << executed.GetError();
}

TEST( RenderGraphCompile, ConsecutiveRasterPassesOnOneFramebufferShareOneRenderPass )
{
    ExternalTexture        color = Recorded( ImageFormat::RGBA8F, ImageLayout::ShaderReadOnly, nullptr );
    ExternalTexture        depth = Recorded( ImageFormat::DEPTH32F, ImageLayout::DepthStencilAttachment, nullptr );
    ExternalTexture        other = Recorded( ImageFormat::RGBA8F, ImageLayout::ShaderReadOnly, nullptr );
    Builder                graph( "merge" );
    ExternalTexture* const colors[]      = { &color };
    ExternalTexture* const otherColors[] = { &other };
    const ImportedFramebuffer main       = graph.ImportFramebuffer( colors, &depth, "Main" );
    const ImportedFramebuffer side       = graph.ImportFramebuffer( otherColors, nullptr, "Side" );
    auto                      onMain     = [&]( const LoadOp& colorLoad, const LoadOp& depthLoad )
    {
        return [&main, colorLoad, depthLoad]( PassBuilder& pass )
        {
            pass.ColorTarget( 0, main.Colors[0], colorLoad );
            pass.DepthTarget( main.Depth, depthLoad );
        };
    };
    // Opaque opens the run with CLEAR; Sky and Overlay LOAD the same targets: one begin, one end.
    graph.AddPass( "Opaque", PassFlags::Raster,
                   onMain( LoadOp::ClearColor( 0, 0, 0, 1 ), LoadOp::ClearDepth( 0 ) ), Ok );
    graph.AddPass( "Sky", PassFlags::Raster, onMain( LoadOp::Load(), LoadOp::Load() ), Ok );
    graph.AddPass( "Overlay", PassFlags::Raster, onMain( LoadOp::Load(), LoadOp::Load() ), Ok );
    // Another framebuffer splits the run.
    graph.AddPass(
         "Side", PassFlags::Raster,
         [&]( PassBuilder& pass ) { pass.ColorTarget( 0, side.Colors[0], LoadOp::Load() ); }, Ok );
    // Back on Main, loading: a new render pass (the previous one was Side's).
    graph.AddPass( "Main again", PassFlags::Raster, onMain( LoadOp::Load(), LoadOp::Load() ), Ok );
    // Same targets and LOAD, but it samples what Side drew: that barrier cannot sit inside a render pass.
    graph.AddPass(
         "Samples side", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( side.Colors[0], Access::SampledGraphics );
             pass.ColorTarget( 0, main.Colors[0], LoadOp::Load() );
             pass.DepthTarget( main.Depth, LoadOp::Load() );
         },
         Ok );
    // Same targets but CLEAR: an incompatible load splits it.
    graph.AddPass( "Clears again", PassFlags::Raster, onMain( LoadOp::ClearColor( 1, 1, 1, 1 ), LoadOp::Load() ),
                   Ok );

    const CompileResult result = CompileOrFail( graph );
    ASSERT_EQ( result.Passes.size(), 7u );
    const std::vector<bool> continues = { false, true, true, false, false, false, false };
    const std::vector<bool> keepsOpen = { true, true, false, false, false, false, false };
    for ( size_t i = 0; i < result.Passes.size(); ++i )
    {
        EXPECT_EQ( result.Passes[i].ContinuesRenderPass, continues[i] ) << result.Passes[i].Name;
        EXPECT_EQ( result.Passes[i].KeepsRenderPassOpen, keepsOpen[i] ) << result.Passes[i].Name;
    }
    // No barrier between the merged passes; the opener keeps its CLEAR.
    EXPECT_TRUE( result.Passes[1].Barriers.empty() );
    EXPECT_TRUE( result.Passes[2].Barriers.empty() );
    EXPECT_EQ( result.Passes[0].Attachments[0].Load, LoadAction::Clear );
    EXPECT_FALSE( result.Passes[5].Barriers.empty() );

    RecordingBackend backend;
    ASSERT_TRUE( graph.Execute( backend ).IsSuccess() );
    EXPECT_EQ( CountCalls( backend.Calls, "BeginRenderPass 2" ),
               4u ); // Opaque, Main again, Samples side, Clears again
    EXPECT_EQ( CountCalls( backend.Calls, "BeginRenderPass 1" ), 1u ); // Side
    EXPECT_EQ( CountCalls( backend.Calls, "EndRenderPass" ), 5u );
    // The one render pass of the run opens in Opaque and closes in Overlay.
    const auto at = [&]( std::string_view call )
    { return std::find( backend.Calls.begin(), backend.Calls.end(), call ) - backend.Calls.begin(); };
    const auto firstEnd = at( "EndRenderPass" );
    EXPECT_GT( firstEnd, at( std::format( kBeginPassFormat, "Overlay" ) ) );
    EXPECT_LT( firstEnd, at( std::format( kEndPassFormat, "Overlay" ) ) );
}

TEST( RenderGraphCompile, DepthTargetOnAnImportedDepthIsTransitionedIntoAttachmentBeforeIt )
{
    // The target depth after the fog sampled it: SHADER_READ_ONLY in its record.
    std::vector<ImageLayout>  depthBack;
    ExternalTexture           depth = Recorded( ImageFormat::DEPTH32F, ImageLayout::ShaderReadOnly, &depthBack );
    ExternalTexture           color = Recorded( ImageFormat::RGBA8F, ImageLayout::ColorAttachment, nullptr );
    Builder                   graph( "depth" );
    ExternalTexture* const    colors[] = { &color };
    const ImportedFramebuffer target   = graph.ImportFramebuffer( colors, &depth, "Target" );
    graph.AddPass(
         "Grid", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.ColorTarget( 0, target.Colors[0], LoadOp::Load() );
             pass.DepthTarget( target.Depth, LoadOp::Load() );
         },
         Ok );
    const CompileResult        result  = CompileOrFail( graph );
    const std::vector<Barrier> onDepth = BarriersOn( &result.Passes[0], target.Depth.Index );
    ASSERT_EQ( onDepth.size(), 1u );
    EXPECT_EQ( onDepth[0].Before, RecordedLayoutState( ImageLayout::ShaderReadOnly ) );
    EXPECT_EQ( onDepth[0].After, GetAccessState( Access::DepthWrite ) );
    EXPECT_EQ( onDepth[0].Range, ( SubresourceRange{ 0, 1, 0, 1 } ) );
    RecordingBackend backend;
    ASSERT_TRUE( graph.Execute( backend ).IsSuccess() );
    // The barrier batch comes before the render pass that uses the depth.
    const auto at = [&]( std::string_view call )
    { return std::find( backend.Calls.begin(), backend.Calls.end(), call ) - backend.Calls.begin(); };
    EXPECT_LT( at( std::format( kBarriersFormat, result.Passes[0].Barriers.size() ) ), at( "BeginRenderPass 2" ) );
    EXPECT_EQ( depthBack, std::vector<ImageLayout>{ ImageLayout::DepthStencilAttachment } );
}

// The deferred frame's own declarations (DeferredFrameNodes, what SceneRendererFrameDeferred.cpp declares) on the
// images as the frame hands them over: the G-buffer depth and the scene target depth both recorded in the
// attachment layout. The graph must plan TRANSFER_SRC / TRANSFER_DST before the copy (the old AddLegacy wrapper
// declared nothing, and CopyDepthImage transitioned by hand behind the graph's back), take the target depth back
// into the attachment layout before Composite, and leave the G-buffer depth where the next frame's G-buffer
// pass begins.
TEST( RenderGraphCompile, DepthResolveCopyPlansTransferBarriersAndCompositeTakesTheDepthBack )
{
    namespace Nodes = Desert::Graphic::DeferredFrameNodes;
    std::vector<ImageLayout> sourceBack;
    std::vector<ImageLayout> targetBack;
    ExternalTexture source = Recorded( ImageFormat::DEPTH32F, ImageLayout::DepthStencilAttachment, &sourceBack );
    ExternalTexture targetDepth =
         Recorded( ImageFormat::DEPTH32F, ImageLayout::DepthStencilAttachment, &targetBack );
    ExternalTexture  targetColor = Recorded( ImageFormat::RGBA8F, ImageLayout::ColorAttachment, nullptr );
    Builder          graph( "deferred" );
    const TextureRef sourceRef = graph.RegisterExternal( source, "GBuffer.Depth" );
    graph.Extract( sourceRef, source, Nodes::kGBufferDepthFinal );
    ExternalTexture* const    colors[] = { &targetColor };
    const ImportedFramebuffer target   = graph.ImportFramebuffer( colors, &targetDepth, "SceneColor" );
    graph.AddPass(
         "Deferred: DepthResolve", PassFlags::Copy,
         [&]( PassBuilder& pass ) { Nodes::DeclareDepthResolve( pass, sourceRef, target.Depth ); }, Ok );
    graph.AddPass(
         "Deferred: Composite", PassFlags::Raster, [&]( PassBuilder& pass ) { Nodes::LoadTarget( pass, target ); },
         Ok );
    const CompileResult result = CompileOrFail( graph );
    ASSERT_EQ( result.Passes.size(), 2u );
    const CompiledPass* copy      = result.FindPass( "Deferred: DepthResolve" );
    const CompiledPass* composite = result.FindPass( "Deferred: Composite" );
    ASSERT_NE( copy, nullptr );
    ASSERT_NE( composite, nullptr );

    // Before the copy: the source into TRANSFER_SRC, the target depth into TRANSFER_DST, both from the recorded
    // attachment layout.
    const std::vector<Barrier> intoSrc = BarriersOn( copy, sourceRef.Index );
    ASSERT_EQ( intoSrc.size(), 1u );
    EXPECT_EQ( intoSrc[0].Before.Layout, ImageLayout::DepthStencilAttachment );
    EXPECT_EQ( intoSrc[0].After, GetAccessState( Access::CopySrc ) );
    EXPECT_EQ( intoSrc[0].After.Layout, ImageLayout::TransferSrc );
    const std::vector<Barrier> intoDst = BarriersOn( copy, target.Depth.Index );
    ASSERT_EQ( intoDst.size(), 1u );
    EXPECT_EQ( intoDst[0].Before.Layout, ImageLayout::DepthStencilAttachment );
    EXPECT_EQ( intoDst[0].After, GetAccessState( Access::CopyDst ) );
    EXPECT_EQ( intoDst[0].After.Layout, ImageLayout::TransferDst );
    EXPECT_FALSE( intoDst[0].DiscardContents );

    // Composite's depth target takes the copied depth from TRANSFER_DST back to the attachment layout, keeping
    // the copied contents.
    const std::vector<Barrier> back = BarriersOn( composite, target.Depth.Index );
    ASSERT_EQ( back.size(), 1u );
    EXPECT_EQ( back[0].Before, GetAccessState( Access::CopyDst ) );
    EXPECT_EQ( back[0].After, GetAccessState( Access::DepthWrite ) );
    EXPECT_FALSE( back[0].DiscardContents );

    RecordingBackend backend;
    ASSERT_TRUE( graph.Execute( backend ).IsSuccess() );
    // Both depth records end in the attachment layout: the target's from Composite, the source's from the extract.
    EXPECT_EQ( targetBack, std::vector<ImageLayout>{ ImageLayout::DepthStencilAttachment } );
    EXPECT_EQ( sourceBack, std::vector<ImageLayout>{ ImageLayout::DepthStencilAttachment } );
}

// A multisampled scene target over the single-sample G-buffer cannot take a copy (Vulkan has no copy between
// sample counts): at samples > 1 the depth reaches the scene through "Deferred: DepthExpand", a Raster node whose
// only target is the scene depth and which samples the G-buffer depth; at one sample it stays the Copy node.
TEST( RenderGraphCompile, DepthToSceneIsACopyAtOneSampleAndARasterDepthExpandAtMsaa )
{
    namespace Nodes = Desert::Graphic::DeferredFrameNodes;
    for ( const uint32_t samples : { 1u, 4u } )
    {
        ExternalTexture source = Recorded( ImageFormat::DEPTH32F, ImageLayout::DepthStencilAttachment, nullptr );
        ExternalTexture target = Recorded( ImageFormat::DEPTH32F, ImageLayout::DepthStencilAttachment, nullptr );
        target.Desc.Samples    = samples;
        Builder          graph( "depth" );
        const TextureRef sourceRef = graph.RegisterExternal( source, "GBuffer.Depth" );
        const TextureRef targetRef = graph.RegisterExternal( target, "SceneColor.Depth" );
        graph.Extract( targetRef, target, Access::DepthWrite );
        Nodes::AddDepthToScene( graph, samples, sourceRef, targetRef, Ok, Ok );
        const CompileResult result = CompileOrFail( graph );
        ASSERT_EQ( result.Passes.size(), 1u ) << samples;
        const CompiledPass* copy   = result.FindPass( "Deferred: DepthResolve" );
        const CompiledPass* expand = result.FindPass( "Deferred: DepthExpand" );
        if ( samples == 1 )
        {
            ASSERT_NE( copy, nullptr );
            EXPECT_EQ( expand, nullptr );
            EXPECT_TRUE( HasFlag( copy->Flags, PassFlags::Copy ) );
            continue;
        }
        EXPECT_EQ( copy, nullptr ) << "a copy between sample counts is illegal";
        ASSERT_NE( expand, nullptr );
        EXPECT_TRUE( HasFlag( expand->Flags, PassFlags::Raster ) );
        // The G-buffer depth is sampled (SHADER_READ_ONLY) and the scene depth is the node's depth attachment.
        const std::vector<Barrier> intoRead = BarriersOn( expand, sourceRef.Index );
        ASSERT_EQ( intoRead.size(), 1u );
        EXPECT_EQ( intoRead[0].After, GetAccessState( Access::SampledGraphics ) );
        bool depthAttachment = false;
        for ( const AttachmentDecision& attachment : expand->Attachments )
            depthAttachment = depthAttachment || ( attachment.IsDepth && attachment.Resource == targetRef.Index );
        EXPECT_TRUE( depthAttachment ) << "DepthExpand does not render into the scene depth";
    }
}

// Compute passes that read scene depth sample a single-sample image. At samples > 1 "Scene: DepthResolve" is a
// Raster node that samples the multisampled scene depth and renders into the 1x SceneDepthResolved (its depth
// attachment); at one sample there is no node and the consumers read the scene depth directly.
TEST( RenderGraphCompile, SceneDepthResolveIsARasterNodeOnlyAtMsaa )
{
    namespace Nodes = Desert::Graphic::DeferredFrameNodes;
    for ( const uint32_t samples : { 1u, 4u } )
    {
        ExternalTexture scene    = Recorded( ImageFormat::DEPTH32F, ImageLayout::DepthStencilAttachment, nullptr );
        ExternalTexture resolved = Recorded( ImageFormat::DEPTH32F, ImageLayout::DepthStencilAttachment, nullptr );
        scene.Desc.Samples       = samples;
        Builder          graph( "scene-depth" );
        const TextureRef sceneRef    = graph.RegisterExternal( scene, "SceneColor.Depth" );
        const TextureRef resolvedRef = graph.RegisterExternal( resolved, "SceneDepthResolved.Depth" );
        graph.Extract( resolvedRef, resolved, Access::SampledCompute );
        Nodes::AddSceneDepthResolve( graph, samples, sceneRef, resolvedRef, Ok );
        const CompileResult result  = CompileOrFail( graph );
        const CompiledPass* resolve = result.FindPass( "Scene: DepthResolve" );
        if ( samples == 1 )
        {
            EXPECT_EQ( resolve, nullptr ) << "no resolve at one sample";
            EXPECT_TRUE( result.Passes.empty() );
            continue;
        }
        ASSERT_NE( resolve, nullptr );
        EXPECT_TRUE( HasFlag( resolve->Flags, PassFlags::Raster ) );
        const std::vector<Barrier> intoRead = BarriersOn( resolve, sceneRef.Index );
        ASSERT_EQ( intoRead.size(), 1u );
        EXPECT_EQ( intoRead[0].After, GetAccessState( Access::SampledGraphics ) );
        bool depthAttachment = false;
        for ( const AttachmentDecision& attachment : resolve->Attachments )
            depthAttachment =
                 depthAttachment || ( attachment.IsDepth && attachment.Resource == resolvedRef.Index );
        EXPECT_TRUE( depthAttachment ) << "the resolve does not render into SceneDepthResolved";
    }
}

// THE MESH AND TERRAIN PASSES ARE RASTER NODES (RDG-LEG1-L1): SceneRendererFrameMesh.cpp adds no legacy pass, each
// of its passes declares its targets (the graph opens the render pass), and none of their bodies opens or closes a
// render pass of its own.
TEST( RenderGraphCompile, MeshAndTerrainPassesAreRasterNodesTheGraphOpens )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const auto read = [&root]( const char* relative )
    {
        std::ifstream file( root / relative );
        EXPECT_TRUE( file ) << relative << " is gone";
        return std::string( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() );
    };
    const std::string frame = read( "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameMesh.cpp" );
    EXPECT_EQ( frame.find( "AddLegacy(" ), std::string::npos );
    EXPECT_NE( frame.find( "pass.ColorTarget(" ), std::string::npos );
    EXPECT_NE( frame.find( "pass.DepthTarget(" ), std::string::npos );
    // The call is matched with its whitespace collapsed: where clang-format breaks "AddRaster(" from its arguments
    // is layout, not a different call.
    std::string collapsed;
    for ( const char c : frame )
        if ( !std::isspace( static_cast<unsigned char>( c ) ) )
            collapsed.push_back( c );
    for ( const char* node : { "\"Deferred: GBuffer\"", "\"TerrainGBuffer\"", "\"Deferred: RSM\"",
                               "\"Deferred: Generic\"", "\"Deferred: Skinned\"", "\"Deferred: Glass\"",
                               "\"Debug: Overdraw\"", "\"Debug: Overdraw Resolve\"" } )
    {
        std::string call = std::format( "AddRaster(graph,{}", node );
        std::erase_if( call, []( const char c ) { return std::isspace( static_cast<unsigned char>( c ) ); } );
        EXPECT_NE( collapsed.find( call ), std::string::npos ) << node;
    }

    const auto bodyOf = []( const std::string& source, std::string_view function )
    {
        // "::Name(" and not "::Name()": a body is the same body whatever parameters it takes (RenderGlassManual
        // receives the scene-colour copy it composites over).
        const size_t begin = source.find( std::format( "::{}(", function ) );
        if ( begin == std::string::npos )
            return std::string{};
        const size_t end = source.find( "\n    }\n", begin );
        return source.substr( begin, end == std::string::npos ? std::string::npos : end - begin );
    };
    const std::pair<const char*, const char*> bodies[] = {
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererDeferred.cpp",
           "RenderGBufferManual" },
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Terrain/TerrainRenderer.cpp",
           "RenderGBufferManual" },
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererShadow.cpp", "RenderRSMManual" },
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererForward.cpp",
           "RenderGenericManual" },
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererForward.cpp",
           "RenderSkinnedManual" },
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererForward.cpp", "RenderGlassManual" },
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererDebug.cpp",
           "RenderOverdrawAccumManual" },
         { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererDebug.cpp",
           "RenderOverdrawResolveManual" } };
    for ( const auto& [file, function] : bodies )
    {
        const std::string body = bodyOf( read( file ), function );
        ASSERT_FALSE( body.empty() ) << file << ": no " << function;
        EXPECT_EQ( body.find( "BeginRenderPass(" ), std::string::npos ) << function;
        EXPECT_EQ( body.find( "EndRenderPass(" ), std::string::npos ) << function;
        EXPECT_EQ( body.find( "TransitionLayout(" ), std::string::npos ) << function;
    }
}

// THE PARTICLE SIMULATION IS A COMPUTE NODE (RDG-LEG1-L3): SceneRendererFrameAtmosphere.cpp adds it through
// graph.AddPass as Compute | NeverCull, not through a legacy wrapper. It declares no graph resource because the
// particle state lives in engine storage buffers the renderer cannot import into the graph; NeverCull is what
// keeps such a node, and a graph with only it runs it once with no barrier planned.
// A volume (an engine VulkanImage3D imported through Renderer::ImportImage) is ONE layer of Depth slices: its
// barriers cover every mip of layer 0, never Depth "layers", and the depth is carried in the description.
TEST( RenderGraphCompile, AVolumeTextureGetsBarriersOverItsMipsOfOneLayer )
{
    TextureDesc volumeDesc;
    volumeDesc.Size   = { 8, 8, 4 };
    volumeDesc.Format = ImageFormat::RGBA16F;
    volumeDesc.Mips   = 3;
    volumeDesc.Layers = 1;
    volumeDesc.Dim    = TextureDim::Tex3D;
    ExternalTexture  volume( volumeDesc, Access::None );
    ExternalBuffer   sink( BufferDesc{ 256 }, Access::None );
    Builder          graph( "volume" );
    const TextureRef ref     = graph.RegisterExternal( volume, "Volume" );
    const BufferRef  sinkRef = graph.RegisterExternal( sink, "Sink" );
    graph.AddPass(
         "Inject", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( ref, Access::StorageWrite ); }, Ok );
    graph.AddPass(
         "March", PassFlags::Compute,
         [&]( PassBuilder& pass )
         {
             pass.Read( ref, Access::SampledCompute );
             pass.Write( sinkRef, Access::StorageWrite );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    ASSERT_EQ( result.Passes.size(), 2u );
    ASSERT_EQ( volume.SubresourceStates.size(), 3u ) << "a volume has Mips x 1 subresources, not Mips x Depth";
    const std::vector<Barrier> into = BarriersOn( result.FindPass( "Inject" ), ref.Index );
    ASSERT_EQ( into.size(), 1u );
    EXPECT_EQ( into[0].Range, ( SubresourceRange{ 0, 3, 0, 1 } ) );
    EXPECT_EQ( into[0].After.Layout, GetAccessState( Access::StorageWrite ).Layout );
    const std::vector<Barrier> read = BarriersOn( result.FindPass( "March" ), ref.Index );
    ASSERT_EQ( read.size(), 1u );
    EXPECT_EQ( read[0].Kind, ResourceKind::Texture );
    EXPECT_EQ( read[0].Range, ( SubresourceRange{ 0, 3, 0, 1 } ) );
    EXPECT_EQ( read[0].Before, GetAccessState( Access::StorageWrite ) );
    EXPECT_EQ( read[0].After, GetAccessState( Access::SampledCompute ) );
    ASSERT_TRUE( ExecuteRecorded( graph ).IsSuccess() );
    EXPECT_EQ( volume.Desc.Size.Depth, 4u );
    EXPECT_EQ( volume.Desc.Dim, TextureDim::Tex3D );
}

// An engine buffer imported into the graph (Renderer::ImportBuffer): a compute write then vertex/compute and
// indirect reads plan ONE buffer barrier whose destination covers every reading stage, and Execute hands the
// final state to RecordFinalState, so the next frame's graph waits on this one's reads before it writes.
TEST( RenderGraphCompile, AnImportedBufferWrittenThenReadGetsABufferBarrierAndCarriesItsState )
{
    std::vector<AccessState> recorded;
    ExternalBuffer           particles( BufferDesc{ 4096 }, Access::None );
    particles.RecordFinalState = [&recorded]( const AccessState& state )
    {
        recorded.push_back( state );
        return Common::BoolResultStr( Common::MakeSuccess( true ) );
    };
    ExternalBuffer sink( BufferDesc{ 256 }, Access::None );
    {
        Builder         graph( "frame0" );
        const BufferRef ref     = graph.RegisterExternal( particles, "Particles" );
        const BufferRef sinkRef = graph.RegisterExternal( sink, "Sink" );
        graph.AddPass(
             "Simulate", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( ref, Access::StorageWrite ); },
             Ok );
        graph.AddPass(
             "Draw", PassFlags::Compute,
             [&]( PassBuilder& pass )
             {
                 pass.Read( ref, Access::StorageRead );
                 pass.Write( sinkRef, Access::StorageWrite );
             },
             Ok );
        graph.AddPass(
             "DrawIndirect", PassFlags::Compute,
             [&]( PassBuilder& pass )
             {
                 pass.Read( ref, Access::IndirectArgs );
                 pass.Write( sinkRef, Access::StorageWrite );
             },
             Ok );

        const CompileResult result = CompileOrFail( graph );
        ASSERT_EQ( result.Passes.size(), 3u );
        // Nothing used the buffer before: its first write waits on nothing.
        EXPECT_TRUE( BarriersOn( result.FindPass( "Simulate" ), ref.Index ).empty() );
        const std::vector<Barrier> raw = BarriersOn( result.FindPass( "Draw" ), ref.Index );
        ASSERT_EQ( raw.size(), 1u );
        EXPECT_EQ( raw[0].Kind, ResourceKind::Buffer );
        EXPECT_EQ( raw[0].Before, GetAccessState( Access::StorageWrite ) );
        EXPECT_NE( raw[0].Before.Stages & PipelineStage_ComputeShader, 0u );
        EXPECT_NE( raw[0].After.Stages & PipelineStage_VertexShader, 0u )
             << "a storage read covers the vertex stage";
        EXPECT_NE( raw[0].After.Stages & PipelineStage_ComputeShader, 0u );
        EXPECT_NE( raw[0].After.Stages & PipelineStage_DrawIndirect, 0u )
             << "the indirect read merges into the barrier before the first reader";
        EXPECT_TRUE( BarriersOn( result.FindPass( "DrawIndirect" ), ref.Index ).empty() );
        ASSERT_TRUE( ExecuteRecorded( graph ).IsSuccess() );
    }
    ASSERT_EQ( recorded.size(), 1u );
    EXPECT_NE( recorded[0].Stages & PipelineStage_DrawIndirect, 0u );
    EXPECT_TRUE( recorded[0].IsReadOnly() );
    EXPECT_EQ( particles.State, recorded[0] );

    // Next frame: the simulation's write waits on last frame's reads (WAR), from the state written back.
    Builder         graph( "frame1" );
    const BufferRef ref = graph.RegisterExternal( particles, "Particles" );
    graph.AddPass(
         "Simulate", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( ref, Access::StorageWrite ); },
         Ok );
    const CompileResult        result = CompileOrFail( graph );
    const std::vector<Barrier> war    = BarriersOn( result.FindPass( "Simulate" ), ref.Index );
    ASSERT_EQ( war.size(), 1u );
    EXPECT_EQ( war[0].Before, recorded[0] );
    EXPECT_EQ( war[0].After, GetAccessState( Access::StorageWrite ) );
}

TEST( RenderGraphCompile, AFailedBufferStateWriteBackFailsExecuteNamingTheBuffer )
{
    ExternalBuffer particles( BufferDesc{ 4096 }, Access::None );
    particles.RecordFinalState = []( const AccessState& )
    { return Common::BoolResultStr( Common::MakeError( "buffer gone" ) ); };
    Builder         graph( "import" );
    const BufferRef ref = graph.RegisterExternal( particles, "Particles" );
    graph.AddPass(
         "Simulate", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( ref, Access::StorageWrite ); },
         Ok );
    const Common::BoolResultStr executed = ExecuteRecorded( graph );
    ASSERT_FALSE( executed.IsSuccess() );
    EXPECT_NE( executed.GetError().find( "Particles" ), std::string::npos ) << executed.GetError();
    EXPECT_NE( executed.GetError().find( "buffer gone" ), std::string::npos ) << executed.GetError();
}

TEST( RenderGraphCompile, ParticleSimulationIsAComputeNodeTheGraphKeeps )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    std::ifstream file( root / "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameAtmosphere.cpp" );
    ASSERT_TRUE( file );
    std::string text( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
    text.erase(
         std::remove_if( text.begin(), text.end(), []( unsigned char c ) { return std::isspace( c ) != 0; } ),
         text.end() );
    const size_t begin = text.find( "voidSceneRenderer::AddFrameParticlesSimulate(" );
    ASSERT_NE( begin, std::string::npos );
    const std::string body = text.substr( begin, text.find( "voidSceneRenderer::", begin + 1 ) - begin );
    EXPECT_EQ( body.find( "AddLegacy(" ), std::string::npos );
    EXPECT_NE(
         body.find( "graph.AddPass(\"Particles:Simulate\",RDG::PassFlags::Compute|RDG::PassFlags::NeverCull" ),
         std::string::npos );

    // The node declares the emitters' buffers: imported through Renderer::ImportBuffer, written StorageWrite.
    EXPECT_NE( body.find( "particles->ImportSimulationBuffers(graph)" ), std::string::npos )
         << "the simulation node does not import the emitters' buffers";
    EXPECT_NE( body.find( "pass.Write(buffer,RDG::Access::StorageWrite)" ), std::string::npos )
         << "the simulation node does not declare its writes";
    EXPECT_EQ( body.find( "[](RDG::PassBuilder&){}" ), std::string::npos )
         << "the simulation node declares nothing";

    std::ifstream particleFile(
         root / "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.cpp" );
    ASSERT_TRUE( particleFile );
    std::string particleText( ( std::istreambuf_iterator<char>( particleFile ) ),
                              std::istreambuf_iterator<char>() );
    particleText.erase( std::remove_if( particleText.begin(), particleText.end(),
                                        []( unsigned char c ) { return std::isspace( c ) != 0; } ),
                        particleText.end() );
    const size_t importAt = particleText.find( "ParticleRenderer::ImportSimulationBuffers(RDG::Builder&graph)" );
    ASSERT_NE( importAt, std::string::npos );
    const std::string importBody =
         particleText.substr( importAt, particleText.find( "voidParticleRenderer::", importAt ) - importAt );
    EXPECT_NE( importBody.find( "renderer.ImportBuffer(fe.Gpu->Particles,fe.ParticlesImport)" ),
               std::string::npos );
    EXPECT_NE( importBody.find( "renderer.ImportBuffer(fe.Gpu->Counter,fe.CounterImport)" ), std::string::npos );
    // An emitter the graph was not told about is not dispatched.
    const size_t simulateAt = particleText.find( "voidParticleRenderer::SimulateInFrame(" );
    ASSERT_NE( simulateAt, std::string::npos );
    EXPECT_NE( particleText.find( "if(!fe.Declared)continue;", simulateAt ), std::string::npos );

    // The same shape in a graph: two frames of a persistent buffer written by the node. The second frame's
    // write waits on the first's, from the state the first graph wrote back.
    ExternalBuffer state( BufferDesc{ 4096 }, Access::None );
    int            runs = 0;
    for ( int frame = 0; frame < 2; ++frame )
    {
        Builder         graph( "particles" );
        const BufferRef ref = graph.RegisterExternal( state, "ParticleState0" );
        graph.AddPass(
             "Particles: Simulate", PassFlags::Compute | PassFlags::NeverCull,
             [&]( PassBuilder& pass ) { pass.Write( ref, Access::StorageWrite ); },
             [&runs]( PassContext& )
             {
                 ++runs;
                 return Common::MakeSuccess( true );
             } );
        const CompileResult        result   = CompileOrFail( graph );
        const std::vector<Barrier> barriers = BarriersOn( result.FindPass( "Particles: Simulate" ), ref.Index );
        EXPECT_EQ( barriers.size(), frame == 0 ? 0u : 1u ) << "frame " << frame;
        RecordingBackend backend;
        ASSERT_TRUE( graph.Execute( backend ).IsSuccess() );
    }
    EXPECT_EQ( runs, 2 );
    EXPECT_EQ( state.State, GetAccessState( Access::StorageWrite ) );
}

namespace
{
    // The file at @p relative under the repository root with every whitespace character removed, so a needle is
    // independent of the formatter's line breaks.
    std::string SqueezedSource( const fs::path& root, const char* relative )
    {
        std::ifstream file( root / relative );
        EXPECT_TRUE( file ) << relative << " is gone";
        std::string text( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
        text.erase(
             std::remove_if( text.begin(), text.end(), []( unsigned char c ) { return std::isspace( c ) != 0; } ),
             text.end() );
        return text;
    }

    // The squeezed body of the definition that starts with @p signature, up to the next definition of its class.
    std::string SqueezedBody( const std::string& text, std::string_view signature, std::string_view next )
    {
        const size_t begin = text.find( signature );
        if ( begin == std::string::npos )
            return {};
        const size_t end = text.find( next, begin + signature.size() );
        return text.substr( begin, end == std::string::npos ? std::string::npos : end - begin );
    }

    // The definition of @p signature in @p text up to its closing brace, so a comment inside the body naming
    // another member cannot cut it short. Empty when the signature or a balanced body is not found.
    std::string FunctionBody( const std::string& text, std::string_view signature )
    {
        const size_t begin = text.find( signature );
        if ( begin == std::string::npos )
            return {};
        const size_t open = text.find( '{', begin + signature.size() );
        if ( open == std::string::npos )
            return {};
        int depth = 0;
        for ( size_t i = open; i < text.size(); ++i )
        {
            if ( text[i] == '{' )
                ++depth;
            else if ( text[i] == '}' && --depth == 0 )
                return text.substr( begin, i + 1 - begin );
        }
        return {};
    }
} // namespace

// THE PHASE PASSES ARE REAL GRAPH NODES THAT DECLARE THEIR TARGETS (RDG-LEG1-L5a). The AddGraphPhasePasses bridge
// no longer opens the engine's render pass around a legacy wrapper: every registered pass is a Raster node whose
// targets are its framebuffer whole (ColorTarget / DepthTarget / ResolveTarget), whose reads are what the system
// names in RenderGraphBuilder::PassConfig::Declare, and whose render pass the graph opens and merges. Each system
// declares its own reads where it registers the pass, the editor's external passes through
// ExternalPassSpecification::Declare, and DispatchComputeCull records no barrier of its own any more (the particle
// draw declares its StorageRead).
TEST( RenderGraphCompile, PhasePassesAreRealGraphNodesThatDeclareTheirTargets )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const char* graphic = "Desert/Desert/Source/Engine/Graphic/";
    const auto  source  = [&]( std::string_view relative )
    { return SqueezedSource( root, std::format( "{}{}", graphic, relative ).c_str() ); };

    const std::string sceneRenderer = source( "SceneRenderer.cpp" );
    EXPECT_EQ( sceneRenderer.find( "AddLegacy(" ), std::string::npos ) << "SceneRenderer.cpp adds a legacy pass";
    EXPECT_EQ( sceneRenderer.find( "BeginRenderPass(pass->CachedRenderPass" ), std::string::npos )
         << "the bridge still opens the engine render pass itself";

    const std::string bridge = SqueezedBody( source( "SceneRendererFrameMesh.cpp" ),
                                             "voidSceneRenderer::AddGraphPhasePasses(", "voidSceneRenderer::" );
    ASSERT_FALSE( bridge.empty() ) << "no AddGraphPhasePasses in SceneRendererFrameMesh.cpp";
    for ( const char* needle :
          { "RDG::PassFlags::Raster", "pass.Declare(declared)",
            "ResolveDeclared(textures,declared,pass.Name,images)", "DeclareOn(node,images,declared)",
            "node.ColorTarget(slot,targets->Colors[slot],color)", "node.DepthTarget(targets->Depth,depth)",
            "node.ResolveTarget(slot,targets->Resolves[slot])",
            "RDG::LoadOp::ClearDepth(spec.ClearColor.DepthStencil.x)" } )
        EXPECT_NE( bridge.find( needle ), std::string::npos ) << "the phase pass node does not " << needle;
    EXPECT_EQ( bridge.find( "AddLegacy(" ), std::string::npos );
    EXPECT_EQ( bridge.find( "BeginRenderPass(" ), std::string::npos );
    EXPECT_EQ( bridge.find( "EndRenderPass(" ), std::string::npos );

    // The declaration lives on the pass registration, not in a list in SceneRenderer.
    EXPECT_NE( source( "RenderGraphBuilder.hpp" ).find( "std::function<void(RenderPassDeclaration&)>Declare;" ),
               std::string::npos );
    EXPECT_NE( source( "ExternalRenderPass.hpp" )
                    .find( "std::function<void(RenderPassDeclaration&,constExternalPassContext&)>Declare;" ),
               std::string::npos );

    // Each system names what its pass samples, in its own RegisterPasses.
    const std::pair<const char*, const char*> declared[] = {
         { "Systems/Scene/Skybox/SkyboxRenderer.cpp", "declared.Read(m_SkyViewLut,RDG::Access::SampledGraphics" },
         { "Systems/Scene/Skybox/SkyboxRenderer.cpp",
           "declared.Read(m_TransmittanceLut,RDG::Access::SampledGraphics" },
         { "Systems/Scene/Mesh/MeshRenderer.cpp", "m_SceneRenderer->DeclareShadowReads(declared)" },
         { "Systems/Scene/Terrain/TerrainRenderer.cpp", "m_SceneRenderer->DeclareShadowReads(declared)" },
         { "Systems/Scene/Particles/ParticleRenderer.cpp",
           "declared.Read(fe.ParticlesRef,RDG::Access::StorageRead)" },
         { "Systems/Scene/Fog/HeightFogRenderer.cpp", "declared.Read(m_FogImage,RDG::Access::SampledGraphics" },
         { "Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
           "declared.Read(m_HistoryImage[m_ResolvedIndex],RDG::Access::SampledGraphics" },
         { "SceneRenderer.cpp", "declared.Read(mesh->GetCascadeShadowImage(c),RDG::Access::SampledGraphics" },
         { "SceneRenderer.cpp", "declared.Read(clouds->GetShadowMap(),RDG::Access::SampledGraphics" } };
    for ( const auto& [file, needle] : declared )
        EXPECT_NE( source( file ).find( needle ), std::string::npos ) << file << " does not declare " << needle;

    // The editor's UI pass declares the backdrop pyramid it samples (no blanket write, no phase-wide sample list).
    EXPECT_NE(
         SqueezedSource( root, "Editor/Source/Editor/RenderSystems/Passes/EditorUIPass.cpp" )
              .find( "declared.Read(ctx.Renderer->GetBackdropBlurImage(),Graphic::RDG::Access::SampledGraphics" ),
         std::string::npos );

    // The particle simulation's dispatch records no barrier: the graph places it between the declared write and
    // read.
    const std::string cull =
         SqueezedBody( source( "API/Vulkan/VulkanRenderer.cpp" ), "voidVulkanRendererAPI::DispatchComputeCull(",
                       "voidVulkanRendererAPI::" );
    ASSERT_FALSE( cull.empty() );
    EXPECT_EQ( cull.find( "vkCmdPipelineBarrier" ), std::string::npos ) << "DispatchComputeCull still barriers";
}

// THE ATMOSPHERE PASSES ARE REAL GRAPH NODES WITH DECLARED ACCESS (RDG-LEG1-L5a). SceneRendererFrameAtmosphere.cpp
// adds no legacy pass: the sky LUTs, the cloud shadow map, the atmospheric fog and the clouds are Compute nodes,
// one per dispatch, each declaring what it writes (StorageWrite) and samples (SampledCompute), and none of their
// bodies moves an image between layouts itself (no ComputeImageBegin*/End*, no TransitionLayout): the graph places
// every barrier.
TEST( RenderGraphCompile, AtmospherePassesAreRealGraphNodesWithDeclaredAccess )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const auto source = [&]( const char* relative )
    { return SqueezedSource( root, std::format( "Desert/Desert/Source/Engine/Graphic/{}", relative ).c_str() ); };

    const std::string frame = source( "SceneRendererFrameAtmosphere.cpp" );
    EXPECT_EQ( frame.find( "AddLegacy(" ), std::string::npos ) << "an atmosphere pass is still a legacy wrapper";
    for ( const char* needle : { "AddComputeNodes(graph,textures,clouds->DeclareShadowMapNodes())",
                                 "AddComputeNodes(graph,textures,sky->DeclareAtmosphereLutNodes())",
                                 "AddComputeNodes(graph,textures,fog->DeclareFrameNodes())",
                                 "AddComputeNodes(graph,textures,clouds->DeclareFrameNodes())" } )
        EXPECT_NE( frame.find( needle ), std::string::npos ) << needle;
    EXPECT_NE( source( "SceneRendererFrame.hpp" ).find( "RDG::PassFlags::Compute|RDG::PassFlags::NeverCull" ),
               std::string::npos );

    struct Declares
    {
        const char*                        File;
        const char*                        Function;
        std::initializer_list<const char*> Needles;
    };
    const Declares declares[] = {
         { "Systems/Scene/Skybox/SkyboxRenderer.cpp",
           "SkyboxRenderer::DeclareAtmosphereLutNodes(",
           { "Write(m_TransmittanceLut,RDG::Access::StorageWrite",
             "Write(m_MultiScatterLut,RDG::Access::StorageWrite", "Write(m_SkyViewLut,RDG::Access::StorageWrite",
             "Write(m_AerialPerspectiveLut,RDG::Access::StorageWrite",
             "Write(m_DistantLight,RDG::Access::StorageWrite",
             "Read(m_TransmittanceLut,RDG::Access::SampledCompute" } },
         { "Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
           "VolumetricCloudRenderer::DeclareShadowMapNodes(",
           { "DeclareVolumeReads(shadow.Access)", "Write(m_ShadowMapImage,RDG::Access::StorageWrite" } },
         { "Systems/Scene/Fog/HeightFogRenderer.cpp",
           "HeightFogRenderer::DeclareFrameNodes(",
           { "Read(depth,RDG::Access::SampledCompute",
             "DeclareAtmosphereReads(fog.Access,RDG::Access::SampledCompute)",
             "Write(m_FogImage,RDG::Access::StorageWrite" } },
         { "Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
           "VolumetricCloudRenderer::DeclareFrameNodes(",
           { "Write(m_SkyOcclusionVolume,RDG::Access::StorageWrite", "Read(depth,RDG::Access::SampledCompute",
             "Write(m_TraceImage,RDG::Access::StorageWrite", "Read(m_TraceImage,RDG::Access::SampledCompute",
             "Write(m_HistoryImage[writeIndex],RDG::Access::StorageWrite" } } };
    for ( const Declares& d : declares )
    {
        const std::string body = FunctionBody( source( d.File ), d.Function );
        ASSERT_FALSE( body.empty() ) << d.File << ": no " << d.Function;
        for ( const char* needle : d.Needles )
            EXPECT_NE( body.find( needle ), std::string::npos ) << d.Function << " does not declare " << needle;
        for ( const char* manual : { "ComputeImageBegin", "ComputeImageEnd", "TransitionLayout(" } )
            EXPECT_EQ( body.find( manual ), std::string::npos ) << d.Function << " still records " << manual;
    }
    // The LUT dispatch bodies and the fog file record no layout change of their own.
    for ( const char* file :
          { "Systems/Scene/Skybox/SkyboxRenderer.cpp", "Systems/Scene/Fog/HeightFogRenderer.cpp" } )
        EXPECT_EQ( source( file ).find( "ComputeImage" ), std::string::npos ) << file;

    // The composite reads the cloud shadow map (through DeclareShadowReads), so the graph brings it back to a
    // sampled layout after the shadow node's storage write.
    EXPECT_NE( source( "SceneRenderer.cpp" )
                    .find( "ResolveDeclared(textures,shadows,\"Deferred:Composite\",shadowMaps)" ),
               std::string::npos );
}
