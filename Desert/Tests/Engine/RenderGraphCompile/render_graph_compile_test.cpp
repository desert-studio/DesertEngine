// RenderGraphCompile - the device-free core of the render graph (RDG1). Every test builds a graph the way
// a frame would, compiles it WITHOUT a device and checks the data an executor would record: which passes
// run, the barrier batch before each, load/store decisions, lifetimes and the aliasing plan. The suite
// compiles the RDG sources directly with no Vulkan include path, and the census below proves the sources
// never ask for one.

#include <Common/Core/DevInstruments.hpp>
#include <Engine/Graphic/Systems/Scene/Particles/ParticlePool.hpp>
#include <Engine/Graphic/Systems/Scene/Particles/ParticleSortGraph.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGExtensionPoint.hpp>
#include <Engine/Graphic/RDG/RDGFault.hpp>
#include <Engine/Graphic/RDG/RDGLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/RDG/RDGSystemTextures.hpp>
#include <Engine/Graphic/DeferredFrameNodes.hpp>
#include <Engine/Graphic/ViewRasterTargets.hpp>
#include <Engine/Graphic/RenderPassDeclaration.hpp>

// `#if DESERT_DEV_INSTRUMENTS` on a macro nobody defined is `#if 0`: without DevInstruments.hpp above, every
// instrument-gated test here was skipped in Debug too. Refuse to compile rather than skip silently again.
#if !defined( DESERT_DEV_INSTRUMENTS )
#error "DevInstruments.hpp must define DESERT_DEV_INSTRUMENTS before the instrument-gated tests"
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <filesystem>
#include <memory>
#include <regex>
#include <span>
#include <format>
#include <functional>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
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

    // A DeferredFrameNodes declare callback with no shader behind it: the node's sampled read, declared plainly.
    void ReadSampledGraphics( PassBuilder& pass, TextureRef texture )
    {
        pass.Read( texture, Access::SampledGraphics );
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
        [[nodiscard]] Common::ResultStr<MemoryRequirements> GetTextureRequirements( const TextureDesc& desc,
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
        [[nodiscard]] Common::ResultStr<MemoryRequirements> GetBufferRequirements( const BufferDesc& desc,
                                                                                   uint32_t ) const override
        {
            return Common::MakeSuccess(
                 MemoryRequirements{ AlignUp( desc.Bytes, kBufferAlignment ), kBufferAlignment, ~0u } );
        }

        static constexpr uint64_t kTextureAlignment = 64ull * 1024ull;
        static constexpr uint64_t kBufferAlignment  = 256ull;
    };

    const FixedEstimate kEstimate;

    // The recording backend creates no resources: BeginGraph never places, so nothing here is reached.
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
            return Common::MakeError<std::shared_ptr<IPhysicalTexture>>( "the recording backend places nothing" );
        }
        Common::ResultStr<std::shared_ptr<IPhysicalBuffer>> PlaceBuffer( const Allocation&, const BufferDesc&,
                                                                         uint32_t, std::string_view ) override
        {
            return Common::MakeError<std::shared_ptr<IPhysicalBuffer>>( "the recording backend places nothing" );
        }
        void EndGraph( std::string_view ) override
        {
        }
        TransientAllocatorStats GetStats() const override
        {
            return {};
        }
    };

    // Records the call sequence Execute drives, one line per call; touches no device.
    class RecordingBackend final : public IBackend
    {
    public:
        explicit RecordingBackend( const IMemoryRequirementsProvider& memory = kEstimate ) : m_Memory( memory )
        {
        }

        [[nodiscard]] BackendKind GetKind() const override
        {
            return BackendKind::Recording;
        }
        [[nodiscard]] const IMemoryRequirementsProvider& GetMemoryRequirements() const override
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
        Common::BoolResultStr UploadBuffer( uint32_t resource, std::span<const std::byte> bytes ) override
        {
            Calls.push_back( std::format( "UploadBuffer {} {} first {}", resource, bytes.size(),
                                          bytes.empty() ? -1 : static_cast<int>( bytes.front() ) ) );
            return Common::MakeSuccess( true );
        }
        [[nodiscard]] std::shared_ptr<IPhysicalTexture> GetPhysicalTexture( uint32_t ) const override
        {
            return nullptr;
        }
        [[nodiscard]] std::shared_ptr<IPhysicalBuffer> GetPhysicalBuffer( uint32_t ) const override
        {
            return nullptr;
        }

        ITransientAllocator& GetTransientAllocator() override
        {
            return m_Allocator;
        }
        PipeCapabilities GetPipeCapabilities() const override
        {
            return Pipes;
        }
        AsyncComputeFallbackLog& GetAsyncComputeFallbackLog() override
        {
            return m_FallbackLog;
        }
        PassFaultReporter& GetPassFaultReporter() override
        {
            return m_FaultReporter;
        }
        Common::BoolResultStr BeginPipeSegment( const PipeSegment& segment ) override
        {
            Segments.push_back( std::format( "Begin {} {}..{}",
                                             segment.OnPipe == Pipe::Graphics ? "Graphics" : "AsyncCompute",
                                             segment.FirstPosition, segment.LastPosition ) );
            return Common::MakeSuccess( true );
        }
        Common::BoolResultStr EndPipeSegment( const PipeSegment& ) override
        {
            Segments.push_back( "End" );
            return Common::MakeSuccess( true );
        }
        void RecordEpilogueBarriers( std::span<const Barrier> barriers ) override
        {
            Calls.push_back( std::format( "Epilogue {}", barriers.size() ) );
        }

        std::vector<std::string> Calls;
        std::vector<std::string> Segments; // Begin/EndPipeSegment, kept out of Calls
        std::vector<std::string> FallbackLines;
        std::vector<std::string> FaultLines; // what the backend's PassFaultReporter logged, in order
        PipeCapabilities         Pipes;

    private:
        NoPlacementAllocator    m_Allocator;
        AsyncComputeFallbackLog m_FallbackLog{ [this]( std::string_view line )
                                               { FallbackLines.emplace_back( line ); } };
        PassFaultReporter       m_FaultReporter{ [this]( PassFaultReporter::Severity, std::string_view line )
                                           { FaultLines.emplace_back( line ); } };

    public:
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
        const Common::ResultStr<CompileResult> result = builder.Compile( kEstimate );
        EXPECT_TRUE( result.IsSuccess() ) << result.GetError();
        return result ? result.GetValue() : CompileResult{};
    }

    // RDG-FAULT1: the one Declaration fault a malformed pass leaves, as "<pass>: <reason>" (empty: none / more).
    std::string OnlyDeclarationFault( const Builder& builder )
    {
        const CompileResult result = CompileOrFail( builder );
        if ( result.Faults.size() != 1 || result.Faults[0].Stage != PassFaultStage::Declaration )
            return {};
        return std::format( "{}: {}", result.Faults[0].PassName, result.Faults[0].Reason );
    }

    std::vector<Barrier> BarriersOn( const CompiledPass* pass, uint32_t resource )
    {
        std::vector<Barrier> out;
        if ( pass == nullptr )
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

// VFX-08e. The particle sprites draw through their material's ParticleSprite cell, so the renderer names no
// shader of its own but the default sprite template and the three simulation programs; the old billboard
// program and its hand-written material are gone. Red when a hard-wired sprite shader comes back.
TEST( RenderGraphCompile, TheParticleRendererNamesNoSpriteShaderButTheDefaultTemplate )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    const fs::path engine = root / "Desert/Desert/Source/Engine/Graphic";
    std::ifstream  file( ( engine / "Systems/Scene/Particles/ParticleRenderer.cpp" ).string() );
    ASSERT_TRUE( file.good() );
    const std::string source( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
    for ( const char* gone : { "ParticleBillboard", ".shader", "DrawsAdditive" } )
        EXPECT_EQ( source.find( gone ), std::string::npos ) << "ParticleRenderer.cpp names " << gone;
    EXPECT_NE( source.find( "\"ParticleSpriteDefault\"" ), std::string::npos );
    EXPECT_FALSE( fs::exists( engine / "Materials/Particles/MaterialParticleBillboard.hpp" ) );
    EXPECT_FALSE( fs::exists( root / "Editor/Resources/Shaders/Programs/Particles/ParticleBillboard.shader" ) );
}

// VFX-08f. The translucent sprite sort, built from the declarations ParticleRenderer::AddSortPasses makes
// (PlanParticleSort + DeclareParticleSortStage) between a stand-in for the last Compact (writes the pool, the
// alive list and the draw slots) and a stand-in for ParticlePass (reads the sorted list for the sorted emitter and
// the alive list for the opaque one). Red if an opaque / masked emitter gets sort nodes, a translucent / additive
// one gets none or a stage count other than ParticleSortStages, a stage leaves a slot of the shader's layout
// undeclared (Validation fault), or any sort node can run before the compact or after the draw.
TEST( RenderGraphCompile, TheParticleSortRunsBetweenTheLastCompactAndTheDrawForBlendedEmittersOnly )
{
    using Desert::Core::Formats::SurfaceBlendMode;
    namespace Sys = Desert::Graphic::System;

    const std::vector<Sys::ParticleSortEmitter> emitters = {
         { 0, SurfaceBlendMode::Opaque, 300, 0, 0 },
         { 1, SurfaceBlendMode::Translucent, 3000, 2 * 300, 0 },
         { 2, SurfaceBlendMode::Masked, 64, 2 * 3300 + 64, 1 },
         { 3, SurfaceBlendMode::Additive, 16, 2 * 3364 + 16, 1 },
    };
    const Sys::ParticleSortPlan plan = Sys::PlanParticleSort( emitters );
    ASSERT_EQ( plan.Ranges.size(), 2u );
    EXPECT_EQ( plan.Ranges[0].Emitter, 1u );
    EXPECT_EQ( plan.Ranges[1].Emitter, 3u );
    EXPECT_EQ( plan.Ranges[0].Length, 4096u );
    EXPECT_EQ( plan.Ranges[1].KeyBase, 4096u );
    EXPECT_EQ( plan.KeyCount, 4096u + 16u );

    ExternalTexture  backbuffer( Tex2D( 32, 32, ImageFormat::BGRA8F ), Access::None );
    Builder          graph( "particle sort" );
    const BufferRef  particles = graph.CreateBuffer( BufferDesc{ 3380u * 64u }, "ParticlePool" );
    const BufferRef  alive     = graph.CreateBuffer( BufferDesc{ 2u * 3380u * 4u }, "ParticleAliveList" );
    const BufferRef  counters  = graph.CreateBuffer( BufferDesc{ 64 }, "ParticleCounters" );
    const TextureRef back      = graph.RegisterExternal( backbuffer, "Backbuffer" );
    graph.AddPass(
         "Particles: Compact 1", PassFlags::Compute,
         [&]( PassBuilder& pass )
         {
             pass.Write( particles, Access::StorageWrite );
             pass.Write( alive, Access::StorageWrite );
             pass.Write( counters, Access::StorageWrite );
         },
         Ok );

    const auto [keys, sorted] = Sys::CreateParticleSortBuffers( graph, plan, 2u * 3380u * 4u );
    const auto layout         = std::make_shared<const ShaderBindingLayout>(
         ShaderBindingLayout{ "ParticleSort",
                                      { { "Particles", ShaderResourceKind::StorageBuffer },
                                        { "AliveList", ShaderResourceKind::StorageBuffer },
                                        { "Counters", ShaderResourceKind::StorageBuffer },
                                        { "SortKeys", ShaderResourceKind::StorageBuffer },
                                        { "SortedAlive", ShaderResourceKind::StorageBuffer } },
                              Sys::kParticleSortPushBytes } );
    const Sys::ParticleSortBuffers buffers{ particles, alive, counters, keys, sorted };
    std::vector<std::string>       sortNames;
    for ( const Sys::ParticleSortRange& range : plan.Ranges )
    {
        const auto stages = Sys::ParticleSortStages( range.Length );
        for ( uint32_t s = 0; s < static_cast<uint32_t>( stages.size() ); ++s )
        {
            const Sys::ParticleSortStageKind kind = stages[s].Kind;
            sortNames.push_back( Sys::ParticleSortPassName( range.Emitter, s ) );
            graph.AddPass(
                 sortNames.back(), PassFlags::Compute, [&, kind]( PassBuilder& pass )
                 { Sys::DeclareParticleSortStage( pass, layout, {}, buffers, kind ); }, Ok );
        }
    }
    graph.AddPass(
         "ParticlePass", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( particles, Access::StorageRead );
             pass.Read( alive, Access::StorageRead );  // the opaque / masked emitters
             pass.Read( sorted, Access::StorageRead ); // the translucent / additive ones
             pass.Read( counters, Access::IndirectArgs );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    EXPECT_TRUE( result.Faults.empty() ) << ( result.Faults.empty() ? "" : result.Faults[0].Reason );
    // 4096 keys: Keys, Local, (Global j=1024, Merge) for k = 2048 and (Global j=2048, Global j=1024, Merge) for
    // k = 4096, Write; 16 keys: Keys, Local, Write.
    EXPECT_EQ( sortNames.size(), Sys::ParticleSortStages( 4096 ).size() + 3u );
    for ( const uint32_t unsorted : { 0u, 2u } )
        EXPECT_EQ( result.FindPass( Sys::ParticleSortPassName( unsorted, 0 ) ), nullptr )
             << "an opaque / masked emitter is drawn unsorted and has no sort node";

    auto position = [&]( const std::string& name )
    {
        const auto it = std::find_if( result.Passes.begin(), result.Passes.end(),
                                      [&]( const auto& pass ) { return pass.Name == name; } );
        return it == result.Passes.end() ? -1 : static_cast<int>( it - result.Passes.begin() );
    };
    const int compact = position( "Particles: Compact 1" );
    const int draw    = position( "ParticlePass" );
    ASSERT_GE( compact, 0 );
    ASSERT_GE( draw, 0 );
    int previous = compact;
    for ( const std::string& name : sortNames )
    {
        const int at = position( name );
        EXPECT_GT( at, previous ) << name << " is culled or runs out of its stage order / before the compact";
        EXPECT_LT( at, draw ) << name << " runs after the draw that reads its sorted list";
        previous = at;
    }
}

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

// THE TWO HALVES OF ONE VULKAN DECLARATION, read where they are spelled. (1) Every vkCmdPushConstants names the
// stages of the pipeline layout it pushes into (VulkanPipeline::GetPushConstantRange, or the compute layout's one
// COMPUTE range), never a shader's reflected ShaderStage: the shader is re-reflected by a recompile while the
// pipeline keeps its layout, and pushing the reflection's stages raised VUID-vkCmdPushConstants-offset-01795
// 4082 times in one editor session (GATE-VAL1). (2) The back buffer is imported in kPresentAcquiredState and the
// acquire wait stage is derived from it, so the two cannot drift (the companion of
// TheAcquiredBackBuffersFirstBarrierWaitsAtTheAcquireStage).
TEST( RenderGraphCompile, VulkanPushStagesAndAcquireStageComeFromOneDeclaration )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const fs::path dir = root / "Desert/Desert/Source/Engine/Graphic/API/Vulkan";
    ASSERT_TRUE( fs::is_directory( dir ) ) << dir.string();
    const auto read = []( const fs::path& path )
    {
        std::ifstream      file( path );
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    };

    int pushes = 0;
    for ( const fs::directory_entry& entry : fs::directory_iterator( dir ) )
    {
        if ( entry.path().extension() != ".cpp" )
            continue;
        const std::string text = read( entry.path() );
        for ( size_t at = text.find( "vkCmdPushConstants(" ); at != std::string::npos;
              at        = text.find( "vkCmdPushConstants(", at + 1 ) )
        {
            ++pushes;
            const std::string call = text.substr( at, text.find( ';', at ) - at );
            EXPECT_EQ( call.find( "ShaderStage" ), std::string::npos )
                 << entry.path().filename().string() << ": a push names a shader's reflected stages:\n"
                 << call;
            const bool layoutRange = call.find( "stageFlags" ) != std::string::npos;
            const bool compute     = call.find( "VK_SHADER_STAGE_COMPUTE_BIT" ) != std::string::npos;
            EXPECT_TRUE( layoutRange || compute )
                 << entry.path().filename().string() << ": a push names neither its layout's range nor COMPUTE:\n"
                 << call;
        }
    }
    EXPECT_GE( pushes, 4 ) << "the census lost the push sites it expected to read";

    const std::string swapchain = read( dir / "VulkanSwapChain.cpp" );
    const std::string output    = read( dir / "VulkanSwapChainOutput.cpp" );
    EXPECT_NE( swapchain.find( "RDG::kPresentAcquiredState )" ), std::string::npos )
         << "ImportBackBuffer no longer imports the back buffer in kPresentAcquiredState";
    EXPECT_NE( output.find( "RdgVulkanStages( RDG::kPresentAcquiredState.Stages )" ), std::string::npos )
         << "the acquire wait stage is no longer derived from kPresentAcquiredState";
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

// ── Culling (RDG-CULL): subresource accuracy, buffers, extraction chains, the switch, diagnostics ──────────

// Liveness is per SUBRESOURCE. A pass writing mip 0 that a live pass samples is kept; a pass writing mip 1 of
// the SAME texture, which nobody reads, is culled. A per-resource rule would keep both.
TEST( RenderGraphCompile, CullingIsPerMipAWriterOfAnUnreadMipIsCulled )
{
    ExternalTexture  backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
    Builder          graph( "mips" );
    const TextureRef chain = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F, 2 ), "Chain" );
    const TextureRef back  = graph.RegisterExternal( backbuffer, "Backbuffer" );

    graph.AddPass(
         "WriteMip0", PassFlags::Compute,
         [&]( PassBuilder& pass ) { pass.Write( chain, Access::StorageWrite, SubresourceRange::Mip( 0 ) ); }, Ok );
    graph.AddPass(
         "WriteMip1", PassFlags::Compute,
         [&]( PassBuilder& pass ) { pass.Write( chain, Access::StorageWrite, SubresourceRange::Mip( 1 ) ); }, Ok );
    graph.AddPass(
         "Present", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( chain, Access::SampledGraphics, SubresourceRange::Mip( 0 ) );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    EXPECT_EQ( result.CulledPasses, std::vector<uint32_t>{ 1 } );
    EXPECT_EQ( result.CulledPassNames, std::vector<std::string>{ "WriteMip1" } );
    EXPECT_NE( result.FindPass( "WriteMip0" ), nullptr );
    EXPECT_EQ( result.FindPass( "WriteMip1" ), nullptr );
    EXPECT_TRUE( HasEdge( result, 0, 2, DependencyKind::ReadAfterWrite ) );
}

// RDG-A2 decision 2: every node kind (a system's ComputeNodeDeclaration, a phase pass, an editor external pass)
// names a graph texture another node produced by its REF and a subresource range
// (RenderPassDeclaration::Read( TextureRef, Access, SubresourceRange )); DeclareRefsOn is the code all of them
// declare through (SceneRendererFrame.hpp DeclareOn). The read becomes a real edge on exactly the declared mip:
// the producer of mip 0 is ordered before the reader, the producer of mip 1 nobody declared is culled.
TEST( RenderGraphCompile, ADeclaredGraphTextureReadOrdersTheNodeAfterItsProducerOnTheDeclaredMip )
{
    ExternalTexture  backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
    Builder          graph( "declared" );
    const TextureRef blur = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F, 2 ), "BackdropBlur" );
    const TextureRef back = graph.RegisterExternal( backbuffer, "Backbuffer" );

    graph.AddPass(
         "BlurMip0", PassFlags::Compute,
         [&]( PassBuilder& pass ) { pass.Write( blur, Access::StorageWrite, SubresourceRange::Mip( 0 ) ); }, Ok );
    graph.AddPass(
         "BlurMip1", PassFlags::Compute,
         [&]( PassBuilder& pass ) { pass.Write( blur, Access::StorageWrite, SubresourceRange::Mip( 1 ) ); }, Ok );

    Desert::Graphic::RenderPassDeclaration declared;
    declared.Read( blur, Access::SampledGraphics, SubresourceRange::Mip( 0 ) );
    ASSERT_EQ( Desert::Graphic::InvalidDeclaredRef( declared ), nullptr );
    graph.AddPass(
         "UIGlass", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             Desert::Graphic::DeclareRefsOn( pass, declared );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    EXPECT_EQ( result.CulledPassNames, std::vector<std::string>{ "BlurMip1" } );
    ASSERT_NE( result.FindPass( "BlurMip0" ), nullptr );
    EXPECT_TRUE( HasEdge( result, 0, 2, DependencyKind::ReadAfterWrite ) );
}

// The refusal half: a declaration naming a ref that is not a handle of this graph (a transient no earlier node
// produced this frame, a buffer nobody gave the graph) is reported, so ResolveDeclared refuses the whole node
// and nothing is added half-declared; a declaration of valid refs only is accepted.
// RDG-A2 (UE: FRDGTexture::Desc). A reader derives what depends on a texture's shape from the texture itself:
// the builder hands back the description a created or registered texture carries (the UI glass reads the
// BackdropBlur pyramid's mip count this way, not from a value its producer publishes beside the ref), and
// refuses a handle that is not a texture of this graph.
TEST( RenderGraphCompile, TheBuilderHandsBackTheDescriptionATextureWasCreatedOrRegisteredWith )
{
    Builder          graph( "Desc" );
    const TextureRef pyramid = graph.CreateTexture( Tex2D( 64, 32, ImageFormat::RGBA16F, 5 ), "Pyramid" );
    ExternalTexture  scene( Tex2D( 128, 64, ImageFormat::RGBA8F ), Access::None );
    const TextureRef imported = graph.RegisterExternal( scene, "Scene" );

    const auto created = graph.GetTextureDesc( pyramid );
    ASSERT_TRUE( created.IsSuccess() ) << created.GetError();
    EXPECT_EQ( created.GetValue().Mips, 5u );
    EXPECT_EQ( created.GetValue().Size.Width, 64u );
    EXPECT_EQ( created.GetValue().Size.Height, 32u );
    EXPECT_EQ( created.GetValue().Format, ImageFormat::RGBA16F );

    const auto registered = graph.GetTextureDesc( imported );
    ASSERT_TRUE( registered.IsSuccess() ) << registered.GetError();
    EXPECT_EQ( registered.GetValue().Size.Width, 128u );
    EXPECT_EQ( registered.GetValue().Format, ImageFormat::RGBA8F );

    EXPECT_FALSE( graph.GetTextureDesc( TextureRef{} ).IsSuccess() );
    const BufferRef buffer = graph.CreateBuffer( BufferDesc{ .Bytes = 256 }, "NotATexture" );
    EXPECT_FALSE( graph.GetTextureDesc( TextureRef{ buffer.Index } ).IsSuccess() );
}

// RDG-FAULT1 C3b: a binding-block entry may name an engine image its system owns across frames (an atmosphere
// LUT, the cloud history) instead of a ref of this graph. It is not a ref until SceneRenderer imports it
// (ResolveDeclared), so InvalidDeclaredRef does not refuse it; DeclareRefsOn declares it on the ref the import
// gave, as a storage write / sampled read the graph orders: the reader runs after the writer, a writer of an
// image nobody reads is culled.
TEST( RenderGraphCompile, ABlockEntryNamingAnEngineImageIsDeclaredOnItsImportAndOrdered )
{
    // Never dereferenced: the declaration only carries the image to the import (aliasing, non-owning).
    const int                                     lutToken = 0;
    const std::shared_ptr<Desert::Graphic::Image> lut( std::shared_ptr<void>(),
                                                       std::bit_cast<Desert::Graphic::Image*>( &lutToken ) );

    ExternalTexture  lutImport( Tex2D( 64, 32, ImageFormat::RGBA16F ), Access::None );
    ExternalTexture  backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
    Builder          graph( "block images" );
    const TextureRef lutRef = graph.RegisterExternal( lutImport, "Sky.TransmittanceLut" );
    const TextureRef back   = graph.RegisterExternal( backbuffer, "Backbuffer" );

    Desert::Graphic::RenderPassDeclaration writer;
    writer
         .Bindings( ShaderBindingLayout{ "TransmittanceLut",
                                         { { "u_TransmittanceLut", ShaderResourceKind::StorageTexture } } },
                    {} )
         .Storage( "u_TransmittanceLut", lut, Access::StorageWrite, "Sky.TransmittanceLut" );
    Desert::Graphic::RenderPassDeclaration reader;
    reader
         .Bindings(
              ShaderBindingLayout{ "Skybox", { { "u_TransmittanceLut", ShaderResourceKind::SampledTexture } } },
              {} )
         .Sampled( "u_TransmittanceLut", lut, Access::SampledGraphics, SamplerDesc::LinearClamp(),
                   "Sky.TransmittanceLut" );
    ASSERT_EQ( Desert::Graphic::BlockImageEntries( writer ).size(), 1u );
    EXPECT_EQ( Desert::Graphic::BlockImageEntries( writer ).front()->ImportName, "Sky.TransmittanceLut" );
    EXPECT_EQ( Desert::Graphic::InvalidDeclaredRef( writer ), nullptr );
    EXPECT_EQ( Desert::Graphic::InvalidDeclaredRef( reader ), nullptr );

    const std::vector<TextureRef> imported{ lutRef };
    graph.AddPass(
         "Sky: TransmittanceLut", PassFlags::Compute,
         [&]( PassBuilder& pass ) { Desert::Graphic::DeclareRefsOn( pass, writer, imported ); }, Ok );
    graph.AddPass(
         "Sky", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             Desert::Graphic::DeclareRefsOn( pass, reader, imported );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    EXPECT_TRUE( result.CulledPassNames.empty() );
    EXPECT_TRUE( HasEdge( result, 0, 1, DependencyKind::ReadAfterWrite ) );
}

// RDG-FAULT1 C3b: two block entries of one pass may read ONE image (the cloud resolve reads the 2D fallback as
// both its history and its history guide while the history is invalid) -- the import gives one ref and the pass
// holds it in one read-only state. The rule is the subresource's state, not the entry count: two reads in two
// layouts (sampled = shader-read-only, storage read = general) are refused by name, as a read plus a write is
// ("MalformedDeclarationsFaultTheirPassWithNames", "Both").
TEST( RenderGraphCompile, TwoBlockEntriesReadingOneImageInOneStateAreOneReadAndTwoLayoutsAreRefused )
{
    const int                                     fallbackToken = 0;
    const std::shared_ptr<Desert::Graphic::Image> fallback(
         std::shared_ptr<void>(), std::bit_cast<Desert::Graphic::Image*>( &fallbackToken ) );

    ExternalTexture  fallbackImport( Tex2D( 4, 4, ImageFormat::RGBA8F ), Access::None );
    ExternalTexture  reconstructed( Tex2D( 64, 64, ImageFormat::RGBA16F ), Access::None );
    Builder          graph( "one image twice" );
    const TextureRef fallbackRef = graph.RegisterExternal( fallbackImport, "Clouds.HistoryFallback" );
    const TextureRef output      = graph.RegisterExternal( reconstructed, "Clouds.History0" );

    Desert::Graphic::RenderPassDeclaration resolve;
    resolve
         .Bindings( ShaderBindingLayout{ "CloudResolve",
                                         { { "u_CloudHistory", ShaderResourceKind::SampledTexture },
                                           { "u_CloudHistoryGuide", ShaderResourceKind::SampledTexture } } },
                    {} )
         .Sampled( "u_CloudHistory", fallback, Access::SampledCompute, SamplerDesc::LinearRepeat(),
                   "Clouds.HistoryFallback" )
         .Sampled( "u_CloudHistoryGuide", fallback, Access::SampledCompute, SamplerDesc::LinearRepeat(),
                   "Clouds.HistoryFallback" );
    ASSERT_EQ( Desert::Graphic::BlockImageEntries( resolve ).size(), 2u );
    EXPECT_EQ( Desert::Graphic::InvalidDeclaredRef( resolve ), nullptr );

    // The one import, once per entry (FrameTextures::Import returns the first ref for the same image).
    const std::vector<TextureRef> imported{ fallbackRef, fallbackRef };
    graph.AddPass(
         "Clouds: Resolve", PassFlags::Compute | PassFlags::NeverCull,
         [&]( PassBuilder& pass )
         {
             Desert::Graphic::DeclareRefsOn( pass, resolve, imported );
             pass.Write( output, Access::StorageWrite );
         },
         Ok );
    const CompileResult result = CompileOrFail( graph );
    EXPECT_TRUE( result.Faults.empty() ) << ( result.Faults.empty() ? "" : result.Faults[0].Reason );
    EXPECT_TRUE( result.CulledPassNames.empty() );

    Builder          layouts( "one image two layouts" );
    const TextureRef twice = layouts.RegisterExternal( fallbackImport, "Clouds.HistoryFallback" );
    layouts.AddPass(
         "TwoLayouts", PassFlags::Compute | PassFlags::NeverCull,
         [&]( PassBuilder& pass )
         {
             pass.Read( twice, Access::SampledCompute );
             pass.Read( twice, Access::StorageRead );
         },
         Ok );
    const std::string fault = OnlyDeclarationFault( layouts );
    ASSERT_FALSE( fault.empty() );
    EXPECT_NE( fault.find( "TwoLayouts" ), std::string::npos ) << fault;
    EXPECT_NE( fault.find( "Clouds.HistoryFallback" ), std::string::npos ) << fault;
}

TEST( RenderGraphCompile, ADeclarationOfAnInvalidGraphRefIsRefused )
{
    Builder          graph( "refused" );
    const TextureRef made = graph.CreateTexture( Tex2D( 32, 32, ImageFormat::RGBA16F ), "Made" );

    Desert::Graphic::RenderPassDeclaration valid;
    valid.Read( made, Access::SampledCompute, SubresourceRange::All() );
    EXPECT_EQ( Desert::Graphic::InvalidDeclaredRef( valid ), nullptr );

    Desert::Graphic::RenderPassDeclaration texture;
    texture.Read( made, Access::SampledCompute, SubresourceRange::All() );
    texture.Read( TextureRef{}, Access::SampledCompute, SubresourceRange::All() );
    const char* textureRefusal = Desert::Graphic::InvalidDeclaredRef( texture );
    ASSERT_NE( textureRefusal, nullptr );
    EXPECT_NE( std::string_view( textureRefusal ).find( "graph texture" ), std::string_view::npos );

    Desert::Graphic::RenderPassDeclaration buffer;
    buffer.Write( BufferRef{}, Access::StorageWrite );
    const char* bufferRefusal = Desert::Graphic::InvalidDeclaredRef( buffer );
    ASSERT_NE( bufferRefusal, nullptr );
    EXPECT_NE( std::string_view( bufferRefusal ).find( "buffer" ), std::string_view::npos );
}

// VAL-SSAO1: "u_SSAO bound as VkImageView 0x0" (30 validation warnings, one editor session). Before RDG-A2 the
// composite's AO was a material Texture2DProperty set only `if ( m_SSAO && aoImage )`: a frame without an AO image
// (SSAO off, a new renderer's first frame) drew with whatever the property held - nothing on a fresh material.
// Now the AO input is declared from the graph: the SSAO transient when the SSAO pass ran, else the registered
// System.White (AO = 1); a ref that is neither faults the composite before its exec, so no null view is bound.
TEST( RenderGraphCompile, CompositeAOIsTheSSAOTransientOrSystemWhiteAndANullRefFaultsThePass )
{
    const ShaderBindingLayout layout{
         "DeferredLighting", { { "u_SSAO", ShaderResourceKind::SampledTexture } }, 0 };

    // SSAO did not run this frame: the composite reads System.White, compiled with no fault and not culled.
    {
        ExternalTexture  black{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
        ExternalTexture  white{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
        ExternalTexture  blackCube{ Tex2D( 1, 1, ImageFormat::RGBA8F, 1, 6 ), Access::SampledGraphics };
        ExternalTexture  backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
        Builder          graph( "composite without ssao" );
        const auto       system = RegisterSystemTextures( graph, black, white, blackCube );
        const TextureRef back   = graph.RegisterExternal( backbuffer, "Backbuffer" );
        const TextureRef ssao{}; // FrameTransients::SSAO when AddFrameSSAO returned early
        graph.AddPass(
             "Deferred: Composite", PassFlags::Raster,
             [&]( PassBuilder& pass )
             {
                 pass.Bindings( layout, {} )
                      .Sampled( "u_SSAO", ssao.IsValid() ? ssao : system.White, Access::SampledGraphics,
                                SubresourceRange::All(), SamplerDesc::LinearRepeat() );
                 pass.ColorTarget( 0, back, LoadOp::DontCare() );
             },
             Ok );
        const CompileResult result = CompileOrFail( graph );
        EXPECT_TRUE( result.Faults.empty() ) << ( result.Faults.empty() ? "" : result.Faults[0].Reason );
        ASSERT_EQ( result.Passes.size(), 1u );
        EXPECT_TRUE( result.CulledPassNames.empty() );
    }

    // The null path: an AO ref the graph does not know faults the composite by name at declaration.
    {
        ExternalTexture  backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
        Builder          graph( "composite with a null ao" );
        const TextureRef back = graph.RegisterExternal( backbuffer, "Backbuffer" );
        graph.AddPass(
             "Deferred: Composite", PassFlags::Raster,
             [&]( PassBuilder& pass )
             {
                 pass.Bindings( layout, {} )
                      .Sampled( "u_SSAO", TextureRef{}, Access::SampledGraphics, SubresourceRange::All(),
                                SamplerDesc::LinearRepeat() );
                 pass.ColorTarget( 0, back, LoadOp::DontCare() );
             },
             Ok );
        const std::string fault = OnlyDeclarationFault( graph );
        ASSERT_FALSE( fault.empty() );
        EXPECT_NE( fault.find( "Deferred: Composite" ), std::string::npos ) << fault;
        EXPECT_NE( fault.find( "invalid texture handle" ), std::string::npos ) << fault;
    }

    // The frame code keeps that shape: the one assignment of the composite's AO falls back to System.White, and
    // the deferred-lighting material holds no u_SSAO property of its own (the pre-RDG route that bound null).
    const fs::path root     = RepoRoot();
    const auto     stripped = [&root]( const char* relative )
    {
        std::ifstream file( root / relative );
        EXPECT_TRUE( file ) << relative << " is gone";
        std::string text( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
        std::erase_if( text, []( unsigned char c ) { return std::isspace( c ) != 0; } );
        return text;
    };
    const std::string frame = stripped( "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameDeferred.cpp" );
    const std::string_view assignment =
         "inputs.SSAO=refs.Transients.SSAO.IsValid()?refs.Transients.SSAO:refs.System.White;";
    EXPECT_NE( frame.find( assignment ), std::string::npos );
    const size_t first = frame.find( "inputs.SSAO=" );
    EXPECT_EQ( frame.find( "inputs.SSAO=", first == std::string::npos ? 0 : first + 1 ), std::string::npos )
         << "the composite's AO is assigned in more than one place";
    EXPECT_EQ( stripped( "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp" )
                    .find( "u_SSAO" ),
               std::string::npos );
}

// A chain of transients ending in an EXTRACTED texture survives although no pass of the graph reads the end of
// it: the extraction is the consumer. The unrelated dead pass beside it is still culled, by name.
TEST( RenderGraphCompile, AChainFeedingAnExtractedTextureIsKept )
{
    ExternalTexture  history;
    Builder          graph( "extract" );
    const TextureRef a    = graph.CreateTexture( Tex2D( 32, 32, ImageFormat::RGBA16F ), "A" );
    const TextureRef b    = graph.CreateTexture( Tex2D( 32, 32, ImageFormat::RGBA16F ), "B" );
    const TextureRef dead = graph.CreateTexture( Tex2D( 32, 32, ImageFormat::RGBA16F ), "Dead" );
    graph.Extract( b, history, Access::SampledGraphics );

    graph.AddPass(
         "WriteA", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( a, Access::StorageWrite ); }, Ok );
    graph.AddPass(
         "WriteDead", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( dead, Access::StorageWrite ); },
         Ok );
    graph.AddPass(
         "AtoB", PassFlags::Compute,
         [&]( PassBuilder& pass )
         {
             pass.Read( a, Access::SampledCompute );
             pass.Write( b, Access::StorageWrite );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    EXPECT_EQ( result.CulledPasses, std::vector<uint32_t>{ 1 } );
    EXPECT_EQ( result.CulledPassNames, std::vector<std::string>{ "WriteDead" } );
    ASSERT_EQ( result.Passes.size(), 2u );
    EXPECT_EQ( result.Passes[0].Name, "WriteA" ); // kept only because AtoB consumes it
    EXPECT_EQ( result.Passes[1].Name, "AtoB" );   // writes the extracted texture
}

// Buffers take part: a producer is kept by a live reader of its buffer, and a producer of a buffer nobody reads
// is culled even when it runs between the two.
TEST( RenderGraphCompile, ABufferProducerIsKeptByALiveBufferReader )
{
    ExternalTexture  backbuffer( Tex2D( 32, 32, ImageFormat::BGRA8F ), Access::None );
    Builder          graph( "buffers" );
    const BufferRef  counts = graph.CreateBuffer( BufferDesc{ 1024 }, "Counts" );
    const BufferRef  unread = graph.CreateBuffer( BufferDesc{ 1024 }, "Unread" );
    const TextureRef back   = graph.RegisterExternal( backbuffer, "Backbuffer" );

    graph.AddPass(
         "Produce", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( counts, Access::StorageWrite ); },
         Ok );
    graph.AddPass(
         "ProduceUnread", PassFlags::Compute,
         [&]( PassBuilder& pass ) { pass.Write( unread, Access::StorageWrite ); }, Ok );
    graph.AddPass(
         "Draw", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( counts, Access::StorageRead );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    EXPECT_EQ( result.CulledPassNames, std::vector<std::string>{ "ProduceUnread" } );
    EXPECT_NE( result.FindPass( "Produce" ), nullptr );
    EXPECT_TRUE( HasEdge( result, 0, 2, DependencyKind::ReadAfterWrite ) );
    EXPECT_EQ( result.FindAllocation( unread.Index ), nullptr );
    EXPECT_NE( result.FindAllocation( counts.Index ), nullptr );
}

// RDG-FAULT1 C3b (UE: QueueBufferUpload). CPU bytes reach a graph buffer through an upload the BUILDER owns: the
// bytes are copied at the call (the caller's vector is overwritten right after), the upload is a Copy pass that
// writes the buffer, and the reader added after it gets a read-after-write edge on it and runs after it.
TEST( RenderGraphCompile, AQueuedBufferUploadIsOrderedBeforeItsReaderAndCopiesTheBytes )
{
    ExternalTexture        backbuffer( Tex2D( 32, 32, ImageFormat::BGRA8F ), Access::None );
    Builder                graph( "upload" );
    const BufferRef        lights = graph.CreateBuffer( BufferDesc{ 64 }, "Lights" );
    const TextureRef       back   = graph.RegisterExternal( backbuffer, "Backbuffer" );
    std::vector<std::byte> bytes( 16, std::byte{ 7 } );
    graph.QueueBufferUpload( lights, bytes );
    std::fill( bytes.begin(), bytes.end(), std::byte{ 0 } ); // the graph holds its own copy
    graph.AddPass(
         "Composite", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( lights, Access::StorageRead );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    EXPECT_TRUE( result.Faults.empty() ) << ( result.Faults.empty() ? "" : result.Faults[0].Reason );
    ASSERT_NE( result.FindPass( "Upload: Lights" ), nullptr );
    EXPECT_TRUE( HasEdge( result, 0, 1, DependencyKind::ReadAfterWrite ) );
    EXPECT_FALSE( BarriersOn( result.FindPass( "Composite" ), lights.Index ).empty() )
         << "the reader gets the CopyDst -> StorageRead barrier";

    RecordingBackend backend;
    ASSERT_TRUE( graph.Execute( backend ).IsSuccess() );
    const auto at = [&]( const std::string& call )
    { return std::find( backend.Calls.begin(), backend.Calls.end(), call ) - backend.Calls.begin(); };
    const auto upload = at( std::format( "UploadBuffer {} 16 first 7", lights.Index ) );
    ASSERT_LT( upload, static_cast<std::ptrdiff_t>( backend.Calls.size() ) ) << "the copied bytes were uploaded";
    EXPECT_LT( upload, at( std::format( kBeginPassFormat, "Composite" ) ) );
}

// A graph buffer nothing uploaded or produced is refused BY NAME in the reader (here the upload comes after the
// reader, which is the same mistake: the graph orders by AddPass, so the reader saw no writer).
TEST( RenderGraphCompile, ABufferReadWithNoUploadOrProducerIsRefusedByName )
{
    ExternalTexture  backbuffer( Tex2D( 32, 32, ImageFormat::BGRA8F ), Access::None );
    Builder          graph( "noupload" );
    const BufferRef  lights = graph.CreateBuffer( BufferDesc{ 64 }, "Lights" );
    const TextureRef back   = graph.RegisterExternal( backbuffer, "Backbuffer" );
    graph.AddPass(
         "Composite", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( lights, Access::StorageRead );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         Ok );
    const std::vector<std::byte> bytes( 16, std::byte{ 1 } );
    graph.QueueBufferUpload( lights, bytes );

    const std::string fault = OnlyDeclarationFault( graph );
    EXPECT_EQ( fault.rfind( "Composite: ", 0 ), 0u ) << fault;
    EXPECT_NE( fault.find( "Lights" ), std::string::npos ) << fault;
    EXPECT_NE( fault.find( "before any pass writes it" ), std::string::npos ) << fault;
}

// An upload that does not fit is refused by name, as a fault of the upload; its reader goes with it (Dependency),
// never reading a half-written buffer. A size off the 4-byte transfer granularity is refused the same way.
TEST( RenderGraphCompile, ABufferUploadLargerThanItsBufferIsRefusedByName )
{
    const auto build = []( size_t size, ExternalTexture& backbuffer )
    {
        auto                         graph  = std::make_unique<Builder>( "oversize" );
        const BufferRef              lights = graph->CreateBuffer( BufferDesc{ 16 }, "Lights" );
        const TextureRef             back   = graph->RegisterExternal( backbuffer, "Backbuffer" );
        const std::vector<std::byte> bytes( size, std::byte{ 1 } );
        graph->QueueBufferUpload( lights, bytes );
        graph->AddPass(
             "Composite", PassFlags::Raster,
             [&]( PassBuilder& pass )
             {
                 pass.Read( lights, Access::StorageRead );
                 pass.ColorTarget( 0, back, LoadOp::DontCare() );
             },
             Ok );
        return graph;
    };
    const auto faultOf = []( const CompileResult& result, std::string_view pass ) -> const PassFault*
    {
        const auto it = std::find_if( result.Faults.begin(), result.Faults.end(),
                                      [&]( const PassFault& fault ) { return fault.PassName == pass; } );
        return it == result.Faults.end() ? nullptr : &*it;
    };

    ExternalTexture backbuffer( Tex2D( 32, 32, ImageFormat::BGRA8F ), Access::None );
    {
        const CompileResult result = CompileOrFail( *build( 32, backbuffer ) );
        const PassFault*    upload = faultOf( result, "Upload: Lights" );
        ASSERT_NE( upload, nullptr );
        EXPECT_EQ( upload->Stage, PassFaultStage::Declaration );
        EXPECT_NE( upload->Reason.find( "upload of 32 bytes into buffer 'Lights' of 16 bytes" ),
                   std::string::npos )
             << upload->Reason;
        const PassFault* reader = faultOf( result, "Composite" );
        ASSERT_NE( reader, nullptr );
        EXPECT_EQ( reader->Stage, PassFaultStage::Dependency );
    }
    {
        const CompileResult result = CompileOrFail( *build( 6, backbuffer ) );
        const PassFault*    upload = faultOf( result, "Upload: Lights" );
        ASSERT_NE( upload, nullptr );
        EXPECT_NE( upload->Reason.find( "not a multiple of 4 bytes" ), std::string::npos ) << upload->Reason;
    }
    {
        const CompileResult result = CompileOrFail( *build( 16, backbuffer ) );
        EXPECT_TRUE( result.Faults.empty() ) << "an exact fit is accepted";
    }
}

// A culled pass records nothing and gets no barrier: the backend never sees it, and no barrier anywhere in the
// plan (per pass or final) touches the resource only it wrote. Its reads do not extend a lifetime either: the
// range of a texture a live and a culled pass both read ends at the live one.
TEST( RenderGraphCompile, CulledPassesGetNoBarriersRecordNothingAndShrinkLifetimes )
{
    ExternalTexture  backbuffer( Tex2D( 32, 32, ImageFormat::BGRA8F ), Access::None );
    Builder          graph( "nobarriers" );
    const TextureRef lit  = graph.CreateTexture( Tex2D( 32, 32, ImageFormat::RGBA16F ), "Lit" );
    const TextureRef dead = graph.CreateTexture( Tex2D( 32, 32, ImageFormat::RGBA16F ), "Dead" );
    const TextureRef back = graph.RegisterExternal( backbuffer, "Backbuffer" );

    graph.AddPass(
         "Light", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( lit, Access::StorageWrite ); }, Ok );
    graph.AddPass(
         "Present", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( lit, Access::SampledGraphics );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         Ok );
    graph.AddPass(
         "Dead", PassFlags::Compute,
         [&]( PassBuilder& pass )
         {
             pass.Read( lit, Access::SampledCompute );
             pass.Write( dead, Access::StorageWrite );
         },
         Ok );

    const CompileResult result = CompileOrFail( graph );
    EXPECT_EQ( result.CulledPassNames, std::vector<std::string>{ "Dead" } );
    auto touchesDead = [&]( const Barrier& barrier ) { return barrier.Resource == dead.Index; };
    for ( const CompiledPass& pass : result.Passes )
        EXPECT_TRUE( std::none_of( pass.Barriers.begin(), pass.Barriers.end(), touchesDead ) ) << pass.Name;
    EXPECT_TRUE( std::none_of( result.FinalBarriers.begin(), result.FinalBarriers.end(), touchesDead ) );
    // Lit: first written by Light (AddPass 0), last read by Present (1) - not by the culled Dead (2).
    const auto lifetime = std::find_if( result.Lifetimes.begin(), result.Lifetimes.end(),
                                        [&]( const ResourceLifetime& l ) { return l.Resource == lit.Index; } );
    ASSERT_NE( lifetime, result.Lifetimes.end() );
    EXPECT_EQ( lifetime->FirstPass, 0u );
    EXPECT_EQ( lifetime->LastPass, 1u );
    EXPECT_EQ( lifetime->FirstPosition, 0u );
    EXPECT_EQ( lifetime->LastPosition, 1u );

    RecordingBackend backend;
    ASSERT_TRUE( graph.Execute( backend ).IsSuccess() );
    for ( const std::string& call : backend.Calls )
    {
        EXPECT_EQ( call.find( std::format( kBeginPassFormat, "Dead" ) ), std::string::npos ) << call;
        EXPECT_EQ( call.find( std::format( kEndPassFormat, "Dead" ) ), std::string::npos ) << call;
    }
}

// The debug switch is real: with culling off every pass runs (the one nothing reads included), CulledPasses is
// empty and the result says culling was off; the frame sets it from DebugViewState, and the viewport edits it.
TEST( RenderGraphCompile, TheCullingSwitchKeepsEveryPassAndTheFrameAndViewportWireIt )
{
    auto build = []( Builder& graph, ExternalTexture& backbuffer, std::vector<std::string>& ran )
    {
        const TextureRef orphan = graph.CreateTexture( Tex2D( 16, 16, ImageFormat::RGBA8F ), "Orphan" );
        const TextureRef back   = graph.RegisterExternal( backbuffer, "Backbuffer" );
        graph.AddPass(
             "Orphan", PassFlags::Compute,
             [&, orphan]( PassBuilder& pass ) { pass.Write( orphan, Access::StorageWrite ); },
             [&ran]( PassContext& )
             {
                 ran.emplace_back( "Orphan" );
                 return Common::MakeSuccess( true );
             } );
        graph.AddPass(
             "Present", PassFlags::Raster,
             [&, back]( PassBuilder& pass ) { pass.ColorTarget( 0, back, LoadOp::ClearColor( 0, 0, 0, 1 ) ); },
             [&ran]( PassContext& )
             {
                 ran.emplace_back( "Present" );
                 return Common::MakeSuccess( true );
             } );
    };

    ExternalTexture          culledBack( Tex2D( 16, 16, ImageFormat::BGRA8F ), Access::None );
    std::vector<std::string> culledRan;
    Builder                  culled( "switch-on" );
    build( culled, culledBack, culledRan );
    EXPECT_TRUE( culled.IsPassCullingEnabled() );
    const CompileResult on = CompileOrFail( culled );
    EXPECT_TRUE( on.PassCulling );
    EXPECT_EQ( on.CulledPassNames, std::vector<std::string>{ "Orphan" } );
    ASSERT_TRUE( ExecuteRecorded( culled ).IsSuccess() );
    EXPECT_EQ( culledRan, std::vector<std::string>{ "Present" } );

    ExternalTexture          keptBack( Tex2D( 16, 16, ImageFormat::BGRA8F ), Access::None );
    std::vector<std::string> keptRan;
    Builder                  kept( "switch-off" );
    kept.SetPassCulling( false );
    build( kept, keptBack, keptRan );
    const CompileResult off = CompileOrFail( kept );
    EXPECT_FALSE( off.PassCulling );
    EXPECT_TRUE( off.CulledPasses.empty() );
    EXPECT_TRUE( off.CulledPassNames.empty() );
    ASSERT_EQ( off.Passes.size(), 2u );
    ASSERT_TRUE( ExecuteRecorded( kept ).IsSuccess() );
    EXPECT_EQ( keptRan, ( std::vector<std::string>{ "Orphan", "Present" } ) );

    // The seam: the view's graph takes the switch from the pushed DebugViewState, and the viewport's Show menu
    // is what sets it (whitespace-insensitive, like the frame-order census).
    auto stripped = []( const fs::path& path )
    {
        std::ifstream     file( path.string() );
        std::stringstream text;
        text << file.rdbuf();
        std::string out;
        for ( char c : text.str() )
            if ( !std::isspace( static_cast<unsigned char>( c ) ) )
                out.push_back( c );
        return out;
    };
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() );
    EXPECT_NE( stripped( root / "Desert/Desert/Source/Engine/Graphic/SceneRenderer.cpp" )
                    .find( "graph.SetPassCulling(!m_DebugView.DisablePassCulling);" ),
               std::string::npos );
    EXPECT_NE( stripped( root / "Editor/Source/Editor/Panels/ViewportPanel/ViewportPanel.cpp" )
                    .find( "ImGui::Checkbox(\"DisablePassCulling\",&view.DisablePassCulling)" ),
               std::string::npos );
}

// ── Barriers ────────────────────────────────────────────────────────────────────────────────────────────

// Bloom: pass i writes mip i while sampling mip i-1 of the SAME texture. Per-subresource state is what
// makes that legal - one barrier moves mip i-1 to shader-read, another brings mip i up from undefined.
// THE ACQUIRED BACK BUFFER'S FIRST BARRIER CHAINS WITH THE ACQUIRE WAIT. The frame waits on the acquire semaphore
// at kPresentAcquiredState's stages (VulkanSwapChainOutput::GetFrameOutput) and the back buffer is imported in
// that state (VulkanSwapChain::ImportBackBuffer). The graph's transition out of Undefined must have those stages
// in its source scope, or it is ordered against nothing and races the presentation engine's read: imported as
// Access::None it was TOP_OF_PIPE, and the validation layer reported SYNC-HAZARD-WRITE-AFTER-READ in the
// EditorImGui region on every frame (GATE-VAL1).
TEST( RenderGraphCompile, TheAcquiredBackBuffersFirstBarrierWaitsAtTheAcquireStage )
{
    EXPECT_EQ( kPresentAcquiredState.Layout, ImageLayout::Undefined );
    EXPECT_EQ( kPresentAcquiredState.Memory, static_cast<MemoryAccessFlags>( MemoryAccess_None ) );
    EXPECT_EQ( kPresentAcquiredState.Stages,
               static_cast<PipelineStageFlags>( PipelineStage_ColorAttachmentOutput ) );

    ExternalTexture backbuffer;
    backbuffer.Desc = Tex2D( 64, 64, ImageFormat::BGRA8F );
    backbuffer.SubresourceStates.assign( backbuffer.Desc.SubresourceCount(), kPresentAcquiredState );
    Builder          graph( "EditorImGui" );
    const TextureRef back = graph.RegisterExternal( backbuffer, "BackBuffer" );
    graph.AddPass(
         "EditorImGui", PassFlags::Raster, [&]( PassBuilder& pass )
         { pass.ColorTarget( 0, back, LoadOp::ClearColor( 0.1f, 0.1f, 0.1f, 1.0f ) ); }, Ok );
    graph.Extract( back, backbuffer, Access::Present );

    const CompileResult        result = CompileOrFail( graph );
    const std::vector<Barrier> first  = BarriersOn( result.FindPass( "EditorImGui" ), back.Index );
    ASSERT_EQ( first.size(), 1u );
    EXPECT_EQ( first[0].Before.Layout, ImageLayout::Undefined );
    EXPECT_NE( first[0].Before.Stages & kPresentAcquiredState.Stages, 0u )
         << "the transition's source scope does not include the stage the acquire semaphore is waited at";
    EXPECT_EQ( first[0].After, GetAccessState( Access::ColorTarget ) );
}

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

    // RDG-FAULT1: the refused GetTexture fails Tonemap's exec - a late fault of Tonemap, not of the frame.
    ASSERT_TRUE( ExecuteRecorded( graph ).IsSuccess() );
    const ExecuteReport& report = graph.GetExecuteReport();
    ASSERT_EQ( report.Faults.size(), 1u );
    EXPECT_EQ( report.Faults[0].PassName, "Tonemap" );
    EXPECT_EQ( report.Faults[0].Stage, PassFaultStage::Execution );
    EXPECT_NE( report.Faults[0].Reason.find( "Sneaky" ), std::string::npos ) << report.Faults[0].Reason;
    EXPECT_NE( wrongAccess.find( "StorageRead" ), std::string::npos ) << wrongAccess;
#else
    // Shipping only (DESERT_CONFIG_SHIPPING): Debug and Release define DESERT_DEV_INSTRUMENTS 1 and run the test.
    GTEST_SKIP() << "the declaration check is a development instrument, compiled out of Shipping";
#endif
}

TEST( RenderGraphCompile, MalformedDeclarationsFaultTheirPassWithNames )
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
        const std::string fault = OnlyDeclarationFault( graph );
        ASSERT_FALSE( fault.empty() );
        EXPECT_NE( fault.find( "Reader" ), std::string::npos ) << fault;
        EXPECT_NE( fault.find( "NeverWritten" ), std::string::npos ) << fault;
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
        const std::string fault = OnlyDeclarationFault( graph );
        ASSERT_FALSE( fault.empty() );
        EXPECT_NE( fault.find( "Both" ), std::string::npos ) << fault;
        EXPECT_NE( fault.find( "Target" ), std::string::npos ) << fault;
    }
    {
        Builder          graph( "kinds" );
        const TextureRef t = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F, 3 ), "Chain" );
        graph.AddPass(
             "ComputeWithAttachment", PassFlags::Compute,
             [&]( PassBuilder& pass ) { pass.ColorTarget( 0, t, LoadOp::DontCare() ); }, Ok );
        const std::string fault = OnlyDeclarationFault( graph );
        ASSERT_FALSE( fault.empty() );
        EXPECT_NE( fault.find( "ComputeWithAttachment" ), std::string::npos ) << fault;
    }
    {
        Builder          graph( "range" );
        const TextureRef t = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F, 3 ), "Chain" );
        graph.AddPass(
             "OutOfRange", PassFlags::Compute,
             [&]( PassBuilder& pass ) { pass.Write( t, Access::StorageWrite, SubresourceRange::Mip( 3 ) ); }, Ok );
        const std::string fault = OnlyDeclarationFault( graph );
        ASSERT_FALSE( fault.empty() );
        EXPECT_NE( fault.find( "mip 3" ), std::string::npos ) << fault;
    }
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

// RDG-FAULT1: a failing exec is a late fault of its pass. The graph is not abandoned (AbandonGraph is for backend
// failures only): the pass's render pass is ended, the frame goes on, and the fault is in the execute report
// naming the pass and its error.
TEST( RenderGraphCompile, AFailingPassIsALateFaultNotAnAbandonedGraph )
{
    ExternalTexture  out( Tex2D( 64, 64, ImageFormat::RGBA8F ), Access::None );
    RecordingBackend backend;
    Builder          graph( "failing" );
    const TextureRef o = graph.RegisterExternal( out, "Out" );
    graph.AddPass(
         "Broken", PassFlags::Raster, [&]( PassBuilder& pass ) { pass.ColorTarget( 0, o, LoadOp::DontCare() ); },
         []( PassContext& ) -> Common::BoolResultStr { return Common::MakeError( "pipeline missing" ); } );
    ASSERT_TRUE( graph.Execute( backend ).IsSuccess() );
    EXPECT_EQ( std::count( backend.Calls.begin(), backend.Calls.end(), "AbandonGraph" ), 0 );
    EXPECT_EQ( std::count( backend.Calls.begin(), backend.Calls.end(), "EndRenderPass" ), 1 );
    const ExecuteReport& report = graph.GetExecuteReport();
    ASSERT_EQ( report.Faults.size(), 1u );
    EXPECT_EQ( report.Faults[0].PassName, "Broken" );
    EXPECT_EQ( report.Faults[0].Stage, PassFaultStage::Execution );
    EXPECT_NE( report.Faults[0].Reason.find( "pipeline missing" ), std::string::npos ) << report.Faults[0].Reason;
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

    TypedProvider                          provider;
    const Common::ResultStr<CompileResult> compiled = graph.Compile( provider );
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
    // B's lifetime is disjoint from A's, but no memory type holds both: it gets a heap of its own.
    EXPECT_EQ( allocA->Offset, 0u );
    EXPECT_NE( allocB->Heap, allocA->Heap );
    EXPECT_EQ( allocB->Offset, 0u );
    EXPECT_EQ( allocC->Heap, allocA->Heap );
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
    const Common::ResultStr<CompileResult> refused = buffers.Compile( provider );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "Scratch" ), std::string::npos ) << refused.GetError();
}

// WHICH ORDER SceneRenderer ADDS. The pass sequence of SceneRenderer::OnUpdate, read from the source: every
// graph node call (graph.AddPass, AddRaster, the DeferredFrameNodes declarations it calls) by its name, every
// system's compute nodes by the declaring call, every system raster pass by the getter that hands it over (a
// getter returning several, ShadowCascadePasses, adds one node per element in its order) and every
// AddSystemRasters call by its list and clear mode (its passes are the getters named before it). A node added only
// at some sample counts (Deferred: DepthResolve at 1x, Deferred: DepthExpand and Scene: DepthResolve at MSAA) is
// listed where its call stands. The table is the frame order before RDG3 (c303909f9, SceneRenderer::OnUpdate):
// its DESERT_PROFILE_PASS scopes and direct calls in sequence, ExecuteRenderGraph = every phase but the deferred
// overlays, then ExecuteTransparency, ExecuteDebugOverlay and ExecuteUI one phase each. Moving a pass changes
// the picture and has to change the table on purpose.
TEST( RenderGraphCompile, SceneRendererAddsItsPassesInTheFrameOrder )
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
    // The deferred frame's node declarations are templates in DeferredFrameNodes.hpp (RenderGraphCompile
    // compiles them device-free); an AddFrame member that calls DeferredFrameNodes::Add<X> adds the nodes of
    // that body, in its order.
    std::string nodesSource;
    {
        std::ifstream file( root / "Desert/Desert/Source/Engine/Graphic/DeferredFrameNodes.hpp" );
        ASSERT_TRUE( file ) << "DeferredFrameNodes.hpp is gone";
        std::string line;
        while ( std::getline( file, line ) )
        {
            const size_t comment = line.find( "//" );
            std::format_to( std::back_inserter( nodesSource ), "{}\n",
                            comment == std::string::npos ? line : line.substr( 0, comment ) );
        }
    }
    const auto nodesBodyOf = [&nodesSource]( std::string_view function ) -> std::string
    {
        const size_t begin = nodesSource.find( std::format( "void {}(", function ) );
        if ( begin == std::string::npos )
            return {};
        const size_t end = std::min( nodesSource.find( "template <", begin ), nodesSource.find( "\n}", begin ) );
        return nodesSource.substr( begin, end == std::string::npos ? std::string::npos : end - begin );
    };
    const auto bodyOf = [&source]( std::string_view function ) -> std::string
    {
        // A definition returns void, or the graph handle it made (AddFrameBackdropBlur hands the UI its pyramid).
        const auto definition = [&source]( std::string_view name, size_t from )
        {
            return std::min( { source.find( std::format( "void SceneRenderer::{}", name ), from ),
                               source.find( std::format( "RDG::TextureRef SceneRenderer::{}", name ), from ),
                               source.find( std::format( "OverlayTargets SceneRenderer::{}", name ), from ) } );
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
            const size_t pass   = text.find( "graph.AddPass(", at );
            const size_t frame  = text.find( "AddFrame", at );
            size_t       raster = text.find( "AddRaster(", at );
            // A system's compute nodes (AddComputeNodes): the entry names the system call that declares them.
            const size_t compute = text.find( "AddComputeNodes(", at );
            // A DeferredFrameNodes declaration: followed into its body in DeferredFrameNodes.hpp.
            const size_t deferred = text.find( "DeferredFrameNodes::Add", at );
            // TAA1-B: the view's temporal upscaler declares its own nodes (ITemporalUpscaler::AddPasses); the
            // entry names the member that holds it.
            const size_t temporal = text.find( "->AddPasses(", at );
            // ARCH1b: a system's raster pass, placed by the position of the call to the getter that hands it over
            // (`->XPass()` / `->XPasses()`, no arguments); the entry names the receiver, when it is a variable,
            // and the getter.
            const auto isGetter = [&text]( size_t arrow )
            {
                size_t end = arrow + 2;
                while ( end < text.size() &&
                        ( std::isalnum( static_cast<unsigned char>( text[end] ) ) || text[end] == '_' ) )
                    ++end;
                const std::string_view name( text.data() + arrow + 2, end - arrow - 2 );
                if ( !name.ends_with( "Pass" ) && !name.ends_with( "Passes" ) )
                    return false;
                size_t open = end;
                while ( open < text.size() && std::isspace( static_cast<unsigned char>( text[open] ) ) )
                    ++open;
                size_t close = open + 1;
                while ( close < text.size() && std::isspace( static_cast<unsigned char>( text[close] ) ) )
                    ++close;
                return close < text.size() && text[open] == '(' && text[close] == ')';
            };
            size_t systemRaster = text.find( "->", at );
            while ( systemRaster != std::string::npos && !isGetter( systemRaster ) )
                systemRaster = text.find( "->", systemRaster + 1 );
            // ARCH1b-4: a list of system raster passes added in order (AddSystemRasters); the entry names the list
            // and the clear mode. The member's own definition (its parameters are not `graph, textures,`) is
            // skipped.
            size_t rasters = text.find( "AddSystemRasters(", at );
            while ( rasters != std::string::npos &&
                    squeeze( text.substr( rasters, 48 ) ).rfind( "AddSystemRasters(graph,textures,", 0 ) != 0 )
                rasters = text.find( "AddSystemRasters(", rasters + 1 );
            // ARCH1b-2: an extension point invoked at its place (AddExtensionPoint): the entry names the point.
            // The member's own definition names no point before its first ';' and is skipped.
            size_t extension = text.find( "AddExtensionPoint(", at );
            while ( extension != std::string::npos &&
                    text.find( "RDG::ExtensionPoint::", extension ) > text.find( ';', extension ) )
                extension = text.find( "AddExtensionPoint(", extension + 1 );
            // A graph node: its name is the first string literal of the call (a std::format loop name keeps
            // its "{}", one entry per call site).
            size_t node = text.find( "graph.AddPass(", at );
            while ( node != std::string::npos && text.find( '"', node ) > text.find( ')', node ) )
                node = text.find( "graph.AddPass(", node + 1 );
            // A call names its node first (a quote before the call's first ')'); the helper's definition does not.
            while ( raster != std::string::npos && text.find( '"', raster ) > text.find( ')', raster ) )
                raster = text.find( "AddRaster(", raster + 1 );
            const size_t first = std::min(
                 { pass, frame, raster, node, compute, deferred, temporal, systemRaster, rasters, extension } );
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
            else if ( first == extension )
            {
                const size_t name = text.find( "RDG::ExtensionPoint::", extension ) +
                                    std::string_view( "RDG::ExtensionPoint::" ).size();
                size_t stop = name;
                while ( stop < text.size() &&
                        ( std::isalnum( static_cast<unsigned char>( text[stop] ) ) || text[stop] == '_' ) )
                    ++stop;
                added.push_back( std::format( "extension[{}]", text.substr( name, stop - name ) ) );
                at = stop;
            }
            else if ( first == systemRaster )
            {
                size_t receiver = systemRaster;
                while ( receiver > 0 && ( std::isalnum( static_cast<unsigned char>( text[receiver - 1] ) ) ||
                                          text[receiver - 1] == '_' ) )
                    --receiver;
                // No variable before the arrow (a macro call's result): the getter alone.
                if ( receiver == systemRaster )
                    receiver += 2;
                const size_t close = text.find( ')', systemRaster );
                added.push_back(
                     std::format( "system[{}]", squeeze( text.substr( receiver, close + 1 - receiver ) ) ) );
                at = close + 1;
            }
            else if ( first == rasters )
            {
                size_t close = text.find( '(', rasters );
                for ( int depth = 0; close < text.size(); ++close )
                {
                    depth += text[close] == '(' ? 1 : text[close] == ')' ? -1 : 0;
                    if ( depth == 0 )
                        break;
                }
                ASSERT_LT( close, text.size() ) << "unbalanced AddSystemRasters call";
                const std::string      call   = squeeze( text.substr( rasters, close + 1 - rasters ) );
                const std::string_view prefix = "AddSystemRasters(graph,textures,";
                added.push_back(
                     std::format( "rasters[{}]", call.substr( prefix.size(), call.size() - prefix.size() - 1 ) ) );
                at = close + 1;
            }
            else if ( first == temporal )
            {
                size_t holder = temporal;
                while ( holder > 0 && ( std::isalnum( static_cast<unsigned char>( text[holder - 1] ) ) ||
                                        text[holder - 1] == '_' ) )
                    --holder;
                added.push_back( std::format( "temporal[{}]", text.substr( holder, temporal - holder ) ) );
                at = temporal + 1;
            }
            else if ( first == deferred )
            {
                const size_t      name   = deferred + std::string_view( "DeferredFrameNodes::" ).size();
                const size_t      open   = text.find( '(', name );
                const std::string callee = text.substr( name, open - name );
                const std::string called = nodesBodyOf( callee );
                ASSERT_FALSE( called.empty() ) << "no definition of DeferredFrameNodes::" << callee;
                collect( called, called.find( '(' ) + 1 );
                at = open + 1;
            }
            else if ( first == compute )
            {
                // The call ends at ITS matching ')', not at the statement's ';': a caller may wrap it, as
                // SettleShadowMapNodes( AddComputeNodes( ... ) ) does.
                size_t close = text.find( '(', compute );
                ASSERT_NE( close, std::string::npos );
                for ( int depth = 0; close < text.size(); ++close )
                {
                    depth += text[close] == '(' ? 1 : text[close] == ')' ? -1 : 0;
                    if ( depth == 0 )
                        break;
                }
                ASSERT_LT( close, text.size() ) << "unbalanced AddComputeNodes call";
                std::string            call   = squeeze( text.substr( compute, close + 1 - compute ) );
                const std::string_view prefix = "AddComputeNodes(graph,textures,";
                ASSERT_EQ( call.rfind( prefix, 0 ), 0u ) << call;
                added.push_back(
                     std::format( "compute[{}]", call.substr( prefix.size(), call.size() - prefix.size() - 1 ) ) );
                at = close + 1;
            }
            else
            {
                // graph.AddPass / AddRaster: the node's name literal.
                const size_t open  = text.find( '"', first );
                const size_t close = text.find( '"', open + 1 );
                ASSERT_NE( close, std::string::npos );
                added.push_back( text.substr( open + 1, close - open - 1 ) );
                at = close + 1;
            }
        }
    };
    collect( body, 0 );

    const std::vector<std::string> frameOrder = {
         "ClearMainFramebuffer",
         // VFX-07b: the scene's particle pool - compact 0, then per fixed step Dispatch Args, Spawn+Update, the
         // next compact.
         "Particles: Compact 0",
         "Particles: Dispatch Args {}",
         "Particles: Spawn+Update {}",
         "Particles: Compact {}",
         "compute[clouds->DeclareShadowMapNodes()]",
         // ARCH1b-4: the opaque raster of the systems by explicit calls, in the order the phase walk drew it:
         // the shadow cascades (each clearing its cascade, 0 first), then ONE clearing sequence on the scene
         // target - the sky CLEARS it, the meshes then the terrain LOAD over it - then the outline silhouette
         // mask (cleared).
         "system[mesh->ShadowCascadePasses()]",
         "rasters[cascades,true]",
         "system[sky->SkyPass()]",
         "system[mesh->GeometryPass()]",
         "system[terrain->GeometryPass()]",
         "rasters[passes,true]",
         "system[mesh->SilhouettePass()]",
         "rasters[std::span<constSystemRasterPass>(&silhouette,1),true]",
         "Deferred: GBuffer",
         "TerrainGBuffer",
         "Deferred: DepthResolve",
         "Deferred: DepthExpand",
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
         "Scene: DepthResolve",
         "compute[sky->DeclareAtmosphereLutNodes()]",
         "compute[fog->DeclareFrameNodes(graph,textures.Transients)]",
         "compute[clouds->DeclareFrameNodes(graph,frame)]",
         // ARCH1b-2: passes from outside the engine join at named extension points, each invoked at its place.
         "extension[AfterOpaque]",
         // ARCH1b: the translucency in draw order by call order (AddFrameTranslucency), no numeric placement:
         // the height fog apply lands on the opaque scene first, the far field (cloud composite) over it, then
         // everything nearer the camera (particles, the editor's passes) over both.
         "system[ApplyPass()]",
         "system[CompositePass()]",
         "system[particles->DrawPass()]",
         "extension[AfterTranslucency]",
         "Debug: Overdraw",
         "Debug: Overdraw Resolve",
         // TAA1-B: the temporal resolve, after the last velocity writer (the translucency) and before the
         // overlays, which draw into its output.
         "temporal[m_TemporalUpscaler]",
         // TAA1-B 6: the output-extent overlay depth, filled from the render-extent scene depth, before the
         // overlays that test against it.
         "Scene: PopulateSceneDepth",
         "Debug: Velocity",
         // The engine's debug lines (bounding boxes), the first overlay, below the editor's.
         "system[mesh->DebugLinesPass()]",
         "extension[Overlay]",
         "UI: BackdropBlur{}",
         "extension[UI]",
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
    EXPECT_EQ( added, frameOrder );

    // RDG-LEG1-L2: the deferred passes are graph nodes with declared accesses.
    const auto declares = [&]( std::string_view function, std::initializer_list<std::string_view> needles )
    {
        const std::string text = squeeze( bodyOf( function ) );
        ASSERT_FALSE( text.empty() ) << function;
        for ( const std::string_view needle : needles )
            EXPECT_NE( text.find( needle ), std::string::npos ) << function << " does not declare " << needle;
    };
    declares( "AddFrameClearMainFramebuffer", { "PassFlags::Raster", "ColorTarget(", "LoadOp::ClearDepth(" } );
    // RDG-FAULT1: the sampled reads live in the system's binding block (SSAORenderer::DeclareBindings,
    // DeferredLightingRenderer::DeclareCompositeBindings declare them Access::SampledGraphics); the node
    // delegates.
    declares( "AddFrameSSAO",
              { "PassFlags::Raster", "ssao->DeclareBindings(pass,depth,normal)", "ColorTarget(0,ao," } );
    // TAA1-B: the temporal resolve reads the scene colour, the scene depth, the frame's velocity, the previous
    // adapted luminance and the registered history, and hands its output back as the post input.
    declares( "AddFrameTemporal",
              { "m_ViewState.History().Register(graph)", ".SceneColor=textures.Import(m_TargetFramebuffer->",
                ".SceneDepth=textures.Depth(m_TargetFramebuffer,", ".Velocity=textures.Transients.Velocity",
                ".History=histories", "m_TemporalUpscaler->AddPasses(graph,frame,inputs)",
                "resolvedColor=added.GetValue().SceneColor;", "overlay.Color=resolvedColor;", "returnoverlay;" } );
    // SCAL-SPATIAL1: below 100 % without a temporal method the spatial upscale is the resolve, at the same point
    // and with no history; the sharpen (Resolution.Sharpness) follows any resolve, on its output.
    declares( "AddFrameTemporal",
              { "constboolspatial=IsSpatialUpscale(frame);",
                "temporal?m_ViewState.History().Register(graph):std::vector<HistoryRefs>{}",
                "m_SpatialUpscale.AddPasses(graph,frame,inputs.SceneColor)", "resolvedColor=upscaled.GetValue();",
                "m_Quality.As<int>(Common::Scalability::Parameter::UpscalerSharpness)",
                "if(SharpenRuns(frame,sharpness))", "m_Sharpen.AddPasses(graph,frame,resolvedColor,sharpness)",
                "resolvedColor=sharpened.GetValue();", "returnwithoutTemporal(upscaled.GetError());",
                "returnwithoutTemporal(sharpened.GetError());" } );
    // TAA1-B 6: above 100 % (Split.Mode == Supersample) the SSAA downsample follows the temporal output, or runs
    // alone on the scene colour without a temporal method, and its output is the resolved colour.
    declares( "AddFrameTemporal",
              { "constboolsupersample=frame.Split.Mode==Common::Scalability::ScaleMode::Supersample;",
                "if(!m_TargetFramebuffer||(!spatial&&!supersample&&!temporal))", "resolvedColor=inputs.SceneColor;",
                "m_SupersampleResolve.AddPasses(graph,frame,resolvedColor)", "resolvedColor=downsampled.GetValue();",
                "returnwithoutTemporal(downsampled.GetError());" } );
    // TAA1-B 6: the overlay target set is at the OUTPUT extent and its depth is the scene depth populated by
    // "Scene: PopulateSceneDepth"; a frame the resolve cannot run on is rendered without it, by name, and the
    // caller then post-processes the scene colour (the fallback is the caller's, not a silent skip).
    declares( "AddFrameTemporal",
              { "RDG::Extent3D{frame.Split.Output.Width,frame.Split.Output.Height,1}",
                "graph.CreateTexture(desc,\"Overlay.Velocity\")",
                "graph.CreateTexture(desc,\"Overlay.SceneDepth\")",
                "populate->DeclareBindings(pass,inputs.SceneDepth)",
                "pass.ColorTarget(0,overlay.Velocity,RDG::LoadOp::ClearColor(",
                "pass.DepthTarget(overlay.Depth,RDG::LoadOp::ClearDepth(Core::kDepthClear),",
                "renderedwithoutthetemporalresolvethisframe:", "returnwithoutTemporal(added.GetError());",
                "returnwithoutTemporal(prepared.GetError());" } );
    declares( "OnUpdate",
              { "overlay.IsValid()?std::vector<RDG::TextureRef>{overlay.Color}:sceneColor()",
                "AddSystemRaster(graph,textures,mesh->DebugLinesPass(),overlay);",
                "AddExtensionPoint(graph,textures,RDG::ExtensionPoint::Overlay,overlay);",
                "AddExtensionPoint(graph,textures,RDG::ExtensionPoint::UI,overlay);",
                // The one resolution function, the render set resized to the frame's split, the velocity at it.
                "ResolveViewResolution(m_ViewExtent,m_Quality.As<int>(Parameter::RenderScalePercent),m_DebugView."
                "ScreenPercentage,",
                // The frame's upscaler is the VIEW's (a viewport override at 50 % under a 100 % setting is TAAU).
                "inputs.Upscaler=resolved.GetValue().Upscaler;", "ResizeRenderTargets(frame.Split.Render);",
                "RDG::Extent3D{frame.Split.Render.Width,frame.Split.Render.Height,1}" } );
    // The overlays draw into the overlay set: every scene-target attachment replaced, no resolves.
    declares( "AddPassNode",
              { "targets->Colors[0]=overlay.Color;", "targets->Colors[kSceneTargetVelocitySlot]=overlay.Velocity;",
                "targets->Depth=overlay.Depth;", "targets->Resolves={};" } );
    // TWO EXTENT SETS: ResizeRenderTargets sizes the Render set (scene target, G-buffer, depth resolve, mask,
    // overdraw, outline); Resize sizes the Output set (tonemap, FXAA, SMAA) and hands the render set its split.
    declares( "ResizeRenderTargets",
              { "m_TargetFramebuffer->Resize(width,height);", "m_GBuffer->Resize(width,height);",
                "resolve->Resize(width,height);", "maskFb->Resize(width,height);",
                "overdrawFb->Resize(width,height);", "->OnResize(width,height);" } );
    declares( "Resize", { "m_RenderSystems[\"TonemapSystem\"])->Resize(width,height);",
                          "m_RenderSystems[\"FXAASystem\"])->Resize(width,height);",
                          "m_RenderSystems[\"SMAASystem\"])->Resize(width,height);",
                          "ResizeRenderTargets(split.GetValue().Render);" } );
    {
        const std::string resize = squeeze( bodyOf( "Resize" ) );
        EXPECT_EQ( resize.find( "m_TargetFramebuffer->Resize(" ), std::string::npos )
             << "Resize sizes the scene target itself: the render set is ResizeRenderTargets'";
        const std::string render = squeeze( bodyOf( "ResizeRenderTargets" ) );
        EXPECT_EQ( render.find( "TonemapSystem" ), std::string::npos )
             << "ResizeRenderTargets sizes the tonemap target: it is in the output set";
    }
    // The exposure dispatch covers the texture it reads, not the scene target's image.
    declares( "AddFrameAutoExposure", { "graph.GetTextureDesc(scene)" } );
    declares( "AddFrameGIResolve", { "PassFlags::Raster", "ColorTarget(0,gather,", "ColorTarget(0,accum," } );
    declares( "AddFrameComposite", { "PassFlags::Raster", "deferred->DeclareCompositeBindings(pass,inputs,lights)",
                                     "LoadTarget(pass,target,loads)" } );
    declares( "AddFrameSceneCopy", { "PassFlags::Raster", "Access::SampledGraphics", "ColorTarget(0,sceneCopy" } );
    // RDG-A2-W4: the SSR passes sample the G-buffer as graph refs by name, never a framebuffer image.
    // RDG-FAULT1: the trace's storage writes are its block's (SSRRenderer::DeclareTraceBindings); the execs read
    // their block, not refs captured from the setup. GBUF1: the third G-buffer input is the depth the positions
    // are reconstructed from. TAA1: the trace's inverse view-projection is a CPU-filled graph uniform buffer
    // (SSRTraceUB) bound by the trace block - the ViewFrame's one copy of the inverse, never recomputed.
    declares( "AddFrameSSR",
              { "PassFlags::Compute", "DeclareTraceBindings(pass,trace,tiles,inputs,sceneCopy,traceUniforms)",
                "System::SSRRenderer::UploadTraceUniforms(graph,frame.InvJitteredViewProjection)",
                "LoadTarget(pass,target,loads)",
                "GBufferInputsinputs{gbuffer[0],gbuffer[1],textures.Depth(m_GBuffer,\"GBuffer\")}",
                "ssr->DeclareResolveBindings(pass,trace,tiles,history,inputs)",
                "ssr->DeclareCompositeBindings(pass,accum,tiles,inputs)",
                "RecordResolve(context,prevViewProj,invViewProj,readable)", "RecordComposite(context,index)" } );
    // ... and those blocks declare the accesses the node used to declare itself.
    {
        const auto declaration = [&]( const char* header )
        {
            std::ifstream file( root / "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred" / header );
            EXPECT_TRUE( file ) << header << " is gone";
            return squeeze(
                 std::string( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() ) );
        };
        EXPECT_NE(
             declaration( "SSRRenderer.hpp" ).find( ".Storage(\"u_Trace\",trace,RDG::Access::StorageWrite)" ),
             std::string::npos );
        EXPECT_NE( declaration( "SSAORenderer.hpp" )
                        .find( ".Sampled(\"u_GBufferDepth\",depth,RDG::Access::SampledGraphics" ),
                   std::string::npos );
        EXPECT_NE( declaration( "DeferredLightingRenderer.hpp" )
                        .find( ".Sampled(\"u_SSAO\",inputs.SSAO,RDG::Access::SampledGraphics" ),
                   std::string::npos );
    }
}

// DepthResolve is a Copy node: G-buffer depth CopySrc -> target depth CopyDst, so the graph plans the barriers
// into TRANSFER_SRC / TRANSFER_DST before it (the old wrapper declared nothing and got none), and the
// G-buffer depth ends the graph in the attachment layout. The same declarations compiled on recorded images:
TEST( RenderGraphCompile, DepthResolveIsACopyNodeWithCopySrcCopyDstAndPlannedBarriers )
{
    const fs::path root = RepoRoot();
    std::ifstream  file( root / "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameDeferred.cpp" );
    ASSERT_TRUE( file );
    std::string text( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
    text.erase(
         std::remove_if( text.begin(), text.end(), []( unsigned char c ) { return std::isspace( c ) != 0; } ),
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
}

// THE POST-PROCESS AND BACKDROP PASSES ARE GRAPH NODES (RDG-LEG1-L4). Every pass SceneRendererFramePostFX.cpp
// adds is a Compute or Raster node whose setup declares what it samples and writes, so the graph places every
// barrier and opens every render pass, and neither the file nor the renderers it
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

        const std::string declarations = postFx.substr( setup, exec - setup );
        const bool        declares     = declarations.find( "pass.Read(" ) != std::string::npos ||
                              declarations.find( "pass.Write(" ) != std::string::npos ||
                              declarations.find( "pass.ColorTarget(" ) != std::string::npos ||
                              declarations.find( "ReadEach(" ) != std::string::npos ||
                              // a renderer's DeclareBindings declares the node's binding block (RDG-FAULT1)
                              declarations.find( "Bindings( pass" ) != std::string::npos;
        EXPECT_TRUE( declares || flags.find( "PassFlags::NeverCull" ) != std::string::npos )
             << name << " declares no access and is not a culling root";
        if ( flags.find( "RDG::PassFlags::Raster" ) != std::string::npos )
            EXPECT_NE( declarations.find( "pass.ColorTarget(" ), std::string::npos )
                 << name << " is a Raster node without a colour target: the graph has no render pass to open";
    }
    // JumpFlood 3, AutoExposure 3, Bloom 2, LightShafts 2, LensFlare 2, Tonemap 1, FXAA 1, SMAA 3, BackdropBlur 1.
    EXPECT_EQ( nodes, 18u );

    // The frame's post-FX recorder (already read above) and every renderer in the PostProcessing folder.
    constexpr std::string_view dir = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing";
    for ( const std::string_view file :
          { "SceneRendererFramePostFX.cpp", "JumpFloodOutlineRenderer.cpp", "LensFlareRenderer.cpp",
            "BackdropBlurRenderer.hpp", "BloomRenderer.cpp", "AutoExposureRenderer.cpp", "LightShaftRenderer.cpp",
            "TonemapRenderer.cpp", "FXAARenderer.cpp", "SMAARenderer.cpp" } )
    {
        const std::string text =
             file == "SceneRendererFramePostFX.cpp" ? postFx : read( std::format( "{}/{}", dir, file ) );
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

// VFX-08e. Depth-tested sprites that fade against the scene depth hold the depth as a READ-ONLY attachment and
// sample it in the same pass (UE: FExclusiveDepthStencil::DepthRead + the SceneDepth SRV). Both accesses fold into
// one use in DEPTH_STENCIL_READ_ONLY_OPTIMAL; a WRITTEN depth attachment that is also sampled stays refused.
TEST( RenderGraphCompile, AReadOnlyDepthAttachmentSampledInTheSamePassHoldsTheDepthReadOnlyLayout )
{
    ExternalTexture  backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
    Builder          graph( "depth read sampled" );
    const TextureRef depth = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::DEPTH32F ), "SceneDepth" );
    const TextureRef back  = graph.RegisterExternal( backbuffer, "Backbuffer" );
    graph.AddPass(
         "Opaque", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
             pass.DepthTarget( depth, LoadOp::ClearDepth( 1.0f ) );
         },
         Ok );
    graph.AddPass(
         "Particles", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( depth, Access::SampledGraphics );
             pass.ColorTarget( 0, back, LoadOp::Load() );
             pass.DepthTarget( depth, LoadOp::Load(), false );
         },
         Ok );
    const CompileResult result = CompileOrFail( graph );
    EXPECT_TRUE( result.Faults.empty() ) << ( result.Faults.empty() ? "" : result.Faults[0].Reason );
    const CompiledPass* particles = result.FindPass( "Particles" );
    ASSERT_NE( particles, nullptr );

    // The attachment is the read-only one (depth test, no write).
    const auto attachment = std::find_if( particles->Attachments.begin(), particles->Attachments.end(),
                                          []( const AttachmentDecision& decision ) { return decision.IsDepth; } );
    ASSERT_NE( attachment, particles->Attachments.end() );
    EXPECT_EQ( attachment->Usage, Access::DepthRead );

    // One barrier into the read-only layout, covering the depth test AND the fragment sample.
    const std::vector<Barrier> onDepth = BarriersOn( particles, depth.Index );
    ASSERT_EQ( onDepth.size(), 1u );
    EXPECT_EQ( onDepth[0].Before, GetAccessState( Access::DepthWrite ) );
    EXPECT_EQ( onDepth[0].After.Layout, ImageLayout::DepthStencilReadOnly );
    const AccessState sampled = GetAccessState( Access::SampledGraphics );
    const AccessState tested  = GetAccessState( Access::DepthRead );
    EXPECT_EQ( onDepth[0].After.Stages & sampled.Stages, sampled.Stages );
    EXPECT_EQ( onDepth[0].After.Stages & tested.Stages, tested.Stages );
    EXPECT_EQ( onDepth[0].After.Memory & sampled.Memory, sampled.Memory );
    EXPECT_EQ( onDepth[0].After.Memory & tested.Memory, tested.Memory );

    // Writing the depth while sampling it is a feedback loop: still one subresource in two states.
    Builder          written( "depth write sampled" );
    const TextureRef writtenDepth = written.CreateTexture( Tex2D( 64, 64, ImageFormat::DEPTH32F ), "SceneDepth" );
    const TextureRef writtenBack  = written.RegisterExternal( backbuffer, "Backbuffer" );
    written.AddPass(
         "WritesAndSamples", PassFlags::Raster | PassFlags::NeverCull,
         [&]( PassBuilder& pass )
         {
             pass.ColorTarget( 0, writtenBack, LoadOp::DontCare() );
             pass.DepthTarget( writtenDepth, LoadOp::ClearDepth( 1.0f ) );
             pass.Read( writtenDepth, Access::SampledGraphics );
         },
         Ok );
    const std::string fault = OnlyDeclarationFault( written );
    ASSERT_FALSE( fault.empty() );
    EXPECT_NE( fault.find( "WritesAndSamples" ), std::string::npos ) << fault;
    EXPECT_NE( fault.find( "SceneDepth" ), std::string::npos ) << fault;
}

TEST( RenderGraphCompile, AFailedLayoutWriteBackFailsExecuteNamingTheTexture )
{
    ExternalTexture color = Recorded( ImageFormat::RGBA8F, ImageLayout::ShaderReadOnly, nullptr );
    color.RecordStates    = []( const std::vector<AccessState>&, bool )
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
// attachment layout. The graph must plan TRANSFER_SRC / TRANSFER_DST before the copy (the old wrapper
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
    const std::vector<LoadOp> loads( target.Colors.size(), LoadOp::Load() );
    graph.AddPass(
         "Deferred: DepthResolve", PassFlags::Copy,
         [&]( PassBuilder& pass ) { Nodes::DeclareDepthResolve( pass, sourceRef, target.Depth ); }, Ok );
    graph.AddPass(
         "Deferred: Composite", PassFlags::Raster,
         [&]( PassBuilder& pass ) { Nodes::LoadTarget( pass, target, loads ); }, Ok );
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
        Nodes::AddDepthToScene( graph, samples, sourceRef, targetRef, Ok, ReadSampledGraphics, Ok );
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
        Nodes::AddSceneDepthResolve( graph, samples, sceneRef, resolvedRef, ReadSampledGraphics, Ok );
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

// A SampleZero graph colour (the view's velocity) on a multisampled target: the node drawing the target declares
// NO resolve attachment for its slot (the render pass would average it: VK resolves float colour only by AVERAGE),
// and AddGraphColorResolves adds one "Velocity: Resolve" Raster node that samples the multisampled twin
// (SampledGraphics) and writes the single-sample velocity as its colour slot 0. The scene colour keeps its
// hardware resolve. Mutations: AppendGraphColors pushes color.Color as the resolve for SampleZero (hardware
// resolve kept) / AddGraphColorResolves skips SampleZero -> red.
TEST( RenderGraphCompile, SampleZeroGraphColourGetsAResolveNodeAndNoHardwareResolve )
{
    using Desert::Graphic::AddGraphColorResolves;
    using Desert::Graphic::AppendGraphColors;
    using Desert::Graphic::CreateViewVelocity;
    using Desert::Graphic::DeclareResolves;
    using Desert::Graphic::GraphColor;
    using Desert::Graphic::RasterTargets;
    using Desert::Graphic::VelocityColor;
    using Desert::Graphic::ViewVelocity;
    // CreateViewVelocity gives Velocity a FaultDefault (a lost velocity reads as no motion), and a FaultDefault
    // needs the graph's system sources: register them as every frame does (SceneRenderer, RDGSystemTextures.hpp).
    ExternalTexture black{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
    ExternalTexture white{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
    ExternalTexture blackCube{ Tex2D( 1, 1, ImageFormat::RGBA8F, 1, 6 ), Access::SampledGraphics };
    Builder         graph( "velocity-resolve" );
    RegisterSystemTextures( graph, black, white, blackCube );
    TextureDesc msColour        = Tex2D( 64, 64, ImageFormat::RGBA16F );
    msColour.Samples            = 4;
    const TextureRef   sceneMs  = graph.CreateTexture( msColour, "SceneColor.MSAA" );
    const TextureRef   scene    = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "SceneColor" );
    const ViewVelocity velocity = CreateViewVelocity( graph, Extent3D{ 64, 64, 1 }, 4 );
    ASSERT_TRUE( velocity.Multisample.IsValid() );

    RasterTargets targets;
    targets.Colors            = { sceneMs };
    targets.Resolves          = { scene };
    const GraphColor colors[] = { VelocityColor( velocity, 4 ) };
    ASSERT_TRUE( AppendGraphColors( targets, colors, true ) );
    graph.AddPass(
         "Forward", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             for ( uint32_t slot = 0; slot < targets.Colors.size(); ++slot )
                 pass.ColorTarget( slot, targets.Colors[slot], LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) );
             DeclareResolves( pass, targets.Resolves );
         },
         Ok );
    AddGraphColorResolves(
         graph, colors, []( PassBuilder& pass, const GraphColor& color )
         { pass.Read( color.Multisample, Access::SampledGraphics ); }, []( const GraphColor& ) { return Ok; } );
    ExternalTexture sceneOut( Tex2D( 64, 64, ImageFormat::RGBA16F ), Access::None );
    ExternalTexture velOut( Tex2D( 64, 64, ImageFormat::RG16F ), Access::None );
    graph.Extract( scene, sceneOut, Access::SampledGraphics );
    graph.Extract( velocity.Resolved, velOut, Access::SampledGraphics );

    const CompileResult result  = CompileOrFail( graph );
    const CompiledPass* forward = result.FindPass( "Forward" );
    const CompiledPass* resolve = result.FindPass( "Velocity: Resolve" );
    ASSERT_NE( forward, nullptr );
    ASSERT_NE( resolve, nullptr ) << "a SampleZero colour at MSAA has no resolve node";
    bool sceneResolved = false;
    for ( const AttachmentDecision& attachment : forward->Attachments )
    {
        EXPECT_FALSE( attachment.IsResolve && attachment.Resource == velocity.Resolved.Index )
             << "velocity is resolved by the render pass (an average), not by sample 0";
        sceneResolved = sceneResolved || ( attachment.IsResolve && attachment.Resource == scene.Index );
    }
    EXPECT_TRUE( sceneResolved ) << "the scene colour lost its hardware resolve";
    EXPECT_TRUE( HasFlag( resolve->Flags, PassFlags::Raster ) );
    bool writesVelocity = false;
    for ( const AttachmentDecision& attachment : resolve->Attachments )
        writesVelocity =
             writesVelocity || ( !attachment.IsDepth && !attachment.IsResolve && attachment.Slot == 0 &&
                                 attachment.Resource == velocity.Resolved.Index );
    EXPECT_TRUE( writesVelocity ) << "the resolve node does not write the single-sample velocity";
    const std::vector<Barrier> intoRead = BarriersOn( resolve, velocity.Multisample.Index );
    ASSERT_EQ( intoRead.size(), 1u );
    EXPECT_EQ( intoRead[0].After, GetAccessState( Access::SampledGraphics ) );
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
           "RecordOverdrawResolve" } };
    for ( const auto& [file, function] : bodies )
    {
        const std::string body = bodyOf( read( file ), function );
        ASSERT_FALSE( body.empty() ) << file << ": no " << function;
        EXPECT_EQ( body.find( "BeginRenderPass(" ), std::string::npos ) << function;
        EXPECT_EQ( body.find( "EndRenderPass(" ), std::string::npos ) << function;
        EXPECT_EQ( body.find( "TransitionLayout(" ), std::string::npos ) << function;
    }

    // The glass shader (StaticMeshGlass) has no cascade slot: the glass pass neither declares nor binds the
    // cascades. Binding u_ShadowMap0..3 by name there is refused ("not a resource of shader 'StaticMeshGlass'")
    // and takes the whole frame graph down with it - the RDG-A2-W4 regression the live editor showed.
    std::string glass =
         bodyOf( read( "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererForward.cpp" ),
                 "RenderGlassManual" );
    std::erase_if( glass, []( const char c ) { return std::isspace( static_cast<unsigned char>( c ) ); } );
    EXPECT_EQ( glass.find( "kShadowMapNames" ), std::string::npos );

    // The scene/view inputs every lit pass binds (SceneViewInputsOf): a cascade that exists this frame is its
    // graph texture, the rest read the white system texture (no shadow). Always-white would light every pixel as
    // unshadowed.
    std::string view = read( "Desert/Desert/Source/Engine/Graphic/FrameGraphRefs.hpp" );
    std::erase_if( view, []( const char c ) { return std::isspace( static_cast<unsigned char>( c ) ); } );
    EXPECT_NE(
         view.find( "inputs.ShadowMaps[c]=t.ShadowCascades[c].IsValid()?t.ShadowCascades[c]:refs.System.White;" ),
         std::string::npos );
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

namespace
{
    std::string SqueezedSource( const fs::path& root, const char* relative );
} // namespace

TEST( RenderGraphCompile, ParticlePoolNodesDeclareTheirBuffersAndDrawIndirect )
{
    // VFX-07/07b. The frame build: every view imports the scene's pool for its draw, only the view that claimed
    // the VFXWorld tick adds the nodes - compact 0, then per step Dispatch Args, Spawn+Update and the next
    // compact, Compute and NOT NeverCull (a node is live because it writes the imported pool; with no emitter it
    // declares nothing).
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const std::string frame =
         SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameAtmosphere.cpp" );
    const size_t begin = frame.find( "voidSceneRenderer::AddFrameParticlesSimulate(" );
    ASSERT_NE( begin, std::string::npos );
    const std::string body = frame.substr( begin, frame.find( "voidSceneRenderer::", begin + 1 ) - begin );
    EXPECT_NE( body.find( "particles->ImportFrameBuffers(graph);if(!particles->ClaimsSimulation())return;"
                          "constuint32_tsteps=particles->SimulationStepCount();"
                          "graph.AddPass(\"Particles:Compact0\",RDG::PassFlags::Compute," ),
               std::string::npos )
         << "a view that did not claim the tick adds simulation nodes, or the draw view does not import the pool";
    EXPECT_NE(
         body.find( "for(uint32_tstep=0;step<steps;++step){graph.AddPass(std::format(\"Particles:DispatchArgs{}\","
                    "step),RDG::PassFlags::Compute," ),
         std::string::npos );
    EXPECT_NE(
         body.find( "graph.AddPass(std::format(\"Particles:Spawn+Update{}\",step),RDG::PassFlags::Compute," ),
         std::string::npos );
    EXPECT_NE( body.find( "graph.AddPass(std::format(\"Particles:Compact{}\",step+1),RDG::PassFlags::Compute," ),
               std::string::npos );
    EXPECT_EQ( body.find( "NeverCull" ), std::string::npos ) << "a particle node outlives its emitters";

    // The declarations: compact writes the pool, both lists and the emitter's Counters, and a later compact reads
    // the step's dispatch arguments IndirectArgs; Dispatch Args reads the step table and writes the Counters and
    // the arguments; Spawn+Update writes the pool and the alive list (the spawned are appended) and reads the
    // arguments IndirectArgs; the draw reads the pool and the alive list and the Counters as IndirectArgs.
    const std::string particles = SqueezedSource(
         root, "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.cpp" );
    EXPECT_NE( particles.find( ".Storage(\"Particles\",m_Pool.ParticlesRef,RDG::Access::StorageWrite)"
                               ".Storage(\"FreeList\",m_Pool.FreeRef,RDG::Access::StorageWrite)"
                               ".Storage(\"AliveList\",m_Pool.AliveRef,RDG::Access::StorageWrite)"
                               ".Storage(\"Counters\",ve.CountersRef,RDG::Access::StorageWrite)"
                               ".PushConstantBytes(static_cast<uint32_t>(sizeof(ParticleCompactPush)));"
                               "if(compact>0)pass.Read(ve.ArgsRef,RDG::Access::IndirectArgs);" ),
               std::string::npos );
    EXPECT_NE( particles.find( ".Storage(\"StepTable\",ve.StepsRef,RDG::Access::StorageRead)"
                               ".Storage(\"Counters\",ve.CountersRef,RDG::Access::StorageWrite)"
                               ".Storage(\"DispatchArgs\",ve.ArgsRef,RDG::Access::StorageWrite)" ),
               std::string::npos )
         << "Dispatch Args does not declare the indirect arguments it writes";
    EXPECT_NE( particles.find( ".Storage(\"Particles\",m_Pool.ParticlesRef,RDG::Access::StorageWrite)"
                               ".Storage(\"StepTable\",ve.StepsRef,RDG::Access::StorageRead)"
                               ".Storage(\"FreeList\",m_Pool.FreeRef,RDG::Access::StorageRead)"
                               ".Storage(\"AliveList\",m_Pool.AliveRef,RDG::Access::StorageWrite)"
                               ".Storage(\"Counters\",ve.CountersRef,RDG::Access::StorageRead)"
                               ".PushConstantBytes(static_cast<uint32_t>(sizeof(ParticleSimPush)));"
                               "pass.Read(ve.ArgsRef,RDG::Access::IndirectArgs);" ),
               std::string::npos );
    // Sized by the GPU counts: Spawn+Update and every compact after the first dispatch indirect; the CPU sizes
    // only Dispatch Args (one thread) and compact 0 (the range's full scan).
    EXPECT_NE( particles.find( "Renderer::DispatchComputeIndirect(bindings,*m_SimPipeline,ve.ArgsRef,"
                               "kParticleSimulateArgsOffset)" ),
               std::string::npos );
    EXPECT_NE( particles.find( "Renderer::DispatchComputeIndirect(bindings,*m_CompactPipeline,ve.ArgsRef,"
                               "kParticleCompactArgsOffset)" ),
               std::string::npos );
    EXPECT_NE( particles.find( "renderer.DispatchCompute(bindings,*m_ArgsPipeline,1,1,1)" ), std::string::npos );
    EXPECT_EQ( particles.find( "DispatchCompute(bindings,*m_SimPipeline" ), std::string::npos )
         << "Spawn+Update is dispatched over a CPU count";
    EXPECT_NE(
         particles.find( "Renderer::DrawProceduralIndirect(bindings,*pipeline,ve.Material->GetMaterialExecutor(),"
                         "ve.CountersRef,slot)" ),
         std::string::npos );
    EXPECT_NE( particles.find( "constuint64_tslot=(ve.Frame->StepCount&1u)*kParticleDrawSlotStride;" ),
               std::string::npos );
    EXPECT_EQ( particles.find( "DrawProcedural(" ), std::string::npos ) << "the billboards draw a fixed count";
    EXPECT_NE( particles.find( "returnm_Simulates&&ve.Declared&&compact<=ve.Frame->StepCount;" ),
               std::string::npos );
    EXPECT_NE( particles.find( "returnm_Simulates&&ve.Declared&&step<ve.Frame->StepCount;" ), std::string::npos );
    // The draw is every view's: it is not gated by the claim.
    EXPECT_NE( particles.find( "returnve.Declared&&ve.Material!=nullptr;" ), std::string::npos );
    EXPECT_NE( particles.find( "m_Simulates=world.PrepareTick(scene);" ), std::string::npos );

    // The pool is the scene's: ParticleWorldGpu, owned by the VFXWorld, claims each tick once; ParticleRenderer
    // owns no pool buffer.
    const std::string world = SqueezedSource(
         root, "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleWorldGpu.cpp" );
    EXPECT_NE( world.find( "if(!m_Claim.Claim(world.GetTickSerial()))returnfalse;" ), std::string::npos );
    EXPECT_NE( world.find( "world.SetGpuState(std::move(made));" ), std::string::npos );
    const std::string header = SqueezedSource(
         root, "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.hpp" );
    EXPECT_EQ( header.find( "ShaderResources::StorageBuffer>" ), std::string::npos )
         << "a view owns particle buffers again";
}

TEST( RenderGraphCompile, TwoViewsOfOneSceneSimulateParticlesOnceAndDrawTwice )
{
    // VFX-07b. Two views of one scene in one tick: the scene's claim (ParticleTickClaim, ParticleWorldGpu) gives
    // the simulation to the first view only; each view's graph is built as AddFrameParticlesSimulate builds it
    // (imports, then the nodes only when claimed) plus ParticlePass. The simulation nodes are added once and the
    // draw twice; in the simulating graph Spawn+Update and compact 1 wait on Dispatch Args' write of the indirect
    // arguments (IndirectArgs after StorageWrite) and the draw on the last compact's Counters. With no emitter the
    // simulation nodes declare nothing and are culled. The next tick is claimed again.
    for ( const bool emitter : { true, false } )
    {
        Desert::Graphic::System::ParticleTickClaim claim;
        constexpr uint64_t                         tick = 7;
        ExternalBuffer                             pool( BufferDesc{ 64 * 64 }, Access::None );
        ExternalBuffer                             freeList( BufferDesc{ 4 * 64 }, Access::None );
        ExternalBuffer                             aliveList( BufferDesc{ 8 * 64 }, Access::None );
        ExternalBuffer                             counters( BufferDesc{ 64 }, Access::None );
        ExternalBuffer                             steps( BufferDesc{ 12 }, Access::None );
        ExternalBuffer                             args( BufferDesc{ 32 }, Access::None );
        ExternalBuffer                             target( BufferDesc{ 16 }, Access::None );
        std::vector<std::string>                   added;
        uint32_t                                   draws = 0;
        for ( const char* viewName : { "view A", "view B" } )
        {
            Builder         graph( viewName );
            const BufferRef poolRef     = graph.RegisterExternal( pool, "ParticlePool" );
            const BufferRef freeRef     = graph.RegisterExternal( freeList, "ParticleFreeList" );
            const BufferRef aliveRef    = graph.RegisterExternal( aliveList, "ParticleAliveList" );
            const BufferRef countersRef = graph.RegisterExternal( counters, "ParticleCounters0" );
            const BufferRef targetRef   = graph.RegisterExternal( target, "SceneColor" );
            const auto      none        = []( PassContext& ) { return Common::MakeSuccess( true ); };
            const bool      simulates   = claim.Claim( tick );
            BufferRef       stepsRef;
            BufferRef       argsRef;
            if ( simulates )
            {
                stepsRef           = graph.RegisterExternal( steps, "ParticleSteps0" );
                argsRef            = graph.RegisterExternal( args, "ParticleDispatchArgs0" );
                const auto compact = [&]( const bool indirect )
                {
                    return [&, indirect]( PassBuilder& pass )
                    {
                        if ( !emitter )
                            return;
                        pass.Write( poolRef, Access::StorageWrite );
                        pass.Write( freeRef, Access::StorageWrite );
                        pass.Write( aliveRef, Access::StorageWrite );
                        pass.Write( countersRef, Access::StorageWrite );
                        if ( indirect )
                            pass.Read( argsRef, Access::IndirectArgs );
                    };
                };
                const std::vector<std::string> nodes = { "Particles: Compact 0", "Particles: Dispatch Args 0",
                                                         "Particles: Spawn+Update 0", "Particles: Compact 1" };
                graph.AddPass( nodes[0], PassFlags::Compute, compact( false ), none );
                graph.AddPass(
                     nodes[1], PassFlags::Compute,
                     [&]( PassBuilder& pass )
                     {
                         if ( !emitter )
                             return;
                         pass.Read( stepsRef, Access::StorageRead );
                         pass.Write( countersRef, Access::StorageWrite );
                         pass.Write( argsRef, Access::StorageWrite );
                     },
                     none );
                graph.AddPass(
                     nodes[2], PassFlags::Compute,
                     [&]( PassBuilder& pass )
                     {
                         if ( !emitter )
                             return;
                         pass.Write( poolRef, Access::StorageWrite );
                         pass.Read( stepsRef, Access::StorageRead );
                         pass.Read( freeRef, Access::StorageRead );
                         pass.Write( aliveRef, Access::StorageWrite );
                         pass.Read( countersRef, Access::StorageRead );
                         pass.Read( argsRef, Access::IndirectArgs );
                     },
                     none );
                graph.AddPass( nodes[3], PassFlags::Compute, compact( true ), none );
                added.insert( added.end(), nodes.begin(), nodes.end() );
            }
            graph.AddPass(
                 "ParticlePass", PassFlags::Compute,
                 [&]( PassBuilder& pass )
                 {
                     pass.Write( targetRef, Access::StorageWrite );
                     if ( !emitter )
                         return;
                     pass.Read( poolRef, Access::StorageRead );
                     pass.Read( aliveRef, Access::StorageRead );
                     pass.Read( countersRef, Access::IndirectArgs );
                 },
                 none );
            ++draws;

            const CompileResult result = CompileOrFail( graph );
            if ( simulates && emitter )
            {
                EXPECT_TRUE( result.CulledPassNames.empty() ) << viewName;
                EXPECT_EQ( BarriersOn( result.FindPass( "Particles: Spawn+Update 0" ), argsRef.Index ).size(), 1u )
                     << "Spawn+Update does not wait on Dispatch Args' indirect arguments";
                EXPECT_EQ( BarriersOn( result.FindPass( "ParticlePass" ), countersRef.Index ).size(), 1u )
                     << "the indirect draw does not wait on the last compact's counters";
            }
            else if ( simulates )
            {
                EXPECT_EQ( result.CulledPassNames,
                           ( std::vector<std::string>{ "Particles: Compact 0", "Particles: Dispatch Args 0",
                                                       "Particles: Spawn+Update 0", "Particles: Compact 1" } ) );
            }
            else
            {
                EXPECT_TRUE( result.CulledPassNames.empty() ) << viewName << ": the draw-only view culls its draw";
            }
        }
        EXPECT_EQ( added.size(), 4u ) << "the scene's particles are simulated once per view, not once per tick";
        EXPECT_EQ( draws, 2u );
        EXPECT_FALSE( claim.Claim( tick ) );
        EXPECT_TRUE( claim.Claim( tick + 1 ) ) << "the next tick is not simulated";
    }
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

// ARCH1b-2: A PASS REGISTERED AT AN EXTENSION POINT LANDS BETWEEN THAT POINT'S NEIGHBOURS. The registry hands each
// point's passes over in registration order and no other point's; a frame build that invokes the point between two
// nodes gets them there in the compiled graph. Red if ForEachAt leaks another point's pass, reorders, or a
// re-registration does not replace (and move to the back) the earlier pass of that name.
TEST( RenderGraphCompile, AnExtensionPassLandsBetweenItsPointsNeighbours )
{
    struct TestExtension
    {
        std::string    Name;
        ExtensionPoint Point = ExtensionPoint::Overlay;
    };
    ExtensionRegistry<TestExtension> registry;
    registry.Register( { "Grid", ExtensionPoint::AfterTranslucency } );
    registry.Register( { "Colliders", ExtensionPoint::Overlay } );
    registry.Register( { "Canvas", ExtensionPoint::UI } );
    registry.Register( { "Cubemap", ExtensionPoint::Overlay } );
    registry.Register( { "Colliders", ExtensionPoint::Overlay } ); // replaced: now after Cubemap
    EXPECT_EQ( registry.All().size(), 4u );
    EXPECT_FALSE( registry.Unregister( "Nothing" ) );

    ExternalTexture  target( Tex2D( 64, 64, ImageFormat::RGBA16F ), Access::None );
    Builder          graph( "extension" );
    const TextureRef scene = graph.RegisterExternal( target, "SceneColor" );
    const auto       add   = [&]( const std::string& name )
    {
        graph.AddPass(
             name, PassFlags::Raster | PassFlags::NeverCull,
             [&]( PassBuilder& pass ) { pass.ColorTarget( 0, scene, LoadOp::Load() ); },
             []( PassContext& ) { return Common::MakeSuccess( true ); } );
    };
    add( "Debug: Lines" );
    registry.ForEachAt( ExtensionPoint::Overlay, [&]( const TestExtension& pass ) { add( pass.Name ); } );
    add( "UI: BackdropBlur" );

    const CompileResult      result = CompileOrFail( graph );
    std::vector<std::string> order;
    for ( const auto& pass : result.Passes )
        order.push_back( pass.Name );
    EXPECT_EQ( order, ( std::vector<std::string>{ "Debug: Lines", "Cubemap", "Colliders", "UI: BackdropBlur" } ) );
}

// ARCH1b-2: no pass is placed through the old external-pass API: the editor, plugins and external graphs register
// at an RDG::ExtensionPoint. Red if RegisterExternalPass / ExternalPassSpecification / ExternalPassContext is
// spelled anywhere in the engine or editor sources again.
TEST( RenderGraphCompile, NoExternalPassApiRemainsInEngineOrEditor )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    size_t files = 0;
    for ( const char* dir : { "Desert/Desert/Source/Engine", "Editor/Source" } )
    {
        for ( const auto& entry : fs::recursive_directory_iterator( root / dir ) )
        {
            const std::string ext = entry.path().extension().string();
            if ( !entry.is_regular_file() || ( ext != ".cpp" && ext != ".hpp" && ext != ".h" ) )
                continue;
            ++files;
            std::ifstream     file( entry.path() );
            const std::string text( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
            for ( const char* symbol :
                  { "RegisterExternalPass", "ExternalPassSpecification", "ExternalPassContext" } )
                EXPECT_EQ( text.find( symbol ), std::string::npos )
                     << entry.path().string() << " spells " << symbol;
        }
    }
    EXPECT_GT( files, 100u ) << "the census read almost nothing";
}

// THE SYSTEM RASTER PASSES ARE REAL GRAPH NODES THAT DECLARE THEIR TARGETS (RDG-LEG1-L5a). AddSystemRasters /
// AddPassNode never open the engine's render pass around a legacy wrapper: every system pass is a Raster node
// whose targets are its framebuffer whole (ColorTarget / DepthTarget / ResolveTarget), whose reads are what the
// system names in SystemRasterPass::Declare, and whose render pass the graph opens and merges. Each system
// declares its own reads where it builds the pass, the editor's extension passes through ExtensionPass::Declare,
// and the in-graph DispatchCompute records no barrier of its own (the particle draw declares its StorageRead).
TEST( RenderGraphCompile, PhasePassesAreRealGraphNodesThatDeclareTheirTargets )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const char* graphic = "Desert/Desert/Source/Engine/Graphic/";
    const auto  source  = [&]( std::string_view relative )
    { return SqueezedSource( root, std::format( "{}{}", graphic, relative ).c_str() ); };

    const std::string sceneRenderer = source( "SceneRenderer.cpp" );
    EXPECT_EQ( sceneRenderer.find( "BeginRenderPass(pass" ), std::string::npos )
         << "the scene renderer still opens a system pass's render pass itself";

    const std::string frameMesh = source( "SceneRendererFrameMesh.cpp" );
    const std::string list =
         SqueezedBody( frameMesh, "voidSceneRenderer::AddSystemRasters(", "voidSceneRenderer::" );
    ASSERT_FALSE( list.empty() ) << "no AddSystemRasters in SceneRendererFrameMesh.cpp";
    const std::string node = SqueezedBody( frameMesh, "voidSceneRenderer::AddPassNode(", "voidSceneRenderer::" );
    ASSERT_FALSE( node.empty() ) << "no AddPassNode in SceneRendererFrameMesh.cpp";
    const std::string bridge = list + node;
    for ( const char* needle :
          { "RDG::PassFlags::Raster", "pass.Declare(declared,textures.GraphRefs())",
            "ResolveDeclared(textures,declared,pass.Name,images)", "DeclareOn(node,images,declared)",
            "node.ColorTarget(slot,targets->Colors[slot],colors[slot])", "targets->Colors[0]=overlay.Color;",
            "targets->Colors[kSceneTargetVelocitySlot]=overlay.Velocity;", "targets->Depth=overlay.Depth;",
            "node.DepthTarget(targets->Depth,depth)", "DeclareResolves(node,targets->Resolves)",
            "RDG::LoadOp::ClearDepth(pass.ClearDepth.value_or(defaults.ClearColor.DepthStencil.x))",
            "AddPassNode(graph,textures,pass,target,pass.Name,color,depth,overlay);" } )
        EXPECT_NE( bridge.find( needle ), std::string::npos ) << "the system raster node does not " << needle;
    EXPECT_EQ( bridge.find( "BeginRenderPass(" ), std::string::npos );
    EXPECT_EQ( bridge.find( "EndRenderPass(" ), std::string::npos );

    // The declaration lives on the pass registration, not in a list in SceneRenderer.
    EXPECT_NE( source( "SystemRasterPass.hpp" )
                    .find( "std::function<void(RenderPassDeclaration&,constFrameGraphRefs&)>Declare;" ),
               std::string::npos );
    EXPECT_NE( source( "ExtensionPass.hpp" )
                    .find( "std::function<void(RenderPassDeclaration&,constExtensionPassContext&)>Declare;" ),
               std::string::npos );

    // Each system names what its pass samples, in its own pass getter.
    const std::pair<const char*, const char*> declared[] = {
         // The procedural sky's LUTs are entries of the SkyboxPass's block (DeclareSkyDraw), each the read.
         { "Systems/Scene/Skybox/SkyboxRenderer.cpp",
           ".Sampled(\"u_SkyViewLut\",refs.Transients.SkyViewLut.IsValid()?refs.Transients.SkyViewLut:white,"
           "RDG::Access::SampledGraphics" },
         { "Systems/Scene/Skybox/SkyboxRenderer.cpp",
           ".Sampled(\"u_TransmittanceLut\",refs.Transients.SkyTransmittanceLut.IsValid()?refs.Transients."
           "SkyTransmittanceLut:white,RDG::Access::SampledGraphics" },
         // The forward mesh node's scene/view inputs are bound per material block of the draw list its Declare
         // builds (MeshDrawList::Declare -> BindSceneViewInputs), so the graph sees each sampled input as a
         // block binding of that node rather than a blanket read.
         { "Systems/Scene/Mesh/MeshRenderer.cpp", "m_ForwardDraws.Declare(declared,SceneViewInputsOf(refs));" },
         { "Systems/Scene/Mesh/MeshRenderer.cpp", "BindSceneViewInputs(block,*view,*layout);" },
         // The terrain node likewise: one block per Forward material of the frame's groups, each binding the
         // scene/view inputs its shader has slots for (TerrainRenderer's DeclareGroupBlocks).
         { "Systems/Scene/Terrain/TerrainRenderer.cpp",
           "(void)DeclareGroupBlocks(declared,m_Pipeline.get(),m_ForwardLayout,GroupExecutors(&ProgramMaterials::"
           "Forward),&view);" },
         { "Systems/Scene/Terrain/TerrainRenderer.cpp", "BindSceneViewInputs(block,*view,*layout);" },
         { "Systems/Scene/Particles/ParticleRenderer.cpp",
           ".Storage(\"Particles\",m_Pool.ParticlesRef,RDG::Access::StorageRead)" },
         // The fog apply's image: the entry of its block (no material route), the read.
         { "Systems/Scene/Fog/HeightFogRenderer.cpp",
           ".Sampled(\"u_FogApply\",refs.Transients.HeightFog,RDG::Access::SampledGraphics" },
         // The cloud composite's pair: entries of its block (the material route + both halves), each the read.
         { "Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
           ".Sampled(\"u_CloudScatter\",scatter,RDG::Access::SampledGraphics" },
         { "Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
           ".Sampled(\"u_CloudGuide\",guide,RDG::Access::SampledGraphics" },
         // The cascades and the cloud shadow map are imported once per frame as scene-view inputs, and every lit
         // node binds them through BindSceneViewInputs (the block entry is the read; RDG-FAULT1).
         { "SceneRendererFrameDeferred.cpp",
           "view.ShadowCascades[c]=textures.Import(mesh->GetCascadeShadowImage(c)," },
         { "SceneRendererFrameAtmosphere.cpp",
           "textures.Transients.CloudShadowMap=textures.Import(clouds->GetShadowMap(),\"Clouds.ShadowMap\");" },
         { "FrameGraphRefs.hpp",
           "inputs.ShadowMaps[c]=t.ShadowCascades[c].IsValid()?t.ShadowCascades[c]:refs.System.White;" },
         { "FrameGraphRefs.hpp", "inputs.CloudShadowMap=CloudShadowMapOrWhite(refs);" } };
    for ( const auto& [file, needle] : declared )
        EXPECT_NE( source( file ).find( needle ), std::string::npos ) << file << " does not declare " << needle;

    // The editor's UI pass declares the backdrop pyramid it samples (no blanket write, no phase-wide sample list).
    EXPECT_NE(
         SqueezedSource( root, "Editor/Source/Editor/RenderSystems/Passes/EditorUIPass.cpp" )
              .find( "declared.Read(ctx.Graph.Transients.BackdropBlur,Graphic::RDG::Access::SampledGraphics" ),
         std::string::npos );

    // The one in-graph compute dispatch is Renderer::DispatchCompute, and it records no barrier: every caller is a
    // graph node, and the graph places the barrier between the declared write and read (RDG-TAILS merged the
    // particle-only DispatchComputeCull into the old route; RDG-A2 P10 deleted that route, DispatchComputeInFrame
    // with VulkanPipelineCompute::RecordInFrame, once its last caller moved to PassBindings).
    const std::string vulkan = source( "API/Vulkan/VulkanRenderer.cpp" );
    const std::string dispatch =
         SqueezedBody( vulkan, "Common::BoolResultStrVulkanRendererAPI::DispatchCompute(", "VulkanRendererAPI::" );
    ASSERT_FALSE( dispatch.empty() );
    EXPECT_EQ( dispatch.find( "vkCmdPipelineBarrier" ), std::string::npos ) << "DispatchCompute barriers";
    for ( const char* file : { "Renderer.hpp", "Renderer.cpp", "RendererAPI.hpp", "API/Vulkan/VulkanRenderer.hpp",
                               "API/Vulkan/VulkanRenderer.cpp", "API/Vulkan/VulkanPipelineCompute.hpp",
                               "API/Vulkan/VulkanPipelineCompute.cpp", "Pipeline.hpp" } )
    {
        const std::string text = source( file );
        EXPECT_EQ( text.find( "DispatchComputeInFrame" ), std::string::npos )
             << file << ": the out-of-PassBindings in-frame dispatch is back";
        EXPECT_EQ( text.find( "RecordInFrame" ), std::string::npos )
             << file << ": the pipeline's in-frame record (setters + its own descriptor ring) is back";
    }
    EXPECT_EQ( vulkan.find( "DispatchComputeCull" ), std::string::npos )
         << "a second dispatch entry point is back";
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
    for ( const char* needle :
          { "AddComputeNodes(graph,textures,clouds->DeclareShadowMapNodes())",
            "AddComputeNodes(graph,textures,sky->DeclareAtmosphereLutNodes())",
            "AddComputeNodes(graph,textures,fog->DeclareFrameNodes(graph,textures.Transients))",
            "AddComputeNodes(graph,textures,clouds->DeclareFrameNodes(graph,frame))" } )
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
           // Each LUT node's block 0 names every LUT as an entry (the renderer's own image, imported by the
           // graph): the entry IS the declaration of the write / the sampled read; no pipeline-level image
           // binding.
           { "Storage(\"u_TransmittanceLut\",m_TransmittanceLut,RDG::Access::StorageWrite",
             "Storage(\"u_MultiScatterLut\",m_MultiScatterLut,RDG::Access::StorageWrite",
             "Storage(\"u_SkyViewLut\",m_SkyViewLut,RDG::Access::StorageWrite",
             "Storage(\"u_AerialPerspectiveLut\",m_AerialPerspectiveLut,RDG::Access::StorageWrite",
             "Storage(\"u_DistantSkyLight\",m_DistantLight,RDG::Access::StorageWrite",
             R"(Sampled("u_TransmittanceLut",m_TransmittanceLut,RDG::Access::SampledCompute,GlobalTextureFilterSampler())",
             // RDG-PSO: each LUT node's layout is the one kept for its pipeline (keyed on the pipeline's shader).
             "declared.Bindings(layout.Get(pipeline->GetSpecification().Shader),",
             "declareBlock(transmittance.Access,m_TransmittanceLutPipeline.get(),m_TransmittanceLutLayout,0)",
             "sampledLuts(declareBlock(distant.Access,m_DistantLightPipeline.get(),m_DistantLightLayout,0))" } },
         { "Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
           "VolumetricCloudRenderer::DeclareShadowMapNodes(",
           { "SampledMedium(SampledVolumes(DeclareComputeBlock(shadow.Access,m_ShadowMapPipeline.get(),m_"
             "ShadowMapLayout,",
             R"(Storage("u_CloudShadowMap",m_ShadowMapImage,RDG::Access::StorageWrite,"Clouds.ShadowMap"))" } },
         { "Systems/Scene/Fog/HeightFogRenderer.cpp",
           "HeightFogRenderer::DeclareFrameNodes(",
           // Block 0's entries: the fog image it writes, the depth and the sky's two images it samples, each
           // with the sampler the image carried as its own.
           { "block.Storage(\"u_FogApply\",fogImage,RDG::Access::StorageWrite)",
             ".Sampled(\"u_SceneDepth\",depth,RDG::Access::SampledCompute,GlobalTextureFilterSampler()",
             ".Sampled(\"u_AerialPerspective\",aerialPerspective,RDG::Access::SampledCompute,VolumeSampler()",
             R"(.Sampled("u_DistantSkyLight",distantSkyLight,RDG::Access::SampledCompute,GlobalTextureFilterSampler())",
             "graph.CreateTexture(fogDesc,\"HeightFog.Fog\")", "transients.HeightFog=fogImage" } },
         { "Systems/Scene/Clouds/VolumetricCloudRenderer.cpp",
           "VolumetricCloudRenderer::DeclareFrameNodes(",
           { "Storage(\"u_CloudSkyOcclusion\",m_SkyOcclusionVolume,RDG::Access::StorageWrite",
             "Sampled(\"u_SceneDepth\",depth,RDG::Access::SampledCompute,GlobalTextureFilterSampler()",
             "SampledMedium(SampledVolumes(DeclareComputeBlock(march.Access,m_MarchPipeline.get(),m_MarchLayout,",
             // The sky's three images are entries of the march with the sampler each carried as its own.
             R"(.Sampled("u_DistantSkyLight",distantSkyLight,RDG::Access::SampledCompute,GlobalTextureFilterSampler())",
             ".Sampled(\"u_CloudAerialPerspective\",aerialPerspective,RDG::Access::SampledCompute,VolumeSampler()",
             R"(.Sampled("u_CloudSunTransmittanceLut",sunTransmittanceLut,RDG::Access::SampledCompute,GlobalTextureFilterSampler())",
             // The trace pair: block entries of the march (written) and of the resolve (sampled).
             ".Storage(\"u_CloudScatter\",trace,RDG::Access::StorageWrite)",
             ".Sampled(\"u_CloudTrace\",trace,RDG::Access::SampledCompute",
             "graph.CreateTexture(traceDesc,\"Clouds.Trace\")",
             "Storage(\"u_ReconstructedScatter\",m_HistoryImage[writeIndex],RDG::Access::StorageWrite" } } };
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

    // The composite reads the cascades and the cloud shadow map ONLY as scene view inputs (block entries with
    // their neutral defaults, below), so the graph brings the map back to a sampled layout after the shadow
    // node's storage write. The second, separately resolved read list (DeclareShadowReads -> shadowReads ->
    // ReadAll) is gone: it declared the same images twice and, when it did not resolve, silently dropped them.
    for ( const char* file : { "SceneRenderer.cpp", "SceneRenderer.hpp", "SceneRendererFrameDeferred.cpp" } )
    {
        EXPECT_EQ( source( file ).find( "DeclareShadowReads" ), std::string::npos ) << file;
        EXPECT_EQ( source( file ).find( "shadowReads" ), std::string::npos ) << file;
    }

    // RDG-TAILS-D2: the map is a graph ref of the frame (imported where its node runs), the composite declares
    // it and binds it by shader name; its material only uploads CloudShadowUB (a slot filled by both routes is
    // refused by DrawFullscreen).
    EXPECT_NE( frame.find( "textures.Transients.CloudShadowMap=textures.Import(clouds->GetShadowMap(),"
                           "\"Clouds.ShadowMap\")" ),
               std::string::npos );
    // MESH-PB1: the map is one of the scene/view inputs (SceneViewInputsOf), which the composite declares and
    // binds.
    EXPECT_NE( source( "FrameGraphRefs.hpp" ).find( "inputs.CloudShadowMap=CloudShadowMapOrWhite(refs);" ),
               std::string::npos );
    // The composite hands the inputs to its block (DeclareCompositeBindings below); the block entry is the read,
    // so the node does not also declare inputs.View.Refs() wholesale.
    EXPECT_NE( source( "SceneRendererFrameDeferred.cpp" ).find( "inputs.View=SceneViewInputsOf(refs);" ),
               std::string::npos );
    EXPECT_EQ( source( "SceneRendererFrameDeferred.cpp" ).find( "inputs.View.Refs()" ), std::string::npos );
    // Declared on the composite's setup block against its layout (RDG-FAULT1 C3a); the exec overload that took
    // an RDG::PassBindings and a Shader is gone (C3b) - a re-added one is red here.
    EXPECT_NE( source( "Systems/Scene/Deferred/DeferredLightingRenderer.hpp" )
                    .find( "BindSceneViewInputs(block,inputs.View,*layout);" ),
               std::string::npos );
    EXPECT_EQ( source( "FrameGraphRefs.hpp" ).find( "voidBindSceneViewInputs(RDG::PassBindings&" ),
               std::string::npos );
    const std::string deferredMaterial = source( "Materials/Deferred/MaterialDeferredLighting.hpp" );
    EXPECT_NE( deferredMaterial.find( "CloudShadowUpload(this,cloudShadow)" ), std::string::npos );
    EXPECT_EQ( deferredMaterial.find( "\"u_CloudShadowMap\"" ), std::string::npos )
         << "the deferred material sets the cloud map itself; it is a pass parameter";
}

// NO LEGACY CONSTRUCT REMAINS (RDG-LEG1-L5b). Every pass of the frame is a Raster, Compute or Copy node that
// declares what it touches; the bridge that let old code record its own render passes behind the graph's back
// (the legacy pass, its two accesses and pass kind, the SHADER_READ_ONLY image wrapper and the frame texture table
// built on it) is gone from the engine and the editor, and must not come back under the same names.
TEST( RenderGraphCompile, NoLegacyConstructRemainsInTheEngine )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const std::regex forbidden(
         R"(AddLegacy|AddLegacyPass|LegacyRead|LegacyWrite|PassFlags::Legacy|WrapLegacyImage|LegacyFrameTextures)"
         // MESH-PB1 M2a: the material-route draws (no PassBindings) are deleted from Renderer / RendererAPI.
         R"(|SubmitVertices\s*\(|RenderMesh\(\s*const GraphicsPipeline\s*\*)"
         // MESH-PB1 M2e: the out-of-graph fullscreen blit (the runtime present is a graph node) and the
         // material-side cloud-map binder (u_CloudShadowMap is a pass parameter).
         R"(|SubmitFullscreenTriangle\s*\(|CloudShadowBind\s*\()"
         // MESH-PB1 M2d: the editor's ImGui draw is a graph node; the out-of-graph swapchain pass is gone.
         R"(|BeginSwapChainRenderPass)" );
    std::vector<std::string> found;
    size_t                   scanned = 0;
    for ( const char* tree : { "Desert/Desert/Source", "Editor/Source" } )
    {
        ASSERT_TRUE( fs::is_directory( root / tree ) ) << tree << " is gone";
        for ( const fs::directory_entry& entry : fs::recursive_directory_iterator( root / tree ) )
        {
            const std::string extension = entry.path().extension().string();
            if ( !entry.is_regular_file() ||
                 ( extension != ".cpp" && extension != ".hpp" && extension != ".h" && extension != ".inl" ) )
                continue;
            ++scanned;
            std::ifstream file( entry.path() );
            std::string   line;
            for ( size_t number = 1; std::getline( file, line ); ++number )
                if ( std::regex_search( line, forbidden ) )
                    found.push_back( std::format( "{}:{}: {}", fs::relative( entry.path(), root ).generic_string(),
                                                  number, line ) );
        }
    }
    EXPECT_GT( scanned, 0u );
    std::string list;
    for ( const std::string& hit : found )
        std::format_to( std::back_inserter( list ), "\n  {}", hit );
    EXPECT_TRUE( found.empty() ) << found.size() << " legacy construct(s) remain:" << list;
}

// EVERY MESH PASS BODY RETURNS ITS DRAWS' RESULT (MESH-PB1 M2a). A draw through RDG::PassBindings is refused (an
// undeclared or unfilled binding) by returning an error; a body that drops it and returns BOOLSUCCESS draws
// nothing and the graph reports success. The cascade body returns each caster draw's and each non-mesh caster's
// (terrain) result, and the terrain's own three bodies return RecordDraws'.
TEST( RenderGraphCompile, MeshPassBodiesReturnTheirDrawResult )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const auto stripped = [&root]( const char* relative )
    {
        std::ifstream      file( root / "Desert/Desert/Source/Engine/Graphic" / relative );
        std::ostringstream text;
        text << file.rdbuf();
        std::string out;
        for ( const char ch : text.str() )
            if ( std::isspace( static_cast<unsigned char>( ch ) ) == 0 )
                out.push_back( ch );
        return out;
    };
    const auto count = []( const std::string& text, const std::string& needle )
    {
        size_t n = 0;
        for ( size_t at = text.find( needle ); at != std::string::npos; at = text.find( needle, at + 1 ) )
            ++n;
        return n;
    };

    const std::string shadow = stripped( "Systems/Scene/Mesh/MeshRendererShadow.cpp" );
    ASSERT_FALSE( shadow.empty() );
    EXPECT_NE( shadow.find( "if(autocast=caster->RecordShadowCascade(context,c,firstBlock,m_CascadeVP[c]);"
                            "!cast.IsSuccess()){returncast;}" ),
               std::string::npos )
         << "the cascade body drops the non-mesh caster's draw result";
    // ...and records through the blocks the caster declared on the cascade node in its setup.
    EXPECT_NE( shadow.find( "constuint32_tdeclaredBlocks=caster->DeclareShadowCascade(declared,c);" ),
               std::string::npos )
         << "the cascade's setup no longer lets the non-mesh casters declare their blocks";
    // The cascade's draw list (singles / generic / skinned / instanced, built in its Declare by
    // BuildShadowCascadeDraws) and the RSM's (DeclareRSMDraws), whose Record returns the first refused draw
    // (MeshDrawList::Record).
    EXPECT_GE( count( shadow, "!drawn.IsSuccess())returndrawn;" ), 1u )
         << "a shadow draw's refusal is no longer returned by its body";
    EXPECT_NE( shadow.find( "returnm_RSMDraws.Record(context);" ), std::string::npos )
         << "the RSM body drops its draw list's result";
    EXPECT_NE( shadow.find( "if(autodrawn=m_CascadeDraws[c].Record(context);!drawn.IsSuccess())returndrawn;" ),
               std::string::npos )
         << "the cascade body drops its draw list's result";
    EXPECT_NE( stripped( "Systems/Scene/Mesh/MeshRenderer.cpp" ).find( "!drawn.IsSuccess())returndrawn;" ),
               std::string::npos )
         << "MeshDrawList::Record no longer returns a refused draw";

    const std::string terrain = stripped( "Systems/Scene/Terrain/TerrainRenderer.cpp" );
    ASSERT_FALSE( terrain.empty() );
    EXPECT_NE( terrain.find( "draw.VertexCount,1);!drawn.IsSuccess())returndrawn;}returnBOOLSUCCESS;" ),
               std::string::npos )
         << "RecordDraws drops a refused terrain draw";
    EXPECT_NE( terrain.find( "returnRecordDraws(context,0,*m_Pipeline,&ProgramMaterials::Forward" ),
               std::string::npos );
    EXPECT_NE( terrain.find( "returnRecordDraws(context,0,*m_GBufferPipeline" ), std::string::npos );
    EXPECT_NE( terrain.find( "returnRecordDraws(context,firstBlock,*m_ShadowPipeline" ), std::string::npos );
}

// RDG-FAULT1 C3b, lead decisions A + B on "Deferred: Composite". The lights reach the shader through the graph's
// upload command (two buffers uploaded BEFORE the node, bound as StorageRead block entries), not through the
// material's storage properties; and the material is FILLED in the node's setup, before the block that the
// setup validation checks against its route fill is declared - never in the exec, where the first frame's
// validation would have read an unfilled material.
TEST( RenderGraphCompile, DeferredCompositeUploadsItsLightsAndFillsItsMaterialInSetup )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const std::string frame =
         SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameDeferred.cpp" );
    const std::string body = SqueezedBody( frame, "voidSceneRenderer::AddFrameComposite(", "voidSceneRenderer::" );
    ASSERT_FALSE( body.empty() );
    const size_t upload  = body.find( "System::DeferredLightingRenderer::UploadLights(graph," );
    const size_t node    = body.find( "\"Deferred:Composite\",RDG::PassFlags::Raster" );
    const size_t fill    = body.find( "deferred->FillMaterial(" );
    const size_t declare = body.find( "deferred->DeclareCompositeBindings(pass,inputs,lights);" );
    const size_t record  = body.find( "deferred->Record(context)" );
    ASSERT_NE( upload, std::string::npos );
    ASSERT_NE( node, std::string::npos );
    ASSERT_NE( fill, std::string::npos );
    ASSERT_NE( declare, std::string::npos );
    ASSERT_NE( record, std::string::npos );
    EXPECT_LT( upload, node ) << "the upload is queued before the node that reads it";
    EXPECT_LT( fill, declare ) << "the material is filled before its route fill is declared";
    EXPECT_LT( declare, record ) << "both in the setup, before the exec";
    EXPECT_EQ( body.find( "FillMaterial(", record ), std::string::npos ) << "no material fill in the exec";

    const std::string renderer = SqueezedSource(
         root, "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/DeferredLightingRenderer.hpp" );
    EXPECT_NE(
         renderer.find( ".Storage(ShaderProtocols::PointLight::Name,lights.Point,RDG::Access::StorageRead)" ),
         std::string::npos );
    EXPECT_NE( renderer.find( ".Storage(ShaderProtocols::SpotLight::Name,lights.Spot,RDG::Access::StorageRead)" ),
               std::string::npos );
    EXPECT_NE( renderer.find( "graph.QueueBufferUpload(buffer,bytes);" ), std::string::npos );
    const std::string material = SqueezedSource(
         root, "Desert/Desert/Source/Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp" );
    EXPECT_EQ( material.find( "Get<StorageBufferProperty>(ShaderProtocols::PointLight::Name)" ),
               std::string::npos )
         << "the material no longer writes the lights: the block entry is their one route";
    EXPECT_EQ( material.find( "Get<StorageBufferProperty>(ShaderProtocols::SpotLight::Name)" ),
               std::string::npos );
}

// RDG-FAULT1 C3b, lead decision B on the post chain: a post node's material is filled in the node's SETUP, before
// the block the setup validates against its route fill - never in a Record* exec, where the first frame's
// validation would have read an unfilled material. Every Record* of every converted renderer is checked, so a
// new Record that fills its material goes red here; a renderer joins the table when its nodes are converted.
TEST( RenderGraphCompile, PostFXMaterialsAreFilledInTheSetupNeverInTheExec )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    // Record: what starts every exec body in the squeezed file - "<Class>::Record" for an out-of-line
    // definition, the return type glued to the name for one defined in its class (BackdropBlurRenderer.hpp).
    struct Renderer
    {
        const char* File;
        const char* Record;
    };
    const std::string dir = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/";
    for ( const Renderer& renderer :
          { Renderer{ "TonemapRenderer.cpp", "TonemapRenderer::Record" },
            Renderer{ "FXAARenderer.cpp", "FXAARenderer::Record" },
            Renderer{ "AutoExposureRenderer.cpp", "AutoExposureRenderer::Record" },
            Renderer{ "BloomRenderer.cpp", "BloomRenderer::Record" },
            Renderer{ "LightShaftRenderer.cpp", "LightShaftRenderer::Record" },
            Renderer{ "LensFlareRenderer.cpp", "LensFlareRenderer::Record" },
            Renderer{ "JumpFloodOutlineRenderer.cpp", "JumpFloodOutlineRenderer::Record" },
            Renderer{ "SMAARenderer.cpp", "SMAARenderer::Record" },
            Renderer{ "BackdropBlurRenderer.hpp", "Common::BoolResultStrRecord" } } )
    {
        const std::string text    = SqueezedSource( root, ( dir + renderer.File ).c_str() );
        const std::string record  = renderer.Record;
        size_t            records = 0;
        for ( size_t at = text.find( record ); at != std::string::npos; at = text.find( record, at + 1 ) )
        {
            const std::string body = FunctionBody( text.substr( at ), record );
            ASSERT_FALSE( body.empty() ) << renderer.File << ": no balanced body after " << record;
            ++records;
            for ( const char* fill : { "BindValues(", "BindInputs(", "SetRawData(", "FillMaterial(",
                                       "FillFinalMaterial(", "SetParams(", "->Set(", "->Set<" } )
                EXPECT_EQ( body.find( fill ), std::string::npos )
                     << body.substr( 0, body.find( '{' ) ) << " fills its material (" << fill << ") in the exec";
            EXPECT_EQ( body.find( "PassBindingsbindings(context);" ), std::string::npos )
                 << body.substr( 0, body.find( '{' ) ) << " binds by name in the exec: its block is the setup's";
        }
        EXPECT_GT( records, 0u ) << renderer.File << " has no " << record;
    }

    const std::string tonemap = SqueezedSource( root, std::format( "{}TonemapRenderer.cpp", dir ).c_str() );
    EXPECT_NE(
         FunctionBody( tonemap, "voidTonemapRenderer::FillMaterial(" ).find( "m_MaterialTonemap->BindValues(" ),
         std::string::npos )
         << "the tonemap's values are bound by FillMaterial";
    const std::string postFx =
         SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/SceneRendererFramePostFX.cpp" );
    const std::string node = SqueezedBody( postFx, "voidSceneRenderer::AddFrameTonemap(", "voidSceneRenderer::" );
    const size_t      fill = node.find( "tonemap->FillMaterial(graphInputs);" );
    const size_t      declare = node.find( "tonemap->DeclareBindings(pass,graphInputs);" );
    const size_t      exec    = node.find( "tonemap->Record(context)" );
    ASSERT_NE( fill, std::string::npos );
    ASSERT_NE( declare, std::string::npos );
    ASSERT_NE( exec, std::string::npos );
    EXPECT_LT( fill, declare ) << "the material is filled before its route fill is declared";
    EXPECT_LT( declare, exec ) << "both in the setup, before the exec";

    // The jump-flood composite's uniforms (outline colour, width, smoothness) the same way.
    const std::string jfa = SqueezedSource( root, std::format( "{}JumpFloodOutlineRenderer.cpp", dir ).c_str() );
    EXPECT_NE( FunctionBody( jfa, "voidJumpFloodOutlineRenderer::FillFinalMaterial(" )
                    .find( "m_MaterialComposite->SetParams(" ),
               std::string::npos )
         << "the composite's values are set by FillFinalMaterial";
    const std::string jfaNode =
         SqueezedBody( postFx, "voidSceneRenderer::AddFrameJumpFlood(", "voidSceneRenderer::" );
    const size_t jfaFill    = jfaNode.find( "jfa->FillFinalMaterial();" );
    const size_t jfaDeclare = jfaNode.find( "jfa->DeclareFinalBindings(pass,seed,scene);" );
    const size_t jfaExec    = jfaNode.find( "jfa->RecordFinal(context)" );
    ASSERT_NE( jfaFill, std::string::npos );
    ASSERT_NE( jfaDeclare, std::string::npos );
    ASSERT_NE( jfaExec, std::string::npos );
    EXPECT_LT( jfaFill, jfaDeclare ) << "the composite is filled before its route fill is declared";
    EXPECT_LT( jfaDeclare, jfaExec ) << "both in the setup, before the exec";
}

// RDG-FAULT1 C3b (cb3f65f9d rule): a node's kept layout is keyed on the shader the pipeline it records with
// holds - `<pipeline>->GetSpecification().Shader` - never on a separate shader handle the renderer keeps beside
// the pipeline, which a reload or a rebuilt pipeline does not update (the cache then hands out the old shader's
// layout while the pipeline records the new one). Every ShaderBindingLayoutCache::Get in the post-processing
// renderers (every file of the directory, so a new one is covered) and in the converted scene systems.
TEST( RenderGraphCompile, BindingLayoutsAreKeyedOnTheRecordingPipelinesShader )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    std::vector<std::string> files;
    const std::string        dir = "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/";
    for ( const auto& entry : fs::directory_iterator( root / dir ) )
        files.push_back( dir + entry.path().filename().string() );
    files.emplace_back( "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.cpp" );
    files.emplace_back( "Editor/Source/Editor/RenderSystems/Passes/EditorGridPass.cpp" );
    files.emplace_back( "Editor/Source/Editor/RenderSystems/Passes/EditorCubemapPreviewPass.cpp" );
    files.emplace_back( "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.cpp" );
    files.emplace_back( "Runtime/Source/RuntimeLayer.cpp" );
    files.emplace_back( "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.cpp" );
    files.emplace_back( "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp" );
    // The terrain and mesh renderers (C3b gap 5): the terrain keeps one layout per program, keyed on the pipeline
    // every group records with; a mesh draw list keeps one per recording shader (ShaderBindingLayoutSet).
    const char* const sceneMeshFiles[] = {
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Terrain/TerrainRenderer.cpp",
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp",
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererForward.cpp",
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererDebug.cpp" };
    for ( const char* file : sceneMeshFiles )
        files.emplace_back( file );
    const std::regex get( R"(([Ll]ayout(?:s|Cache)?\.Get\())" );
    const std::regex key( R"(^[A-Za-z_][\w\[\]\.]*->GetSpecification\(\)\.Shader\))" );
    size_t           gets = 0;
    for ( const std::string& file : files )
    {
        const std::string text = SqueezedSource( root, file.c_str() );
        for ( auto it = std::sregex_iterator( text.begin(), text.end(), get ); it != std::sregex_iterator(); ++it )
        {
            ++gets;
            const std::string after = text.substr( static_cast<size_t>( it->position() + it->length() ), 120 );
            EXPECT_TRUE( std::regex_search( after, key ) )
                 << file << ": a layout keyed on " << after.substr( 0, after.find( ')' ) + 1 )
                 << ", not on the recording pipeline's GetSpecification().Shader";
        }
    }
    EXPECT_GT( gets, 10u ) << "the census found almost no layout lookups: the needle is stale";
    // The Skybox LUT and cloud compute helpers take the kept layout; neither derives one per frame any more.
    std::vector<std::string> keptOnly = {
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Skybox/SkyboxRenderer.cpp",
         "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Clouds/VolumetricCloudRenderer.cpp" };
    keptOnly.insert( keptOnly.end(), std::begin( sceneMeshFiles ), std::end( sceneMeshFiles ) );
    for ( const std::string& file : keptOnly )
        EXPECT_EQ( SqueezedSource( root, file.c_str() ).find( "GetBindingLayout(" ), std::string::npos )
             << file << " derives a binding layout per frame again (Renderer::GetBindingLayout)";
    // Each lookup above is one of these, not a stray: the terrain's three programs, the draw list's per-block Get,
    // the glass and overdraw-resolve blocks.
    const std::string terrain = SqueezedSource( root, sceneMeshFiles[0] );
    EXPECT_NE( terrain.find( "layoutCache.Get(pipeline->GetSpecification().Shader)" ), std::string::npos );
    for ( const char* program :
          { "(declared,m_Pipeline.get(),m_ForwardLayout,", "(pass,m_GBufferPipeline.get(),m_GBufferLayout,",
            "(declared,m_ShadowPipeline.get(),m_ShadowLayout," } )
        EXPECT_NE( terrain.find( std::string( "DeclareGroupBlocks" ) + program ), std::string::npos )
             << "a terrain program's blocks are not keyed on the pipeline it records with: " << program;
    const std::string meshList = SqueezedSource( root, sceneMeshFiles[1] );
    EXPECT_NE( meshList.find( "m_Layouts.Get(declared.Pipeline->GetSpecification().Shader)" ), std::string::npos )
         << "a draw-list block's layout is no longer the kept one of its recording pipeline's shader";
    // A block is per executor AND recording shader: one executor drawn through pipelines of two shaders must not
    // share a block validated against only one of their layouts.
    EXPECT_NE( meshList.find( "block.Material==command.Material&&block.Pipeline->GetSpecification().Shader.get()=="
                              "recordedWith" ),
               std::string::npos );
    EXPECT_NE( meshList.find( "m_Layouts.DropExpired();" ), std::string::npos )
         << "the draw list no longer forgets the layouts of destroyed shaders";
}

// RDG-FAULT1 C3b, the scene and UI systems that record from setup-declared blocks: no exec in these files opens a
// name-taking PassBindings( context ) - every RDG::PassBindings is constructed over a declared block
// (PassBindings( context, context.GetBindingBlock( n ) )). A file joins the table when its nodes are converted.
TEST( RenderGraphCompile, ConvertedSystemsOpenOnlyTheirSetupBlocks )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    for ( const char* file :
          { "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.cpp",
            "Editor/Source/Editor/RenderSystems/Passes/EditorGridPass.cpp",
            "Editor/Source/Editor/RenderSystems/Passes/EditorCubemapPreviewPass.cpp",
            "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.cpp", "Runtime/Source/RuntimeLayer.cpp" } )
    {
        const std::string text   = SqueezedSource( root, file );
        size_t            opened = 0;
        for ( size_t at = text.find( "RDG::PassBindings" ); at != std::string::npos;
              at        = text.find( "RDG::PassBindings", at + 1 ) )
        {
            size_t args = at + std::string_view( "RDG::PassBindings" ).size();
            while ( args < text.size() &&
                    ( std::isalnum( static_cast<unsigned char>( text[args] ) ) != 0 || text[args] == '_' ) )
                ++args;
            if ( args >= text.size() || text[args] != '(' )
                continue; // a type use (a parameter, a reference), not a construction
            ++opened;
            EXPECT_EQ( text.compare( args, std::string_view( "(context,context.GetBindingBlock(" ).size(),
                                     "(context,context.GetBindingBlock(" ),
                       0 )
                 << file << ": " << text.substr( at, 80 ) << " is not opened over a setup-declared block";
        }
        EXPECT_GT( opened, 0u ) << file << " opens no PassBindings: the needle is stale";
    }

    // ParticlePass fills each emitter's material in its Declare, before the block that names the material's
    // route fill; the exec only draws.
    const std::string particles = SqueezedSource(
         root, "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Particles/ParticleRenderer.cpp" );
    const size_t pass = particles.find( ".AddPass(\"ParticlePass\"" );
    const size_t declare =
         particles.find( ".Declare=[this](RenderPassDeclaration&declared,constFrameGraphRefs&)", pass );
    ASSERT_NE( pass, std::string::npos );
    ASSERT_NE( declare, std::string::npos );
    const std::string exec        = particles.substr( pass, declare - pass );
    const std::string declaration = particles.substr( declare );
    EXPECT_EQ( exec.find( "->Update(" ), std::string::npos ) << "ParticlePass fills a material in its exec";
    const size_t update   = declaration.find( "fe.Gpu->Material->Update(*view);" );
    const size_t bindings = declaration.find( "fe.Gpu->Material->GetMaterialExecutor()->GetRouteFill()" );
    ASSERT_NE( update, std::string::npos );
    ASSERT_NE( bindings, std::string::npos );
    EXPECT_LT( update, bindings ) << "the material is filled before its route fill is declared";
    EXPECT_NE( declaration.find( ".Storage(\"Particles\",m_Pool.ParticlesRef,RDG::Access::StorageRead)"
                                 ".Storage(\"AliveList\",m_Pool.AliveRef,RDG::Access::StorageRead);"
                                 "declared.Read(fe.CountersRef,RDG::Access::IndirectArgs);" ),
               std::string::npos );
    // Both walk the emitters by the one condition, so the exec's n-th drawn emitter opens block n.
    EXPECT_NE( exec.find( "if(!IsDrawn(fe))continue;" ), std::string::npos );
    EXPECT_NE( declaration.find( "if(!IsDrawn(fe))continue;" ), std::string::npos );

    // The editor's grid and cubemap-ball passes declare their one block in the external pass's Declare (layout
    // kept per pipeline shader, the material's route fill); the exec opens block 0 of it. Without the Declare the
    // exec's GetBindingBlock( 0 ) faults the node every frame.
    for ( const char* file : { "Editor/Source/Editor/RenderSystems/Passes/EditorGridPass.cpp",
                               "Editor/Source/Editor/RenderSystems/Passes/EditorCubemapPreviewPass.cpp" } )
    {
        const std::string text = SqueezedSource( root, file );
        EXPECT_NE( text.find( "pass.Declare=[this](Graphic::RenderPassDeclaration&declared,"
                              "constGraphic::ExtensionPassContext&){declared.Bindings(m_BindingLayout.Get("
                              "m_Pipeline->GetSpecification().Shader),m_Material->GetMaterialExecutor()->"
                              "GetRouteFill());};" ),
                   std::string::npos )
             << file << " declares no setup block";
    }

    // Render2D: the setup (DeclareInto) and the exec (Flush) walk the draw list through the one Resolve, so the
    // n-th drawn command opens the n-th declared block; the executors are filled in the setup only.
    const std::string r2d   = SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.cpp" );
    const std::string setup = SqueezedBody( r2d, "voidRender2D::DeclareInto(", "voidRender2D::" );
    // Flush is FlushList over the frame's own list; the setup resolves each command ONCE (PreparedDraws) and the
    // exec records the prepared draws in order, so the n-th prepared draw opens the n-th declared block.
    const std::string flush = SqueezedBody( r2d, "Common::BoolResultStrRender2D::FlushList(", "voidRender2D::" );
    ASSERT_FALSE( setup.empty() );
    ASSERT_FALSE( flush.empty() );
    EXPECT_NE( setup.find( "ResolvedCommandresolved=Resolve(cmd,backdropValid);" ), std::string::npos );
    EXPECT_NE( setup.find( "for(constauto&draw:m_Prepared.Draws())" ), std::string::npos );
    EXPECT_NE( flush.find( "for(constauto&draw:m_Prepared.Draws())" ), std::string::npos );
    EXPECT_EQ( flush.find( "Resolve(" ), std::string::npos ) << "Flush resolves a command a second time";
    // The UI material's fills are UIMaterialCache::PrepareDraw's (reached from Resolve in the setup), the plain
    // executors' projection push is the setup's; Flush fills nothing.
    const std::string prepareDraw =
         FunctionBody( SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/Render2D/UIMaterialCache.cpp" ),
                       "std::stringUIMaterialCache::PrepareDraw(" );
    for ( const char* fill : { "SetRawData(", "SetPushMatrix(", "SetMaterialIndex(" } )
        EXPECT_NE( prepareDraw.find( fill ), std::string::npos ) << fill;
    EXPECT_NE( setup.find( "PushConstant(&m_Projection" ), std::string::npos );
    for ( const char* fill :
          { "SetRawData(", "SetPushMatrix(", "SetMaterialIndex(", "PushConstant(&m_Projection" } )
        EXPECT_EQ( flush.find( fill ), std::string::npos ) << "Flush fills an executor: " << fill;
    EXPECT_NE( setup.find( ".Sampled(\"u_Backdrop\",backdrop,RDG::Access::SampledGraphics,RDG::SubresourceRange::"
                           "All(),RDG::SamplerDesc::LinearClamp()).PushConstantBytes(static_cast<uint32_t>(sizeof("
                           "GlassPush)));" ),
               std::string::npos );
    EXPECT_NE( flush.find( "uint32_tblock=firstBlock;" ), std::string::npos );

    // The editor's UI pass gathers the frame (the canvas walk) in its Declare and declares the draw list's
    // blocks there; its exec only flushes.
    const std::string ui = SqueezedSource( root, "Editor/Source/Editor/RenderSystems/Passes/EditorUIPass.cpp" );
    const size_t      uiDeclare = ui.find( "pass.Declare=[this](Graphic::RenderPassDeclaration&declared," );
    const size_t      uiExec    = ui.find( "pass.Execute=[this](constGraphic::ExtensionPassContext&ctx," );
    ASSERT_NE( uiDeclare, std::string::npos );
    ASSERT_NE( uiExec, std::string::npos );
    ASSERT_LT( uiDeclare, uiExec );
    const std::string uiSetup = ui.substr( uiDeclare, uiExec - uiDeclare );
    const std::string uiRun   = ui.substr( uiExec );
    EXPECT_NE( uiSetup.find( "UI::RenderCanvas2D(" ), std::string::npos );
    EXPECT_NE( uiSetup.find( "m_Render2D.DeclareBindings(declared,ctx.Graph.Transients.BackdropBlur);" ),
               std::string::npos );
    EXPECT_EQ( uiRun.find( "UI::RenderCanvas2D(" ), std::string::npos ) << "the canvas walk is back in the exec";
    EXPECT_NE( uiRun.find( "m_Render2D.Flush(node,ctx.Graph.Transients.BackdropBlur,0);" ), std::string::npos );
    // The runtime declares the blit as block 0 and the 2D batch after it.
    const std::string runtime = SqueezedSource( root, "Runtime/Source/RuntimeLayer.cpp" );
    EXPECT_NE( runtime.find( "m_Render2D->DeclareBindings(pass,Graphic::RDG::TextureRef{});" ),
               std::string::npos );
    EXPECT_NE( runtime.find( "m_Render2D->Flush(context,Graphic::RDG::TextureRef{},sceneRef.IsValid()?1u:0u);" ),
               std::string::npos );
}

// THE AUTO-EXPOSURE HISTOGRAM IS A TRANSIENT BUFFER OF EACH FRAME GRAPH (RDG-A2 P8). It is cleared, filled and
// resolved within one frame and nothing reads it the next, so the renderer keeps no StorageBuffer for it: the
// graph creates it (Builder::CreateBuffer from AutoExposureRenderer::GetHistogramDesc), Clear and Histogram
// declare Write(StorageWrite), Average declares Read(StorageRead), no node needs NeverCull, and every dispatch
// binds it by the shader's block name through PassBindings (no DispatchComputeInFrame, no SetStorageBuffer).
TEST( RenderGraphCompile, AutoExposureHistogramIsATransientBufferOfTheFrameGraph )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const std::string frame =
         SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/SceneRendererFramePostFX.cpp" );
    const std::string body =
         SqueezedBody( frame, "voidSceneRenderer::AddFrameAutoExposure(", "voidSceneRenderer::" );
    ASSERT_FALSE( body.empty() );
    EXPECT_NE( body.find( "graph.CreateBuffer(System::AutoExposureRenderer::GetHistogramDesc(),"
                          "\"AutoExposure.Histogram\")" ),
               std::string::npos );
    EXPECT_EQ( body.find( "ImportHistogram" ), std::string::npos ) << "the histogram is not an import any more";
    // The accesses are the setup-declared blocks' entries (RDG-FAULT1 C3b): each node's setup declares its block.
    EXPECT_NE( body.find( "autoExp->DeclareClearBindings(pass,histogram);" ), std::string::npos );
    EXPECT_NE( body.find( "autoExp->DeclareHistogramBindings(pass,scene,histogram);" ), std::string::npos );
    EXPECT_NE( body.find( "autoExp->DeclareAverageBindings(pass,histogram,previous,adapted);" ),
               std::string::npos );
    EXPECT_EQ( body.find( "NeverCull" ), std::string::npos );

    const std::string renderer = SqueezedSource(
         root, "Desert/Desert/Source/Engine/Graphic/Systems/Scene/PostProcessing/AutoExposureRenderer.cpp" );
    for ( const char* legacy : { "StorageBuffer::Create(", "ImportBuffer(", "SetStorageBuffer(", "SetInput(",
                                 "SetOutput(", "DispatchComputeInFrame(" } )
        EXPECT_EQ( renderer.find( legacy ), std::string::npos )
             << "AutoExposureRenderer.cpp still calls " << legacy;
    size_t boundWrites = 0;
    for ( size_t at = renderer.find( ".Storage(\"Histogram\",histogram,RDG::Access::StorageWrite)" );
          at != std::string::npos;
          at = renderer.find( ".Storage(\"Histogram\",histogram,RDG::Access::StorageWrite)", at + 1 ) )
        ++boundWrites;
    EXPECT_EQ( boundWrites, 2u ) << "Clear and Histogram bind the histogram by the shader's block name";
    EXPECT_NE( renderer.find( ".Storage(\"Histogram\",histogram,RDG::Access::StorageRead)" ), std::string::npos );
}

// Execute compiles against the backend's pipes, brackets every segment, records ownership releases after
// their pass, and announces demoted AsyncCompute passes once per backend however many graphs demote.
TEST( RenderGraphCompile, ExecuteWalksSegmentsAndReportsDemotionOncePerBackend )
{
    RecordingBackend demoting;
    for ( int frame = 0; frame < 2; ++frame )
    {
        ExternalTexture  out( Tex2D( 64, 64, ImageFormat::RGBA8F ), Access::None );
        Builder          graph( "demoted" );
        const TextureRef o = graph.RegisterExternal( out, "Out" );
        graph.AddPass(
             "Async", PassFlags::Compute | PassFlags::AsyncCompute,
             [&]( PassBuilder& pass ) { pass.Write( o, Access::StorageWrite ); }, Ok );
        Common::BoolResultStr executed = graph.Execute( demoting );
        ASSERT_TRUE( executed.IsSuccess() ) << executed.GetError();
    }
    ASSERT_EQ( demoting.FallbackLines.size(), 1u );
    EXPECT_NE( demoting.FallbackLines[0].find( "Async" ), std::string::npos );
    EXPECT_EQ( demoting.Segments,
               ( std::vector<std::string>{ "Begin Graphics 0..0", "End", "Begin Graphics 0..0", "End" } ) );

    RecordingBackend separate;
    separate.Pipes.SeparateComputeFamily = true;
    ExternalTexture  out( Tex2D( 64, 64, ImageFormat::RGBA8F ), Access::None );
    Builder          graph( "async" );
    const TextureRef o = graph.RegisterExternal( out, "Out" );
    const TextureRef t = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA8F ), "T" );
    graph.AddPass(
         "Produce", PassFlags::Compute, [&]( PassBuilder& pass ) { pass.Write( t, Access::StorageWrite ); }, Ok );
    graph.AddPass(
         "Consume", PassFlags::Compute | PassFlags::AsyncCompute,
         [&]( PassBuilder& pass )
         {
             pass.Read( t, Access::SampledCompute );
             pass.Write( o, Access::StorageWrite );
         },
         Ok );
    Common::BoolResultStr executed = graph.Execute( separate );
    ASSERT_TRUE( executed.IsSuccess() ) << executed.GetError();
    EXPECT_TRUE( separate.FallbackLines.empty() );
    EXPECT_EQ( separate.Segments,
               ( std::vector<std::string>{ "Begin Graphics 0..0", "End", "Begin AsyncCompute 1..1", "End" } ) );
    // Produce releases T and Out to the compute queue; Consume releases Out back to Graphics.
    EXPECT_EQ( std::count( separate.Calls.begin(), separate.Calls.end(), "Epilogue 2" ), 1 );
    EXPECT_EQ( std::count( separate.Calls.begin(), separate.Calls.end(), "Epilogue 1" ), 1 );
}

// MESH-PB1: the lit mesh nodes declare the scene/view inputs they bind (SceneViewInputs: shadow cascades,
// environment cubes, BRDF LUT, cloud shadow map). A body that binds a texture its node did not declare is refused
// by the graph at run time; this keeps the declaration from being dropped while the binding stays.
TEST( RenderGraphCompile, LitMeshNodesDeclareTheSceneViewInputs )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const auto read = [&root]( const char* relative )
    {
        std::ifstream file( root / relative );
        EXPECT_TRUE( file ) << relative << " is gone";
        return std::string( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() );
    };
    const std::string mesh = read( "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.cpp" );
    // MeshGeometryPass builds its draw list in setup and declares one block per material of it, the scene/view
    // inputs bound where the material's shader has slots for them (MeshDrawList; RDG-FAULT1 C3a).
    EXPECT_NE( mesh.find( "m_ForwardDraws.Declare( declared, SceneViewInputsOf( refs ) );" ), std::string::npos )
         << "MeshGeometryPass no longer declares the scene/view inputs";
    EXPECT_NE( mesh.find( "BindSceneViewInputs( block, *view, *layout );" ), std::string::npos )
         << "MeshDrawList no longer binds the scene/view inputs into its blocks";

    const std::string frame    = read( "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameMesh.cpp" );
    size_t            declared = 0;
    for ( size_t at = frame.find( "= view.Refs();" ); at != std::string::npos;
          at        = frame.find( "= view.Refs();", at + 1 ) )
        ++declared;
    EXPECT_EQ( declared, 0u ) << "a mesh node declares SceneViewInputs::Refs() wholesale instead of its blocks";
    EXPECT_NE( frame.find( "meshRenderer->DeclareGenericDraws( pass, view );" ), std::string::npos );
    EXPECT_NE( frame.find( "meshRenderer->DeclareSkinnedDraws( pass, view );" ), std::string::npos );
    EXPECT_NE( frame.find( "meshRenderer->DeclareGBufferDraws( pass );" ), std::string::npos );
    // The glass declares its inputs as its draw list's blocks in setup (MeshDrawList::DeclareBlocks binds them
    // where a cell's shader has slots for them; RDG-PSO: one block per cell), plus the scene copy it refracts.
    const std::string glass =
         read( "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Mesh/MeshRendererForward.cpp" );
    // The call's arguments are matched with every whitespace run read as one space: clang-format may wrap them.
    std::string glassFlat;
    for ( const char c : glass )
    {
        if ( std::isspace( static_cast<unsigned char>( c ) ) == 0 )
            glassFlat += c;
        else if ( !glassFlat.empty() && glassFlat.back() != ' ' )
            glassFlat += ' ';
    }
    EXPECT_NE( glassFlat.find( "m_GlassDraws.Declare( pass, view," ), std::string::npos )
         << "Deferred: Glass no longer declares the scene/view inputs in its binding blocks";
    EXPECT_NE( frame.find( "meshRenderer->DeclareGlassBindings( pass, sceneCopy, view );" ), std::string::npos );

    const std::string refs = read( "Desert/Desert/Source/Engine/Graphic/FrameGraphRefs.hpp" );
    EXPECT_NE( refs.find( "{ EnvIrradiance, EnvSpecular, BrdfLut, CloudShadowMap }" ), std::string::npos )
         << "SceneViewInputs::Refs() must name every input it binds, the cloud map included";

    std::string composite = read( "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameDeferred.cpp" );
    composite.erase( std::remove_if( composite.begin(), composite.end(),
                                     []( unsigned char c ) { return std::isspace( c ) != 0; } ),
                     composite.end() );
    EXPECT_NE( composite.find( "inputs.View=SceneViewInputsOf(refs);" ), std::string::npos )
         << "Deferred: Composite no longer hands the scene/view inputs to its block";
    EXPECT_NE( read( "Desert/Desert/Source/Engine/Graphic/Systems/Scene/Deferred/DeferredLightingRenderer.hpp" )
                    .find( "BindSceneViewInputs( block, inputs.View, *layout );" ),
               std::string::npos )
         << "Deferred: Composite no longer declares the scene/view inputs";
}

// THE PRESENT IS A GRAPH NODE (MESH-PB1 M2c). The runtime's back buffer is imported into a graph each frame
// (Renderer::ImportBackBuffer, UE: RegisterExternalTexture of the viewport's RHI texture), one Raster node clears
// it, blits the scene through PassBindings and draws the 2D batch, and the graph extracts it as Present. No draw
// happens outside a graph pass: the swapchain render pass and the 2D batcher's out-of-graph SubmitIndexed are gone
// from the runtime, and Render2D::Flush takes the node's context.
TEST( RenderGraphCompile, RuntimePresentIsAGraphNode )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const std::string runtime = SqueezedSource( root, "Runtime/Source/RuntimeLayer.cpp" );
    EXPECT_NE( runtime.find( "renderer.ImportBackBuffer(backBuffer)" ), std::string::npos );
    EXPECT_NE( runtime.find( "graph.AddPass(\"RuntimePresent\",Graphic::RDG::PassFlags::Raster" ),
               std::string::npos );
    EXPECT_NE( runtime.find( "pass.ColorTarget(0,target," ), std::string::npos );
    // The blit's block is declared in the node's setup (RDG-FAULT1 C3b) with the sampler it had: LinearClamp.
    EXPECT_NE( runtime.find( "pass.Bindings(m_BlitLayout.Get(m_BlitPipeline->GetSpecification().Shader),"
                             "m_BlitExecutor->GetRouteFill()).Sampled(\"u_Texture\",sceneRef,"
                             "Graphic::RDG::Access::SampledGraphics,Graphic::RDG::SubresourceRange::All(),"
                             "Graphic::RDG::SamplerDesc::LinearClamp());" ),
               std::string::npos );
    // The ref the graph extracts is the imported back buffer itself, not the node's target: under
    // --render-movie the node composes into the movie target and the back buffer is only cleared.
    EXPECT_NE( runtime.find( "constGraphic::RDG::TextureRefbackBufferRef=graph.RegisterExternal(backBuffer,"
                             "\"BackBuffer\");" ),
               std::string::npos );
    EXPECT_NE( runtime.find( "graph.Extract(backBufferRef,backBuffer,Graphic::RDG::Access::Present)" ),
               std::string::npos );
    EXPECT_NE( runtime.find( "renderer.ExecuteGraph(graph)" ), std::string::npos );
    for ( const char* gone : { "BeginSwapChainRenderPass", "SubmitIndexed", "SubmitFullscreenTriangle",
                               "EndRenderPass", "SetImage(" } )
        EXPECT_EQ( runtime.find( gone ), std::string::npos )
             << "RuntimeLayer.cpp draws outside the graph: " << gone;

    const std::string render2D =
         SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.hpp" );
    EXPECT_NE( render2D.find( "Common::BoolResultStrFlush(constRDG::PassContext&context,RDG::TextureRefbackdrop,"
                              "uint32_tfirstBlock);" ),
               std::string::npos )
         << "Render2D::Flush takes the node's context, with no default";
    for ( const char* file : { "Desert/Desert/Source/Engine/Graphic/Renderer.hpp",
                               "Desert/Desert/Source/Engine/Graphic/RendererAPI.hpp",
                               "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.cpp" } )
        EXPECT_EQ( SqueezedSource( root, file ).find( "SubmitIndexed(" ), std::string::npos )
             << file << ": the out-of-graph indexed draw is back";
    EXPECT_EQ(
         SqueezedSource( root, "Editor/Source/Editor/Panels/UI/UIEditorPanel.cpp" ).find( "BeginRenderPass(" ),
         std::string::npos )
         << "the UI editor preview draws outside the graph";
}

// THE EDITOR'S INTERFACE IS A GRAPH NODE (MESH-PB1 M2d). VulkanImGui::End imports the back buffer, records the
// main viewport's draw data in one Raster node on that pass's own command buffer
// (VulkanRdgBackend::CommandBufferOf), extracts the image as Present and executes the graph through the renderer.
// The ImGui backend's pipeline is built against the graph's canonical render pass (CreateRdgRenderPass), not
// against a swapchain render pass, and nothing in the file opens or closes a render pass of its own. The swapchain
// keeps no render pass or framebuffers.
TEST( RenderGraphCompile, EditorInterfaceIsAGraphNode )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const std::string imgui = SqueezedSource( root, "Editor/Source/Editor/ImGuiIntegration/VulkanImGuiLayer.cpp" );
    EXPECT_NE( imgui.find( "renderer.ImportBackBuffer(backBuffer)" ), std::string::npos );
    EXPECT_NE( imgui.find( "graph.AddPass(\"EditorImGui\",Graphic::RDG::PassFlags::Raster" ), std::string::npos );
    EXPECT_NE( imgui.find( "pass.ColorTarget(0,target," ), std::string::npos );
    EXPECT_NE( imgui.find( "VulkanRdgBackend::CommandBufferOf(context)" ), std::string::npos );
    EXPECT_NE( imgui.find( "ImGui_ImplVulkan_RenderDrawData(drawData,commandBuffer.GetValue())" ),
               std::string::npos )
         << "the draw data is recorded on the node's command buffer";
    EXPECT_NE( imgui.find( "graph.Extract(target,backBuffer,Graphic::RDG::Access::Present)" ), std::string::npos );
    EXPECT_NE( imgui.find( "Renderer::ExecuteGraph(graph)" ), std::string::npos );
    EXPECT_NE( imgui.find( "ImGui_ImplVulkan_Init(&init_info,m_ImguiRenderPass)" ), std::string::npos );
    EXPECT_NE( imgui.find( "CreateRdgRenderPass(" ), std::string::npos );
    for ( const char* gone :
          { "GetCurrentCommandBuffer", "BeginRenderPass(", "EndRenderPass(", "GetRenderPass(" } )
        EXPECT_EQ( imgui.find( gone ), std::string::npos )
             << "VulkanImGuiLayer.cpp draws outside the graph: " << gone;

    for ( const char* file : { "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanSwapChain.hpp",
                               "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanSwapChain.cpp" } )
    {
        const std::string swapChain = SqueezedSource( root, file );
        for ( const char* gone : { "m_VkRenderPass", "m_SwapChainFramebuffers", "vkCreateRenderPass(" } )
            EXPECT_EQ( swapChain.find( gone ), std::string::npos )
                 << file << ": the swapchain owns a render pass again: " << gone;
    }
}

// ── Fault isolation (RDG-FAULT1) ────────────────────────────────────────────────────────────────────────
// THE DEFECT THESE PIN (seen live 2026-10-05): one pass binding slots its shader lacks (glass bound
// u_ShadowMap0..3) failed the WHOLE graph - nothing drew - and the same error was logged every frame. The
// relations under test: the faulted pass and only what depends on it alone leave the plan; a shared reader gets
// the producer's declared default; the frame still executes; the report is said once and taken back once.

namespace
{
    // StaticMeshGlass as the graph sees it: it samples the scene colour and declares no shadow map.
    ShaderBindingLayout GlassLayout()
    {
        return { "StaticMeshGlass", { { "u_SceneColor", ShaderResourceKind::SampledTexture } }, 0 };
    }

    // Shadow -> Lighting -> Glass (binds u_ShadowMap0: Validation fault) -> GlassBlur (reads only Glass) ->
    // Composite (reads Lighting AND GlassBlur) -> Backbuffer.
    struct GlassFrame
    {
        ExternalTexture          black{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
        ExternalTexture          white{ Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::SampledGraphics };
        ExternalTexture          blackCube{ Tex2D( 1, 1, ImageFormat::RGBA8F, 1, 6 ), Access::SampledGraphics };
        ExternalTexture          backbuffer{ Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None };
        Builder                  graph{ "FaultFrame" };
        SystemTextures           system;
        TextureRef               shadow, lit, glass, glassBlur, back;
        std::vector<std::string> ran;

        // @p glassBlurDefault: what Composite reads when GlassBlur is lost; @p backPolicy: the backbuffer's.
        GlassFrame( FaultDefault glassBlurDefault, ExternalFaultPolicy backPolicy )
        {
            system    = RegisterSystemTextures( graph, black, white, blackCube );
            shadow    = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Shadow" );
            lit       = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Lit" );
            glass     = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "Glass" );
            glassBlur = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "GlassBlur" );
            back      = graph.RegisterExternal( backbuffer, "Backbuffer" );
            graph.SetFaultDefault( glassBlur, glassBlurDefault );
            graph.SetFaultPolicy( back, backPolicy );

            auto record = [this]( const char* name )
            {
                return [this, name]( PassContext& )
                {
                    ran.emplace_back( name );
                    return Common::MakeSuccess( true );
                };
            };
            graph.AddPass(
                 "Shadow", PassFlags::Raster, [this]( PassBuilder& pass )
                 { pass.ColorTarget( 0, shadow, LoadOp::ClearColor( 1, 1, 1, 1 ) ); }, record( "Shadow" ) );
            graph.AddPass(
                 "Lighting", PassFlags::Raster,
                 [this]( PassBuilder& pass )
                 {
                     pass.Read( shadow, Access::SampledGraphics );
                     pass.ColorTarget( 0, lit, LoadOp::ClearColor( 0, 0, 0, 1 ) );
                 },
                 record( "Lighting" ) );
            graph.AddPass(
                 "Glass", PassFlags::Raster,
                 [this]( PassBuilder& pass )
                 {
                     pass.ColorTarget( 0, glass, LoadOp::ClearColor( 0, 0, 0, 0 ) );
                     pass.Bindings( GlassLayout(), {} )
                          .Sampled( "u_SceneColor", lit, Access::SampledGraphics, SubresourceRange::All(),
                                    SamplerDesc::LinearClamp() )
                          .Sampled( "u_ShadowMap0", shadow, Access::SampledGraphics, SubresourceRange::All(),
                                    SamplerDesc::LinearClamp() );
                 },
                 record( "Glass" ) );
            graph.AddPass(
                 "GlassBlur", PassFlags::Raster,
                 [this]( PassBuilder& pass )
                 {
                     pass.Read( glass, Access::SampledGraphics );
                     pass.ColorTarget( 0, glassBlur, LoadOp::DontCare() );
                 },
                 record( "GlassBlur" ) );
            graph.AddPass(
                 "Composite", PassFlags::Raster,
                 [this]( PassBuilder& pass )
                 {
                     pass.Read( lit, Access::SampledGraphics );
                     pass.Read( glassBlur, Access::SampledGraphics );
                     pass.ColorTarget( 0, back, LoadOp::DontCare() );
                 },
                 record( "Composite" ) );
        }
    };

    std::vector<std::string> ExecutedNames( const CompileResult& result )
    {
        std::vector<std::string> names;
        names.reserve( result.Passes.size() );
        for ( const CompiledPass& pass : result.Passes )
            names.push_back( pass.Name );
        return names;
    }

    Common::BoolResultStr RecordAs( std::vector<std::string>& ran, const char* name )
    {
        ran.emplace_back( name );
        return Common::MakeSuccess( true );
    }
} // namespace

TEST( RenderGraphCompile, BindingValidationNamesTheSlotTheShaderLacks )
{
    DeclaredBindingBlock block;
    block.Layout = std::make_shared<const ShaderBindingLayout>( GlassLayout() );
    block.Entries.push_back( { "u_SceneColor", ShaderResourceKind::SampledTexture, ResourceKind::Texture, 0,
                               Access::SampledGraphics, SubresourceRange::All(), SamplerDesc::LinearClamp() } );
    EXPECT_TRUE( ValidatePassBindings( block ).IsSuccess() );

    block.Entries.push_back( { "u_ShadowMap0", ShaderResourceKind::SampledTexture, ResourceKind::Texture, 1,
                               Access::SampledGraphics, SubresourceRange::All(), SamplerDesc::LinearClamp() } );
    const Common::BoolResultStr refused = ValidatePassBindings( block );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "'u_ShadowMap0' is not a resource of shader 'StaticMeshGlass'" ),
               std::string::npos )
         << refused.GetError();
}

// RDG-FAULT1 C3b. The kept layout follows the shader OBJECT and its compile: another object at the same reload
// generation (a renderer swapped its shader without a reload) re-derives; the same object at the same generation
// hands back the kept pointer (no per-frame derivation, no copy); a reload re-derives; a block declared earlier
// keeps the layout it was validated against.
TEST( RenderGraphCompile, LayoutCacheKeysOnTheShaderObjectAndItsReload )
{
    struct FakeShader
    {
        std::string Name;
    };
    int        derived = 0;
    const auto derive  = [&]( const FakeShader& shader )
    {
        ++derived;
        return ShaderBindingLayout{ shader.Name, {}, 0 };
    };
    LayoutCache                                      cache;
    auto                                             first = std::make_shared<FakeShader>( FakeShader{ "First" } );
    const std::shared_ptr<const ShaderBindingLayout> kept  = cache.Get( first, 0, derive );
    EXPECT_EQ( kept->ShaderName, "First" );
    EXPECT_EQ( cache.Get( first, 0, derive ).get(), kept.get() );
    EXPECT_EQ( derived, 1 );

    auto second = std::make_shared<FakeShader>( FakeShader{ "Second" } );
    EXPECT_EQ( cache.Get( second, 0, derive )->ShaderName, "Second" );
    EXPECT_EQ( derived, 2 );

    second->Name = "SecondReloaded";
    EXPECT_EQ( cache.Get( second, 1, derive )->ShaderName, "SecondReloaded" );
    EXPECT_EQ( derived, 3 );
    EXPECT_EQ( kept->ShaderName, "First" ); // a block holding the old layout still validates against it

    second.reset(); // destroyed; a new object (possibly at the same address) is never the old one
    auto third = std::make_shared<FakeShader>( FakeShader{ "Third" } );
    EXPECT_EQ( cache.Get( third, 1, derive )->ShaderName, "Third" );
    EXPECT_EQ( derived, 4 );
}

// RDG-FAULT1 C3b (mesh draw lists): one kept layout PER SHADER OBJECT. Two shaders in one set each derive once and
// keep their own pointer; a reload of one re-derives only it; a destroyed shader's cache is dropped and a new
// object never inherits its layout.
TEST( RenderGraphCompile, LayoutCacheSetKeepsOneLayoutPerShaderObject )
{
    struct FakeShader
    {
        std::string Name;
    };
    int        derived = 0;
    const auto derive  = [&]( const FakeShader& shader )
    {
        ++derived;
        return ShaderBindingLayout{ shader.Name, {}, 0 };
    };
    LayoutCacheSet                                   set;
    auto                                             lit   = std::make_shared<FakeShader>( FakeShader{ "Lit" } );
    auto                                             glass = std::make_shared<FakeShader>( FakeShader{ "Glass" } );
    const std::shared_ptr<const ShaderBindingLayout> litKept   = set.Get( lit, 0, derive );
    const std::shared_ptr<const ShaderBindingLayout> glassKept = set.Get( glass, 0, derive );
    EXPECT_EQ( litKept->ShaderName, "Lit" );
    EXPECT_EQ( glassKept->ShaderName, "Glass" );
    EXPECT_EQ( derived, 2 );
    // The next frame: both hand back their kept pointer, nothing re-derived (alternating does not evict).
    EXPECT_EQ( set.Get( lit, 0, derive ).get(), litKept.get() );
    EXPECT_EQ( set.Get( glass, 0, derive ).get(), glassKept.get() );
    EXPECT_EQ( derived, 2 );

    glass->Name = "GlassReloaded";
    EXPECT_EQ( set.Get( glass, 1, derive )->ShaderName, "GlassReloaded" );
    EXPECT_EQ( set.Get( lit, 0, derive ).get(), litKept.get() ) << "a reload of one shader re-derived another";
    EXPECT_EQ( derived, 3 );

    lit.reset();
    set.DropExpired();
    EXPECT_EQ( set.Size(), 1u ) << "a destroyed shader's layout is kept forever";
    auto newcomer = std::make_shared<FakeShader>( FakeShader{ "Newcomer" } );
    EXPECT_EQ( set.Get( newcomer, 0, derive )->ShaderName, "Newcomer" );
    EXPECT_EQ( derived, 4 );
}

// RDG-FAULT1 C3b (Tonemap): one block may name ONE ref in several slots - bloom, light shafts and lens flare are
// all System.Black when their nodes did not run. Three sampled reads of one subresource in one layout are one read
// of the pass, so the node compiles and runs; refusing a repeated ref in a block would drop the tonemap every
// frame an effect is off.
TEST( RenderGraphCompile, OneRefInThreeSlotsOfOneBlockIsOneRead )
{
    ExternalTexture     blackImport( Tex2D( 1, 1, ImageFormat::RGBA8F ), Access::None );
    ExternalTexture     backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
    Builder             graph( "tonemap black" );
    const TextureRef    black = graph.RegisterExternal( blackImport, "System.Black" );
    const TextureRef    back  = graph.RegisterExternal( backbuffer, "Backbuffer" );
    ShaderBindingLayout layout{ "SceneComposite",
                                { { "u_BloomTexture", ShaderResourceKind::SampledTexture },
                                  { "u_LightShaftTexture", ShaderResourceKind::SampledTexture },
                                  { "u_LensFlareTexture", ShaderResourceKind::SampledTexture } },
                                0 };
    graph.AddPass(
         "PostFX: Tonemap", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Bindings( layout, {} )
                  .Sampled( "u_BloomTexture", black, Access::SampledGraphics, SubresourceRange::Mip( 0 ),
                            SamplerDesc::LinearClamp() )
                  .Sampled( "u_LightShaftTexture", black, Access::SampledGraphics, SubresourceRange::Mip( 0 ),
                            SamplerDesc::LinearClamp() )
                  .Sampled( "u_LensFlareTexture", black, Access::SampledGraphics, SubresourceRange::Mip( 0 ),
                            SamplerDesc::LinearClamp() );
             pass.ColorTarget( 0, back, LoadOp::DontCare() );
         },
         Ok );
    const CompileResult result = CompileOrFail( graph );
    ASSERT_EQ( result.Passes.size(), 1u );
    EXPECT_TRUE( result.CulledPassNames.empty() );
}

TEST( RenderGraphCompile, BindingValidationRefusesABlockWithoutALayout )
{
    const Common::BoolResultStr refused = ValidatePassBindings( DeclaredBindingBlock{} );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "declares no shader binding layout" ), std::string::npos );
}

TEST( RenderGraphCompile, FaultedPassIsCulledWithItsExclusiveDependants )
{
    const GlassFrame    frame( FaultDefault::Black, ExternalFaultPolicy::FrameFatal );
    const CompileResult result = CompileOrFail( frame.graph );

    // Glass (2) is faulted; GlassBlur (3) read only Glass and has no default for it, so it goes with it.
    EXPECT_EQ( result.FaultCulledPasses, ( std::vector<uint32_t>{ 2, 3 } ) );
    EXPECT_TRUE( result.CulledPasses.empty() ); // fault culling is not reported as "nothing consumes it"
    EXPECT_EQ( ExecutedNames( result ), ( std::vector<std::string>{ "Shadow", "Lighting", "Composite" } ) );
    ASSERT_EQ( result.Faults.size(), 2u );
    EXPECT_EQ( result.Faults[0].PassName, "Glass" );
    EXPECT_EQ( result.Faults[0].Stage, PassFaultStage::Validation );
    EXPECT_NE( result.Faults[0].Reason.find( "u_ShadowMap0" ), std::string::npos );
    EXPECT_EQ( result.Faults[1].PassName, "GlassBlur" );
    EXPECT_EQ( result.Faults[1].Stage, PassFaultStage::Dependency );
    EXPECT_EQ( result.Faults[1].RootPass, std::optional<uint32_t>{ 2 } );
    // A removed pass leaves no trace in the plan: no allocation for what only it and its dependants used.
    EXPECT_EQ( result.FindAllocation( frame.glass.Index ), nullptr );
    EXPECT_EQ( result.FindAllocation( frame.glassBlur.Index ), nullptr );
    EXPECT_FALSE( result.Frame.has_value() );
}

TEST( RenderGraphCompile, SharedDependantReadsTheProducersSystemDefault )
{
    const GlassFrame    frame( FaultDefault::Black, ExternalFaultPolicy::FrameFatal );
    const CompileResult result = CompileOrFail( frame.graph );

    ASSERT_EQ( result.Substitutions.size(), 1u );
    const DefaultSubstitution& substitution = result.Substitutions[0];
    EXPECT_EQ( substitution.ReaderPass, 4u ); // Composite
    EXPECT_EQ( substitution.Original, frame.glassBlur.Index );
    EXPECT_EQ( substitution.Replacement, frame.system.Black.Index );
    EXPECT_EQ( substitution.Default, FaultDefault::Black );
    EXPECT_FALSE( substitution.AttachmentCleared );
}

TEST( RenderGraphCompile, FaultDefaultsOwnTheSystemSourcesAndTheClears )
{
    GlassFrame           frame( FaultDefault::Black, ExternalFaultPolicy::FrameFatal );
    const FaultDefaults& defaults = frame.graph.GetFaultDefaults();
    // RegisterSystemTextures is the one call that gives a graph its sources.
    EXPECT_TRUE( defaults.HasSources() );
    EXPECT_EQ( defaults.GetSource( FaultDefault::Black ), frame.system.Black.Index );
    EXPECT_EQ( defaults.GetSource( FaultDefault::White ), frame.system.White.Index );
    EXPECT_EQ( defaults.GetSource( FaultDefault::BlackCube ), frame.system.BlackCube.Index );
    EXPECT_EQ( defaults.GetSource( FaultDefault::None ), kInvalidResource );
    EXPECT_FALSE( Builder{ "bare" }.GetFaultDefaults().HasSources() );

    const ClearValue black = FaultDefaults::GetClear( FaultDefault::Black );
    const ClearValue white = FaultDefaults::GetClear( FaultDefault::White );
    EXPECT_EQ( black.Color[0], 0.0f );
    EXPECT_EQ( black.Color[3], 1.0f );
    EXPECT_EQ( white.Color[0], 1.0f );
    EXPECT_EQ( white.Color[3], 1.0f );

    // A FaultDefault in a graph without system textures cannot be honoured: a malformed graph.
    Builder          bare( "bare" );
    const TextureRef lost = bare.CreateTexture( Tex2D( 8, 8, ImageFormat::RGBA16F ), "Lost" );
    bare.SetFaultDefault( lost, FaultDefault::White );
    const Common::ResultStr<CompileResult> refused = bare.Compile( kEstimate );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "Lost" ), std::string::npos ) << refused.GetError();
}

TEST( RenderGraphCompile, FrameExecutesWithoutTheFaultedPassAndReportsOnce )
{
    RecordingBackend backend;
    for ( int frameIndex = 0; frameIndex < 3; ++frameIndex )
    {
        GlassFrame frame( FaultDefault::Black, ExternalFaultPolicy::FrameFatal );
        ASSERT_TRUE( frame.graph.Execute( backend ).IsSuccess() );
        EXPECT_EQ( frame.ran, ( std::vector<std::string>{ "Shadow", "Lighting", "Composite" } ) );
        EXPECT_EQ( frame.graph.GetExecuteReport().Faults.size(), 2u );
        EXPECT_TRUE( std::none_of( backend.Calls.begin(), backend.Calls.end(),
                                   []( const std::string& call ) { return call == "BeginPass Glass"; } ) );
    }
    // Three frames with the same defect: ONE line, naming the cascade with its root.
    ASSERT_EQ( backend.FaultLines.size(), 1u );
    EXPECT_NE( backend.FaultLines[0].find( "Glass" ), std::string::npos );
    EXPECT_NE( backend.FaultLines[0].find( "GlassBlur" ), std::string::npos );
    EXPECT_EQ( backend.GetPassFaultReporter().GetActiveCount(), 1u );
}

TEST( RenderGraphCompile, FaultThatLeavesAFrameFatalExternalUnwrittenIsAFrameFault )
{
    // No default for GlassBlur: Composite, the backbuffer's only writer, is culled too.
    RecordingBackend backend;
    GlassFrame       frame( FaultDefault::None, ExternalFaultPolicy::FrameFatal );
    frame.graph.Extract( frame.back, frame.backbuffer, Access::Present );
    const CompileResult result     = CompileOrFail( frame.graph );
    const auto&         frameFault = result.Frame;
    if ( !frameFault.has_value() )
        FAIL() << "the compile reports no frame fault";
    // The report alone tells the caller what to clear and the state to leave it in (the Extract's: Present).
    const std::vector<FrameFaultExternal> expected{ { frame.back.Index, Access::Present } };
    EXPECT_EQ( frameFault->Externals, expected );
    EXPECT_EQ( frameFault->RootPasses, std::vector<uint32_t>{ 2 } );

    EXPECT_FALSE( frame.graph.Execute( backend ).IsSuccess() );
    EXPECT_TRUE( backend.Calls.empty() ); // nothing recorded: the caller clears the backbuffer and presents
    EXPECT_TRUE( frame.ran.empty() );
    EXPECT_EQ( backend.FaultLines.size(), 1u ); // the frame fault is reported through the same reporter
    const ExecuteReport& report        = frame.graph.GetExecuteReport();
    const auto&          reportedFault = report.Frame;
    if ( !reportedFault.has_value() )
        FAIL() << "the execute report carries no frame fault";
    EXPECT_EQ( reportedFault->Externals, expected );
}

TEST( RenderGraphCompile, KeepsContentsExternalWithoutWriterIsNotAFrameFault )
{
    const GlassFrame    frame( FaultDefault::None, ExternalFaultPolicy::KeepsContents );
    const CompileResult result = CompileOrFail( frame.graph );
    EXPECT_FALSE( result.Frame.has_value() );
    EXPECT_EQ( result.FaultCulledPasses, ( std::vector<uint32_t>{ 2, 3, 4 } ) );
    // Shadow and Lighting fed only removed passes: culled the ordinary way, not as faults.
    EXPECT_EQ( result.CulledPasses, ( std::vector<uint32_t>{ 0, 1 } ) );
    EXPECT_TRUE( result.Passes.empty() );
}

TEST( RenderGraphCompile, HistoryExternalWithoutWriterIsListedForItsOwnerToReset )
{
    RecordingBackend backend;
    GlassFrame       frame( FaultDefault::None, ExternalFaultPolicy::InvalidateHistory );
    frame.graph.Extract( frame.back, frame.backbuffer, Access::SampledGraphics );

    EXPECT_TRUE( frame.graph.Execute( backend ).IsSuccess() );
    const ExecuteReport& report = frame.graph.GetExecuteReport();
    EXPECT_FALSE( report.Frame.has_value() );
    // The index is the external's TextureRef::Index: the owner matches it against the ref it registered.
    EXPECT_EQ( report.InvalidatedExternals, std::vector<uint32_t>{ frame.back.Index } );
}

TEST( RenderGraphCompile, LateExecutionFaultKeepsTheFrameAndSkipsOnlyItsDependants )
{
    ExternalTexture          backbuffer( Tex2D( 64, 64, ImageFormat::BGRA8F ), Access::None );
    Builder                  graph( "late" );
    const TextureRef         a    = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "A" );
    const TextureRef         b    = graph.CreateTexture( Tex2D( 64, 64, ImageFormat::RGBA16F ), "B" );
    const TextureRef         back = graph.RegisterExternal( backbuffer, "Backbuffer" );
    std::vector<std::string> ran;
    graph.AddPass(
         "Broken", PassFlags::Raster,
         [&]( PassBuilder& pass ) { pass.ColorTarget( 0, a, LoadOp::ClearColor( 0, 0, 0, 1 ) ); },
         [&]( PassContext& ) -> Common::BoolResultStr
         {
             ran.emplace_back( "Broken" );
             return Common::MakeError( "descriptor pool exhausted" );
         } );
    graph.AddPass(
         "UsesBroken", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( a, Access::SampledGraphics );
             pass.ColorTarget( 0, b, LoadOp::DontCare() );
         },
         [&]( PassContext& ) { return RecordAs( ran, "UsesBroken" ); } );
    graph.AddPass(
         "Present", PassFlags::Raster,
         [&]( PassBuilder& pass ) { pass.ColorTarget( 0, back, LoadOp::DontCare() ); },
         [&]( PassContext& ) { return RecordAs( ran, "Present" ); } );
    graph.AddPass(
         "Blend", PassFlags::Raster,
         [&]( PassBuilder& pass )
         {
             pass.Read( b, Access::SampledGraphics );
             pass.ColorTarget( 0, back, LoadOp::Load() );
         },
         [&]( PassContext& ) { return RecordAs( ran, "Blend" ); } );

    RecordingBackend backend;
    ASSERT_TRUE( graph.Execute( backend ).IsSuccess() );
    // Broken ran and failed; UsesBroken (no default for A) and Blend (its only input B is lost) are skipped;
    // Present is not touched by the fault.
    EXPECT_EQ( ran, ( std::vector<std::string>{ "Broken", "Present" } ) );
    const ExecuteReport& report = graph.GetExecuteReport();
    ASSERT_FALSE( report.Faults.empty() );
    EXPECT_EQ( report.Faults[0].PassName, "Broken" );
    EXPECT_EQ( report.Faults[0].Stage, PassFaultStage::Execution );
    EXPECT_FALSE( report.Frame.has_value() );
    // The render pass Broken opened is closed and the graph ends normally (no AbandonGraph).
    EXPECT_TRUE( std::none_of( backend.Calls.begin(), backend.Calls.end(),
                               []( const std::string& call ) { return call == "AbandonGraph"; } ) );
    ASSERT_FALSE( backend.Calls.empty() );
    EXPECT_EQ( backend.Calls.back().rfind( "EndGraph", 0 ), 0u );
}

TEST( RenderGraphCompile, FaultReporterSaysOncePerPassAndReasonAgainOnChangeAndOnRecovery )
{
    std::vector<std::pair<PassFaultReporter::Severity, std::string>> lines;
    PassFaultReporter reporter( [&]( PassFaultReporter::Severity severity, std::string_view line )
                                { lines.emplace_back( severity, std::string( line ) ); } );
    const std::vector<std::string_view> added = { "Glass" };

    ExecuteReport broken;
    broken.Faults.push_back( { 2, "Glass", PassFaultStage::Validation,
                               "'u_ShadowMap0' is not a resource of shader 'StaticMeshGlass'", std::nullopt } );
    for ( int frameIndex = 0; frameIndex < 5; ++frameIndex )
        reporter.Report( "Scene", added, broken );
    ASSERT_EQ( lines.size(), 1u ); // five frames, one line
    EXPECT_EQ( lines[0].first, PassFaultReporter::Severity::Error );

    ExecuteReport changed    = broken;
    changed.Faults[0].Reason = "'u_ShadowMap1' is not a resource of shader 'StaticMeshGlass'";
    reporter.Report( "Scene", added, changed );
    reporter.Report( "Scene", added, changed );
    ASSERT_EQ( lines.size(), 2u ); // a different reason is news, once

    // The same pass failing in another graph (a preview) is its own key, and that graph not executing this
    // frame does not count as its recovery.
    reporter.Report( "Preview", added, broken );
    ASSERT_EQ( lines.size(), 3u );

    reporter.Report( "Scene", added, ExecuteReport{} );
    ASSERT_EQ( lines.size(), 4u );
    EXPECT_EQ( lines[3].first, PassFaultReporter::Severity::Recovered );
    EXPECT_NE( lines[3].second.find( "Glass" ), std::string::npos );
    reporter.Report( "Scene", added, ExecuteReport{} );
    EXPECT_EQ( lines.size(), 4u );              // recovery is said once
    EXPECT_EQ( reporter.GetActiveCount(), 1u ); // Preview's fault is still active

    reporter.Report( "Scene", added, broken );
    EXPECT_EQ( lines.size(), 5u ); // a relapse is reported again
}

namespace
{
    // What a setup's binding block receives from BindSceneViewInputs (the same Sampled() shape as
    // RenderPassDeclaration::BlockDeclaration and RDG::BindingBlockBuilder), kept as the block
    // ValidatePassBindings checks.
    struct CollectedBlock
    {
        DeclaredBindingBlock Block;

        CollectedBlock& Sampled( std::string_view name, TextureRef texture, Access access, SubresourceRange range,
                                 SamplerDesc sampler )
        {
            Block.Entries.push_back( { std::string( name ), ShaderResourceKind::SampledTexture,
                                       ResourceKind::Texture, texture.Index, access, range, sampler } );
            return *this;
        }
    };

    Desert::Graphic::SceneViewInputs DistinctSceneViewInputs()
    {
        Desert::Graphic::SceneViewInputs view;
        for ( uint32_t c = 0; c < Desert::Graphic::kSceneViewShadowCascades; ++c )
            view.ShadowMaps[c] = TextureRef{ 10 + c };
        view.EnvIrradiance  = TextureRef{ 20 };
        view.EnvSpecular    = TextureRef{ 21 };
        view.BrdfLut        = TextureRef{ 22 };
        view.CloudShadowMap = TextureRef{ 23 };
        return view;
    }

    std::vector<ShaderSlot> SceneViewSlotsWithoutCascades()
    {
        return {
             { std::string( Desert::Graphic::kSceneViewEnvIrradianceName ), ShaderResourceKind::SampledTexture },
             { std::string( Desert::Graphic::kSceneViewEnvSpecularName ), ShaderResourceKind::SampledTexture },
             { std::string( Desert::Graphic::kSceneViewBrdfLutName ), ShaderResourceKind::SampledTexture },
             { std::string( Desert::Graphic::kSceneViewCloudShadowMapName ),
               ShaderResourceKind::SampledTexture } };
    }
} // namespace

// RDG-FAULT1 (the fault that started it): the glass shader (StaticMeshGlass) has no cascade slot. The scene/view
// inputs are declared in the glass node's SETUP against the shader's layout, so u_ShadowMap0..3 never enter its
// block and ValidatePassBindings passes it; a lit layout still gets all four cascades.
TEST( RenderGraphCompile, SceneViewInputsDeclareOnlyTheSlotsTheLayoutHas )
{
    const Desert::Graphic::SceneViewInputs view = DistinctSceneViewInputs();

    ShaderBindingLayout glass = GlassLayout();
    for ( ShaderSlot& slot : SceneViewSlotsWithoutCascades() )
        glass.Slots.push_back( std::move( slot ) );
    EXPECT_TRUE( Desert::Graphic::SamplesSceneViewInputs( glass ) );
    CollectedBlock glassBlock;
    glassBlock.Block.Layout = std::make_shared<const ShaderBindingLayout>( glass );
    glassBlock.Sampled( "u_SceneColor", TextureRef{ 0 }, Access::SampledGraphics, SubresourceRange::All(),
                        SamplerDesc::LinearRepeat() );
    Desert::Graphic::BindSceneViewInputs( glassBlock, view, glass );
    const Common::BoolResultStr glassValid = ValidatePassBindings( glassBlock.Block );
    EXPECT_TRUE( glassValid.IsSuccess() ) << glassValid.GetError();
    EXPECT_EQ( glassBlock.Block.Entries.size(), 5u ); // the scene copy + irradiance, specular, BRDF LUT, cloud map
    for ( const DeclaredBindingEntry& entry : glassBlock.Block.Entries )
        EXPECT_EQ( entry.ShaderName.rfind( "u_ShadowMap", 0 ), std::string::npos ) << entry.ShaderName;

    ShaderBindingLayout lit{ "StaticMeshLit", SceneViewSlotsWithoutCascades(), 0 };
    for ( const std::string_view name : Desert::Graphic::kSceneViewShadowMapNames )
        lit.Slots.push_back( { std::string( name ), ShaderResourceKind::SampledTexture } );
    CollectedBlock litBlock;
    litBlock.Block.Layout = std::make_shared<const ShaderBindingLayout>( lit );
    Desert::Graphic::BindSceneViewInputs( litBlock, view, lit );
    const Common::BoolResultStr litValid = ValidatePassBindings( litBlock.Block );
    EXPECT_TRUE( litValid.IsSuccess() ) << litValid.GetError();
    ASSERT_EQ( litBlock.Block.Entries.size(), 8u );
    for ( uint32_t c = 0; c < Desert::Graphic::kSceneViewShadowCascades; ++c )
    {
        EXPECT_EQ( litBlock.Block.Entries[c].ShaderName, Desert::Graphic::kSceneViewShadowMapNames[c] );
        EXPECT_EQ( litBlock.Block.Entries[c].Index, 10 + c );
    }

    // A G-buffer program samples no scene/view input: nothing is declared for it.
    const ShaderBindingLayout gbuffer{
         "StaticMeshGBuffer", { { "u_AlbedoTexture", ShaderResourceKind::SampledTexture } }, 0 };
    EXPECT_FALSE( Desert::Graphic::SamplesSceneViewInputs( gbuffer ) );
    CollectedBlock gbufferBlock;
    Desert::Graphic::BindSceneViewInputs( gbufferBlock, view, gbuffer );
    EXPECT_TRUE( gbufferBlock.Block.Entries.empty() );
}

// RDG-FAULT1 C3b: the name-taking exec route is DELETED, not deprecated. A PassBindings is opened only from the
// block its pass's setup declared (PassBindings( context, context.GetBindingBlock( i ) )); there is no constructor
// from the context alone and no public Sampled / Storage / Uniform taking a shader name at exec. Once no caller is
// left the compiler cannot catch a re-added overload (nothing calls it), so this census does: red on re-add of the
// declaration or the definition. STRUCTURAL, not by spelling: any PassBindings constructor callable with a
// PassContext alone (whatever the parameter is called, by reference or pointer, or with every later parameter
// defaulted), and any PUBLIC PassBindings member whose first parameter is a name (string_view / std::string /
// const char*). Mutations (each red): `explicit PassBindings( const PassContext& ctx );` in the header;
// `PassBindings( const PassContext& c, BindingBlockRef b = {} );`; a public `PassBindings& Texture(
// std::string_view slot, ... );`; `PassBindings::PassBindings( const PassContext& x )` defined in the .cpp;
// `PassBindings::Uniform(` in the .cpp.
TEST( RenderGraphCompile, TheNameTakingExecBindingApiStaysDeleted )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const std::string header =
         SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/RDG/RDGPassBindings.hpp" );
    const std::string body = SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/RDG/RDGPassBindings.cpp" );
    ASSERT_FALSE( header.empty() );
    ASSERT_FALSE( body.empty() );
    EXPECT_NE( header.find( "PassBindings(constPassContext&context,BindingBlockRefblock);" ), std::string::npos )
         << "the block constructor is the one way to open a PassBindings";
    // A PassContext parameter of any name, then either the end of the list or only defaulted parameters.
    const std::string context = R"(\((const)?(RDG::)?PassContext(&|\*)\w*(=[^,()]*)?(,[^,()]*=[^,()]*)*\))";
    const std::regex  contextAloneDecl( std::format( "PassBindings{}", context ) );
    const std::regex  contextAloneDef( std::format( "PassBindings::PassBindings{}", context ) );
    EXPECT_FALSE( std::regex_search( header, contextAloneDecl ) )
         << "a PassBindings constructor callable with the context alone is back in RDGPassBindings.hpp";
    EXPECT_FALSE( std::regex_search( body, contextAloneDef ) )
         << "a PassBindings constructor taking the context alone is defined in RDGPassBindings.cpp";

    // The public part of class PassBindings: no member takes a shader name first.
    const size_t open = header.find( "classPassBindings{" );
    ASSERT_NE( open, std::string::npos ) << "class PassBindings moved";
    const size_t close = header.find( "};", open );
    ASSERT_NE( close, std::string::npos );
    const std::string classBody  = header.substr( open, close - open );
    const size_t      privateAt  = classBody.find( "private:" );
    const std::string publicPart = classBody.substr( 0, privateAt );
    const std::regex  nameFirst( R"(\w+\((std::)?(string_view|conststd::string&|std::string|constchar\*))" );
    std::smatch       hit;
    EXPECT_FALSE( std::regex_search( publicPart, hit, nameFirst ) )
         << "PassBindings has a public member taking a shader name at exec again: " << hit.str();
    const std::regex nameMemberDef( R"(PassBindings::(Sampled|Storage|Uniform|Texture|Buffer)\()" );
    EXPECT_FALSE( std::regex_search( body, hit, nameMemberDef ) )
         << hit.str() << " is defined in RDGPassBindings.cpp again";
}

// RDG-FAULT1: ShaderBindingLayoutCache.hpp is included by every renderer header that keeps a layout
// (UIMaterialCache.hpp, Render2D.hpp, the post renderers, RuntimeLayer.hpp, the editor passes). It needs only a
// Shader forward declaration; Get lives in its .cpp. Re-inlining Get re-adds Renderer.hpp to all of them, which
// compiles fine - so this is red.
TEST( RenderGraphCompile, ShaderBindingLayoutCacheHeaderStaysLight )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const std::string header =
         SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/ShaderBindingLayoutCache.hpp" );
    const std::string body =
         SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/ShaderBindingLayoutCache.cpp" );
    ASSERT_FALSE( header.empty() );
    ASSERT_FALSE( body.empty() );
    for ( const char* heavy : { "#include<Engine/Graphic/Renderer.hpp>", "#include<Engine/Graphic/Shader.hpp>" } )
        EXPECT_EQ( header.find( heavy ), std::string::npos )
             << heavy << " is back in ShaderBindingLayoutCache.hpp";
    EXPECT_NE( header.find( "classShader;" ), std::string::npos );
    EXPECT_NE( body.find( "ShaderBindingLayoutCache::Get(" ), std::string::npos ) << "Get moved out of the .cpp";
}

// RDG-FAULT1: a UI material that would leave its parameter row unwritten binds the default UI material for its own
// draws (UIMaterialFallback, pinned in the Render2D suite) instead of faulting the whole UI / present node. The
// decision only holds if the one function setup and Flush share asks it, and the cache answers through the
// fallback - a dropped call compiles fine, so this census is what goes red.
TEST( RenderGraphCompile, UIMaterialDrawsFallBackPerDrawNotPerNode )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const std::string render2d =
         SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/Render2D/Render2D.cpp" );
    const std::string cache =
         SqueezedSource( root, "Desert/Desert/Source/Engine/Graphic/Render2D/UIMaterialCache.cpp" );
    ASSERT_FALSE( render2d.empty() );
    ASSERT_FALSE( cache.empty() );
    const std::size_t resolve = render2d.find( "Render2D::ResolvedCommandRender2D::Resolve(" );
    ASSERT_NE( resolve, std::string::npos ) << "Render2D::Resolve (shared by setup and Flush) moved";
    const std::size_t nextFn = render2d.find( "Render2D::DeclareInto(", resolve );
    const std::string body =
         render2d.substr( resolve, nextFn == std::string::npos ? std::string::npos : nextFn - resolve );
    EXPECT_NE( body.find( "m_MaterialCache.DrawableOrDefault(" ), std::string::npos )
         << "Render2D::Resolve no longer routes a material draw through UIMaterialCache::DrawableOrDefault";
    const std::size_t drawable = cache.find( "UIMaterialCache::DrawableOrDefault(" );
    ASSERT_NE( drawable, std::string::npos );
    const std::size_t admit = cache.find( "m_Fallback.Choose(", drawable );
    const std::size_t after = cache.find( "UIMaterialCache::RetireUnused(", drawable );
    EXPECT_TRUE( admit != std::string::npos && admit < after )
         << "DrawableOrDefault no longer asks the UIMaterialFallback";
    // The decision itself (report once per material ASSET, the default in its place) is
    // UIMaterialFallback::Choose, driven with fake entries by drawlist2d
    // UIMaterialFallback.OneBrokenMaterialAmongSeveralThroughThePreparedPath; here: DrawableOrDefault is exactly
    // that decision over the real entries.
    const std::string drawableBody = cache.substr( drawable, after - drawable );
    EXPECT_NE( drawableBody.find( "returnm_Fallback.Choose(*entry,[this](){returnErrorEntry();}," ),
               std::string::npos )
         << "DrawableOrDefault no longer is UIMaterialFallback::Choose with the error fill as the default";
    EXPECT_NE( drawableBody.find( "{returnPrepareDraw(candidate,projection);}" ), std::string::npos )
         << "DrawableOrDefault no longer judges a candidate by PrepareDraw";
    EXPECT_NE( drawableBody.find( "LOG_ERROR(\"{}\",line);}" ), std::string::npos )
         << "the fallback's one report no longer reaches the log";
    EXPECT_NE( cache.find( "built.AssetName=materialService->AssetNameOf(handle);" ), std::string::npos )
         << "an entry no longer keeps the asset name it was resolved from";
    // A SHADER HOT RELOAD REBUILDS THE ENTRY (drawlist2d AShaderReloadRebuildsTheMaterialInsteadOfFallingBack pins
    // RebuildIfReloaded): Build records the generation, every Resolve hit and the error fill follow it, and the
    // follow keys on the entry's PIPELINE's shader - the one PrepareDraw judges the row against.
    EXPECT_NE( cache.find( "entry.ShaderGeneration=shader->GetCodeGeneration();" ), std::string::npos )
         << "Build no longer records the shader generation the entry was built at";
    const std::string follow = FunctionBody( cache, "voidUIMaterialCache::FollowShaderReload(" );
    ASSERT_FALSE( follow.empty() ) << "UIMaterialCache::FollowShaderReload moved";
    EXPECT_NE( follow.find( "entry.Pipeline->GetSpecification().Shader->GetCodeGeneration()" ),
               std::string::npos );
    EXPECT_NE( follow.find( "UIMaterialFallback::RebuildIfReloaded(entry,generation," ), std::string::npos );
    EXPECT_NE( follow.find( "m_RetiredBuilds.push_back(" ), std::string::npos )
         << "a reload destroys the replaced pipeline/material under a frame that may still read them";
    const std::string resolveEntry =
         FunctionBody( cache, "UIMaterialCache::Resolve(constAssets::AssetHandle&handle)" );
    EXPECT_NE( resolveEntry.find( "FollowShaderReload(hit->second," ), std::string::npos )
         << "a cached entry no longer follows its shader's reload";
    EXPECT_NE( FunctionBody( cache, "UIMaterialCache::ErrorEntry()" ).find( "FollowShaderReload(*m_Error," ),
               std::string::npos )
         << "the default UI material no longer follows its shader's reload";
    EXPECT_NE( FunctionBody( cache, "voidUIMaterialCache::RetireUnused()" ).find( "m_RetiredBuilds" ),
               std::string::npos )
         << "replaced builds are never released";
    // The fallback and the setup refusal cannot disagree: PrepareDraw judges by the row's fit AND the very
    // ValidatePassBindings the setup runs, and the row is written nowhere else.
    const std::size_t prepare = cache.find( "std::stringUIMaterialCache::PrepareDraw(" );
    ASSERT_NE( prepare, std::string::npos );
    const std::string prepareBody = cache.substr( prepare, drawable > prepare ? drawable - prepare : 0 );
    EXPECT_NE( prepareBody.find( "UIMaterialFallback::RowFault(" ), std::string::npos );
    EXPECT_NE( prepareBody.find( "RDG::ValidatePassBindings(block)" ), std::string::npos )
         << "PrepareDraw no longer runs the setup's own binding validation";
    EXPECT_EQ( render2d.find( "kMaterialRowBlockName" ), std::string::npos )
         << "Render2D.cpp writes the parameter row again - PrepareDraw is the one place";
    // PREPARED ONCE PER FRAME: the setup prepares the draw list (PreparedDraws, drawlist2d_test), Flush records
    // exactly that list and resolves / prepares / validates nothing itself.
    const std::string declareBody = FunctionBody( render2d, "voidRender2D::DeclareInto(" );
    ASSERT_FALSE( declareBody.empty() ) << "Render2D::DeclareInto moved";
    EXPECT_NE( declareBody.find( "m_Prepared.Prepare(list.GetCommands()," ), std::string::npos )
         << "the setup no longer prepares the frame's draws";
    EXPECT_NE( declareBody.find( "Resolve(cmd,backdropValid)" ), std::string::npos );
    const std::string flushBody = FunctionBody( render2d, "Common::BoolResultStrRender2D::FlushList(" );
    ASSERT_FALSE( flushBody.empty() ) << "Render2D::Flush moved";
    for ( const char* again : { "Resolve(", "DrawableOrDefault(", "PrepareDraw(", "ValidatePassBindings(",
                                "m_Prepared.Prepare(", "m_DrawList.GetCommands())" } )
        EXPECT_EQ( flushBody.find( again ), std::string::npos )
             << "Render2D::Flush calls " << again << " - a draw is prepared a second time in the exec";
    EXPECT_NE( flushBody.find( "for(constauto&draw:m_Prepared.Draws())" ), std::string::npos )
         << "Flush no longer records the list the setup prepared";
    EXPECT_NE( flushBody.find( "if(!m_Prepared.Ready()||m_PreparedList!=&list)" ), std::string::npos )
         << "Flush records a list nobody prepared";
}

// RDG-FAULT1: every producer whose loss a surviving reader can absorb names the value the reader gets instead, at
// the producer, right after the texture is created (before its pass is added): a lost SSAO term is "no occlusion"
// (White - Black would black out the lit scene), a lost bloom / light shaft / SSR / GI / flare term "adds nothing"
// (Black). A producer without one takes every reader down with it.
TEST( RenderGraphCompile, ProducersDeclareTheirFaultDefault )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const auto stripped = [&root]( const char* relative )
    {
        std::ifstream file( root / relative );
        EXPECT_TRUE( file ) << relative << " is gone";
        std::string text( ( std::istreambuf_iterator<char>( file ) ), std::istreambuf_iterator<char>() );
        std::erase_if( text, []( const char c ) { return std::isspace( static_cast<unsigned char>( c ) ); } );
        return text;
    };
    struct Producer
    {
        const char* File;
        const char* Variable;
        const char* GraphName;
        const char* Default;
    };
    constexpr const char* kDeferred   = "Desert/Desert/Source/Engine/Graphic/SceneRendererFrameDeferred.cpp";
    constexpr const char* kPostFX     = "Desert/Desert/Source/Engine/Graphic/SceneRendererFramePostFX.cpp";
    const Producer        producers[] = {
         { kDeferred, "ao", "SSAO", "White" },
         { kDeferred, "gather", "GI.Gather", "Black" },
         { kDeferred, "trace", "SSR.Trace", "Black" },
         { kDeferred, "tiles", "SSR.TileMask", "Black" },
         { kPostFX, "chain", "Bloom", "Black" },
         { kPostFX, "ping", "LightShaft.Ping", "Black" },
         { kPostFX, "pong", "LightShaft.Pong", "Black" },
         { kPostFX, "image", "LensFlare", "Black" },
    };
    for ( const Producer& producer : producers )
    {
        const std::string text   = stripped( producer.File );
        const std::string create = std::format( "TextureRef{}=graph.CreateTexture(", producer.Variable );
        const size_t      at     = text.find( create );
        ASSERT_NE( at, std::string::npos ) << producer.GraphName << ": " << create;
        const size_t named = text.find( std::format( ",\"{}\");", producer.GraphName ), at );
        ASSERT_NE( named, std::string::npos ) << producer.GraphName;
        EXPECT_LT( named - at, 120u ) << producer.GraphName << ": the texture is created under another name";
        const size_t defaulted = text.find( std::format( "graph.SetFaultDefault({},RDG::FaultDefault::{});",
                                                         producer.Variable, producer.Default ),
                                            at );
        ASSERT_NE( defaulted, std::string::npos )
             << producer.GraphName << " declares no FaultDefault::" << producer.Default;
        EXPECT_LT( defaulted, text.find( "AddPass(", at ) )
             << producer.GraphName << ": the FaultDefault is not declared with the texture";
    }
}
