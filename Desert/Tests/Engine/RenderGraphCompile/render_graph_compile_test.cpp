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
        const size_t begin = source.find( std::format( "void SceneRenderer::{}(", function ) );
        if ( begin == std::string::npos )
            return {};
        const size_t end = source.find( "void SceneRenderer::", begin + 1 );
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
            const size_t first  = std::min( { legacy, phases, frame } );
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
            else if ( first == legacy )
            {
                const size_t open  = text.find( '"', legacy );
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
         "ClearMainFramebuffer",   "Particles: SimulateInFrame",
         "CloudShadowMap",         "phases[!RenderPhase::IsDeferredOverlay(phase)]",
         "Deferred: GBuffer",      "TerrainGBuffer",
         "Deferred: DepthResolve", "Deferred: SSAO",
         "Deferred: RSM",          "Deferred: GIResolve",
         "Deferred: GITemporal",   "Deferred: Composite",
         "Deferred: Generic",      "Deferred: Skinned",
         "Deferred: SceneCopy",    "Deferred: SSR",
         "Deferred: SSRResolve",   "Deferred: SSRComposite",
         "Deferred: Glass",
         "SkyAtmosphereLuts",      "AtmosphericFog",
         "VolumetricClouds",       "phases[phase==RenderPhase::Transparency]",
         "Debug: Overdraw",        "phases[phase==RenderPhase::Debug]",
         "UI: BackdropBlur",       "phases[phase==RenderPhase::UI]",
         "PostFX: JumpFlood",      "PostFX: AutoExposure",
         "PostFX: Bloom",          "PostFX: LightShafts",
         "PostFX: LensFlare",      "PostFX: Tonemap",
         "PostFX: FXAA",           "PostFX: SMAA",
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
    EXPECT_NE( body.find( "\"Deferred:DepthResolve\",RDG::PassFlags::Copy" ), std::string::npos );
    // The declarations are DeferredFrameNodes', which
    // DepthResolveCopyPlansTransferBarriersAndCompositeTakesTheDepthBack compiles on recorded images.
    EXPECT_NE( body.find( "DeferredFrameNodes::DeclareDepthResolve(pass,sourceRef,targetRef)" ),
               std::string::npos );
    EXPECT_NE( body.find( "\"GBuffer.Depth\",DeferredFrameNodes::kGBufferDepthFinal" ), std::string::npos );
    EXPECT_EQ( body.find( "AddLegacy(" ), std::string::npos );
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
            texture.RecordFinalStates = [writtenBack]( const std::vector<AccessState>& states )
            {
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
    color.RecordFinalStates = []( const std::vector<AccessState>& )
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
