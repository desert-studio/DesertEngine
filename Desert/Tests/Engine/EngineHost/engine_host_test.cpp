// EngineHost: Renderer::DispatchCompute through RDG::PassBindings on the ENGINE's own device, booted headless.
//
// The boot is Application's (Engine/Core/Application.cpp) without the window and the swapchain:
// RendererContext::Create -> EngineContext::Initialize -> Device::Create -> SetDevice -> RendererContext::Init
// -> Renderer::Init. A frame with no window is the windowed frame without an output: it records into the frame
// slot's own command buffer (VulkanFrameLoop) and Renderer::PresentFinalImage submits it through the one
// submission path (VulkanFrameLoop::Submit) and waits that slot's fence.
//
// Mutations that turn this suite red:
//   * VulkanRenderer.cpp DispatchCompute: drop the vkCmdBindDescriptorSets call -> the bytes differ.
//   * VulkanRenderer.cpp DispatchCompute: pass groupCountX - 1 to vkCmdDispatch -> the last 64 words differ.
//   * VulkanRenderer.cpp PresentFinalImage: skip the slot fence wait (`m_FrameLoop->WaitSlot( slot )` of the
//     no-output branch) -> the readback races the GPU.
//   * Renderer::DrawFullscreen: draw fewer than 3 vertices -> pixels keep the clear colour;
//     FullscreenTriangle.glslh ScreenUVToNdc without the y flip -> every row lands mirrored.
//   * Renderer::DrawProcedural: draw one instance instead of instanceCount -> the right half keeps the clear
//   colour.
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Shader.hpp>

#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include "../../TestSupport/runner.hpp"

#include <array>
#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace Desert;
using namespace Desert::Graphic;
namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t kWords = 1024; // 16 groups of 64

    // The value the compute shader writes into word i; the test computes the same on the CPU.
    uint32_t Expected( uint32_t i )
    {
        return ( i * 2654435761u ) ^ 0xA5A5A5A5u;
    }

    constexpr const char* kFillShader = R"DSL(// DesertAsset {"Kind":"Shader","Guid":"e46f0a5b1c2d4e3f8a9b0c1d2e3f4a5b","Versions":{"SHDR":1},"Dependencies":[]}
Shader "EngineHostFill"
{
    Compute
    {
        LocalSize(64, 1, 1);

        Buffer(0) Words
        {
            uint u_Words[];
        };

        void main()
        {
            uint i = gl_GlobalInvocationID.x;
            u_Words[i] = (i * 2654435761u) ^ 0xA5A5A5A5u;
        }
    }
}
)DSL";

    // The headless engine, booted once for the process; HostEnvironment shuts the renderer down.
    struct Host
    {
        std::shared_ptr<RendererContext> Context;
        std::shared_ptr<Engine::Device>  Device;
        std::string                      Error;
    };

    Host& GetHost()
    {
        static Host host = []
        {
            Host h;
            // The engine resolves Resources/Shaders (and their #includes) from the Editor directory, the working
            // directory the Editor and the packaged game start in; walk up from wherever the runner started.
            for ( fs::path dir = fs::current_path(); !dir.empty(); dir = dir.parent_path() )
            {
                if ( fs::exists( dir / "Editor" / "Resources" / "Shaders" ) )
                {
                    fs::current_path( dir / "Editor" );
                    break;
                }
                if ( dir == dir.parent_path() )
                    break;
            }
            if ( glfwInit() != GLFW_TRUE ) // the instance asks glfwVulkanSupported(); no window is created
            {
                h.Error = "glfwInit failed";
                return h;
            }
            h.Context = RendererContext::Create( nullptr );
            EngineContext::CreateInstance().Initialize( nullptr, nullptr, h.Context );
            auto device = Engine::Device::Create();
            if ( !device )
            {
                h.Error = device.GetError();
                return h;
            }
            h.Device = device.ExtractValue();
            EngineContext::GetInstance().SetDevice( h.Device );
            h.Context->Init();
            const auto ready = Renderer::CreateInstance().Init();
            if ( !ready )
                h.Error = ready.GetError();
            return h;
        }();
        return host;
    }

    class HostEnvironment final : public ::testing::Environment
    {
    public:
        void TearDown() override
        {
            Host& host = GetHost();
            if ( host.Device )
                host.Device->WaitIdle();
            if ( host.Error.empty() )
                Renderer::GetInstance().Shutdown();
        }
    };

    Common::ResultStr<std::shared_ptr<ComputePipeline>> MakeFillPipeline()
    {
        const fs::path file = fs::temp_directory_path() / "EngineHostFill.shader";
        {
            std::ofstream out( file, std::ios::binary | std::ios::trunc );
            out << kFillShader;
        }
        auto asset = std::make_shared<Assets::ShaderAsset>( Common::Filepath( file.string() ) );
        if ( const auto loaded = asset->LoadFromFile(); !loaded )
            return Common::MakeError<std::shared_ptr<ComputePipeline>>( loaded.GetError() );
        const std::shared_ptr<Shader> shader = Shader::Create( asset );
        if ( !shader )
            return Common::MakeError<std::shared_ptr<ComputePipeline>>( "Shader::Create returned null" );
        return ComputePipeline::Create( { .Shader = shader, .DebugName = "EngineHostFill" } );
    }

    constexpr uint32_t kSide = 64; // the DrawFullscreen target: kSide x kSide RGBA8 UNORM

    // Each pixel writes its own texel coordinate from the interpolated v_TexCoord: (x, y, 255, 255). A triangle
    // that misses a pixel leaves the clear colour; a flipped or shifted uv writes another pixel's coordinate.
    constexpr const char* kCoordShader = R"DSL(// DesertAsset {"Kind":"Shader","Guid":"7c1d2e3f4a5b46c7d8e9f0a1b2c3d4e5","Versions":{"SHDR":1},"Dependencies":[]}
Shader "EngineHostCoord"
{
    Fragment
    {
        In(0) vec2 v_TexCoord;
        Out(0) vec4 o_Color;

        void main()
        {
            vec2 texel = floor( v_TexCoord * 64.0 );
            o_Color = vec4( texel / 255.0, 1.0, 1.0 );
        }
    }

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord = FullscreenTriangleUV();
            gl_Position = vec4( FullscreenTriangleNdc(), 0.0, 1.0 );
        }
    }
}
)DSL";

    // Two instances of a six-vertex quad, each covering one half of the target in x: instance i spans
    // ndc x in [-1 + i, i]. Each pixel writes its coordinate and 10 + gl_InstanceIndex. An instance count
    // dropped to 1 leaves the right half at the clear colour; a lost gl_InstanceIndex writes 10 there.
    constexpr const char* kInstancedShader =
         R"DSL(// DesertAsset {"Kind":"Shader","Guid":"8d2e3f4a5b6c47d8e9f0a1b2c3d4e5f6","Versions":{"SHDR":1},"Dependencies":[]}
Shader "EngineHostInstanced"
{
    Fragment
    {
        In(0) vec2 v_TexCoord;
        In(1) float v_Instance;
        Out(0) vec4 o_Color;

        void main()
        {
            vec2 texel = floor( v_TexCoord * 64.0 );
            o_Color = vec4( texel / 255.0, ( 10.0 + v_Instance ) / 255.0, 1.0 );
        }
    }

    Vertex
    {
        Out(0) vec2 v_TexCoord;
        Out(1) float v_Instance;

        void main()
        {
            const vec2 corners[6] = vec2[]( vec2( 0.0, 0.0 ), vec2( 1.0, 0.0 ), vec2( 0.0, 1.0 ),
                                            vec2( 1.0, 0.0 ), vec2( 1.0, 1.0 ), vec2( 0.0, 1.0 ) );
            vec2 uv = vec2( ( corners[gl_VertexIndex].x + float( gl_InstanceIndex ) ) * 0.5, corners[gl_VertexIndex].y );
            v_TexCoord = uv;
            v_Instance = float( gl_InstanceIndex );
            gl_Position = vec4( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0 );
        }
    }
}
)DSL";

    // @p name is both the shader's name and its temp file's stem.
    Common::ResultStr<std::shared_ptr<GraphicsPipeline>> MakeRasterPipeline( const char* source, const char* name )
    {
        const fs::path file = fs::temp_directory_path() / ( std::string( name ) + ".shader" );
        {
            std::ofstream out( file, std::ios::binary | std::ios::trunc );
            out << source;
        }
        auto asset = std::make_shared<Assets::ShaderAsset>( Common::Filepath( file.string() ) );
        if ( const auto loaded = asset->LoadFromFile(); !loaded )
            return Common::MakeError<std::shared_ptr<GraphicsPipeline>>( loaded.GetError() );
        const std::shared_ptr<Shader> shader = Shader::Create( asset );
        if ( !shader )
            return Common::MakeError<std::shared_ptr<GraphicsPipeline>>( "Shader::Create returned null" );
        GraphicsPipelineSpecification spec;
        spec.DebugName    = name;
        spec.Shader       = shader;
        spec.TargetLayout = RenderTargetLayout{ .ColorFormats = { Core::Formats::ImageFormat::RGBA8F } };
        return GraphicsPipeline::Create( spec );
    }
} // namespace

TEST( EngineHost, TheEngineBootsWithoutAWindow )
{
    const Host& host = GetHost();
    ASSERT_TRUE( host.Error.empty() ) << host.Error;
    EXPECT_NE( EngineContext::GetInstance().GetDevice(), nullptr );
    EXPECT_EQ( EngineContext::GetInstance().GetWindow(), nullptr );
}

// One compute pass whose exec is Renderer::DispatchCompute with the output buffer bound by shader name through
// PassBindings; a copy pass moves it into a host-read extraction. Byte-exact against the CPU, two frames.
TEST( EngineHost, DispatchComputeThroughPassBindingsIsByteExact )
{
    const Host& host = GetHost();
    ASSERT_TRUE( host.Error.empty() ) << host.Error;
    auto pipeline = MakeFillPipeline();
    ASSERT_TRUE( pipeline.IsSuccess() ) << pipeline.GetError();

    for ( int frame = 0; frame < 2; ++frame ) // the second frame reuses the graph's frame objects
    {
        auto&                       renderer = Renderer::GetInstance();
        const Common::BoolResultStr begun    = renderer.BeginFrame();
        ASSERT_TRUE( begun.IsSuccess() ) << "frame " << frame << ": " << begun.GetError();

        RDG::ExternalBuffer  readback;
        RDG::Builder         graph( "engine-host-dispatch" );
        const RDG::BufferRef words = graph.CreateBuffer( RDG::BufferDesc{ kWords * 4u }, "Words" );
        const RDG::BufferRef bytes = graph.CreateBuffer( RDG::BufferDesc{ kWords * 4u }, "Readback" );
        graph.AddPass(
             "Fill", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass ) { pass.Write( words, RDG::Access::StorageWrite ); },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context );
                 bindings.Storage( "Words", words, RDG::Access::StorageWrite );
                 return Renderer::GetInstance().DispatchCompute( bindings, *pipeline.GetValue(), kWords / 64u, 1u,
                                                                 1u );
             } );
        graph.AddPass(
             "Copy", RDG::PassFlags::Copy,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( words, RDG::Access::CopySrc );
                 pass.Write( bytes, RDG::Access::CopyDst );
             },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const auto cmd    = API::Vulkan::VulkanRdgBackend::CommandBufferOf( context );
                 auto       source = context.GetBuffer( words, RDG::Access::CopySrc );
                 auto       target = context.GetBuffer( bytes, RDG::Access::CopyDst );
                 if ( !cmd || !source || !target )
                     return Common::MakeError( "Copy: no command buffer or buffer" );
                 auto from = API::Vulkan::VulkanRdgBackend::BufferOf( source.GetValue() );
                 auto to   = API::Vulkan::VulkanRdgBackend::BufferOf( target.GetValue() );
                 if ( !from || !to )
                     return Common::MakeError( "Copy: no Vulkan buffer" );
                 const VkBufferCopy region{ 0, 0, kWords * 4u };
                 vkCmdCopyBuffer( cmd.GetValue(), from.GetValue()->GetBuffer(), to.GetValue()->GetBuffer(), 1,
                                  &region );
                 return Common::MakeSuccess( true );
             } );
        graph.Extract( bytes, readback, RDG::Access::HostRead );

        const Common::BoolResultStr executed = renderer.ExecuteGraph( graph );
        ASSERT_TRUE( executed.IsSuccess() ) << executed.GetError();
        const Common::BoolResultStr presented = renderer.PresentFinalImage();
        ASSERT_TRUE( presented.IsSuccess() ) << presented.GetError();

        ASSERT_TRUE( readback.Physical && readback.Physical->GetBackendKind() == RDG::BackendKind::Vulkan );
        const auto& buffer = static_cast<const API::Vulkan::VulkanRdgBuffer&>( *readback.Physical );
        buffer.InvalidateForHost();
        ASSERT_NE( buffer.GetMapped(), nullptr );
        std::vector<uint32_t> got( kWords );
        std::memcpy( got.data(), buffer.GetMapped(), kWords * 4u );
        uint32_t mismatches = 0;
        uint32_t first      = kWords;
        for ( uint32_t i = 0; i < kWords; ++i )
            if ( got[i] != Expected( i ) && mismatches++ == 0 )
                first = i;
        EXPECT_EQ( mismatches, 0u ) << "frame " << frame << ": first mismatch at word " << first;
    }
}

namespace
{
    // One frame: a raster pass whose exec is @p draw, into a kSide x kSide RGBA8 graph texture cleared to 0, and a
    // copy pass reading it back. Returns the kSide * kSide * 4 bytes, or the step that failed.
    Common::ResultStr<std::vector<uint8_t>>
    DrawAndReadBack( const char* name, const std::function<Common::BoolResultStr( RDG::PassBindings& )>& draw )
    {
        auto&                       renderer = Renderer::GetInstance();
        const Common::BoolResultStr begun    = renderer.BeginFrame();
        if ( !begun )
            return Common::MakeError<std::vector<uint8_t>>( "BeginFrame: " + begun.GetError() );

        RDG::TextureDesc desc;
        desc.Size   = { kSide, kSide, 1 };
        desc.Format = Core::Formats::ImageFormat::RGBA8F;

        RDG::ExternalBuffer   readback;
        RDG::Builder          graph( name );
        const RDG::TextureRef target = graph.CreateTexture( desc, "Target" );
        const RDG::BufferRef  bytes  = graph.CreateBuffer( RDG::BufferDesc{ kSide * kSide * 4u }, "Readback" );
        graph.AddPass(
             "Draw", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             { pass.ColorTarget( 0, target, RDG::LoadOp::ClearColor( 0.0f, 0.0f, 0.0f, 0.0f ) ); },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 RDG::PassBindings bindings( context );
                 return draw( bindings );
             } );
        graph.AddPass(
             "Copy", RDG::PassFlags::Copy,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( target, RDG::Access::CopySrc );
                 pass.Write( bytes, RDG::Access::CopyDst );
             },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const auto cmd    = API::Vulkan::VulkanRdgBackend::CommandBufferOf( context );
                 auto       source = context.GetTexture( target, RDG::Access::CopySrc );
                 auto       into   = context.GetBuffer( bytes, RDG::Access::CopyDst );
                 if ( !cmd || !source || !into )
                     return Common::MakeError( "Copy: no command buffer, image or buffer" );
                 auto image  = API::Vulkan::VulkanRdgBackend::TextureOf( source.GetValue() );
                 auto buffer = API::Vulkan::VulkanRdgBackend::BufferOf( into.GetValue() );
                 if ( !image || !buffer )
                     return Common::MakeError( "Copy: no Vulkan image or buffer" );
                 VkBufferImageCopy region{};
                 region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                 region.imageExtent      = { kSide, kSide, 1 };
                 vkCmdCopyImageToBuffer( cmd.GetValue(), image.GetValue()->GetImage(),
                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.GetValue()->GetBuffer(), 1,
                                         &region );
                 return Common::MakeSuccess( true );
             } );
        graph.Extract( bytes, readback, RDG::Access::HostRead );

        if ( const Common::BoolResultStr executed = renderer.ExecuteGraph( graph ); !executed )
            return Common::MakeError<std::vector<uint8_t>>( "ExecuteGraph: " + executed.GetError() );
        if ( const Common::BoolResultStr presented = renderer.PresentFinalImage(); !presented )
            return Common::MakeError<std::vector<uint8_t>>( "PresentFinalImage: " + presented.GetError() );

        if ( !readback.Physical || readback.Physical->GetBackendKind() != RDG::BackendKind::Vulkan )
            return Common::MakeError<std::vector<uint8_t>>( "the readback buffer was not extracted" );
        const auto& buffer = static_cast<const API::Vulkan::VulkanRdgBuffer&>( *readback.Physical );
        buffer.InvalidateForHost();
        if ( buffer.GetMapped() == nullptr )
            return Common::MakeError<std::vector<uint8_t>>( "the readback buffer is not mapped" );
        const auto* px = static_cast<const uint8_t*>( buffer.GetMapped() );
        return Common::MakeSuccess( std::vector<uint8_t>( px, px + kSide * kSide * 4u ) );
    }

    // Empty when every pixel equals @p expected( x, y ) (RGBA); else how many differ and the first that does.
    std::string Mismatches( const std::vector<uint8_t>&                                        px,
                            const std::function<std::array<uint8_t, 4>( uint32_t, uint32_t )>& expected )
    {
        uint32_t    count = 0;
        std::string first;
        for ( uint32_t y = 0; y < kSide; ++y )
            for ( uint32_t x = 0; x < kSide; ++x )
            {
                const uint8_t*               p    = px.data() + ( y * kSide + x ) * 4u;
                const std::array<uint8_t, 4> want = expected( x, y );
                if ( std::memcmp( p, want.data(), 4 ) != 0 && count++ == 0 )
                    first = "(" + std::to_string( x ) + ", " + std::to_string( y ) + ") holds " +
                            std::to_string( p[0] ) + "," + std::to_string( p[1] ) + "," + std::to_string( p[2] ) +
                            "," + std::to_string( p[3] );
            }
        return count == 0 ? std::string() : std::to_string( count ) + " wrong pixels, first " + first;
    }
} // namespace

// One raster pass whose exec is Renderer::DrawFullscreen (DrawProcedural(3, 1)) into a graph texture cleared to 0;
// a copy pass reads the image back. Every pixel must hold its own coordinate: the one triangle covers the whole
// target and its uv is the screen texture coordinate with (0,0) at the top-left texel. Two frames.
TEST( EngineHost, DrawFullscreenCoversEveryPixelWithItsOwnTexCoord )
{
    const Host& host = GetHost();
    ASSERT_TRUE( host.Error.empty() ) << host.Error;
    auto pipeline = MakeRasterPipeline( kCoordShader, "EngineHostCoord" );
    ASSERT_TRUE( pipeline.IsSuccess() ) << pipeline.GetError();

    for ( int frame = 0; frame < 2; ++frame )
    {
        const auto px = DrawAndReadBack(
             "engine-host-fullscreen", [&]( RDG::PassBindings& bindings )
             { return Renderer::GetInstance().DrawFullscreen( bindings, *pipeline.GetValue(), nullptr ); } );
        ASSERT_TRUE( px.IsSuccess() ) << "frame " << frame << ": " << px.GetError();
        EXPECT_EQ( Mismatches( px.GetValue(),
                               []( uint32_t x, uint32_t y ) {
                                   return std::array<uint8_t, 4>{ static_cast<uint8_t>( x ),
                                                                  static_cast<uint8_t>( y ), 255u, 255u };
                               } ),
                   "" )
             << "frame " << frame;
    }
}

// DrawProcedural with instanceCount 2: two six-vertex quads, instance i covering x in [i * kSide / 2, (i + 1) *
// kSide / 2). Every pixel holds its own coordinate and 10 + the instance that covers it. Mutations:
// Renderer::DrawProcedural passing 1 instead of instanceCount -> the right half keeps the clear colour;
// firstInstance 1 -> the left half's blue is 11.
TEST( EngineHost, DrawProceduralDrawsEveryInstance )
{
    const Host& host = GetHost();
    ASSERT_TRUE( host.Error.empty() ) << host.Error;
    auto pipeline = MakeRasterPipeline( kInstancedShader, "EngineHostInstanced" );
    ASSERT_TRUE( pipeline.IsSuccess() ) << pipeline.GetError();

    const auto px = DrawAndReadBack(
         "engine-host-instanced", [&]( RDG::PassBindings& bindings )
         { return Renderer::GetInstance().DrawProcedural( bindings, *pipeline.GetValue(), nullptr, 6, 2 ); } );
    ASSERT_TRUE( px.IsSuccess() ) << px.GetError();
    EXPECT_EQ( Mismatches( px.GetValue(),
                           []( uint32_t x, uint32_t y )
                           {
                               return std::array<uint8_t, 4>{ static_cast<uint8_t>( x ), static_cast<uint8_t>( y ),
                                                              static_cast<uint8_t>( x < kSide / 2 ? 10u : 11u ),
                                                              255u };
                           } ),
               "" );
}

namespace
{
    // The host's device is made by the first test that asks for it and shut down once, after the suite.
    const Desert::TestSupport::SuiteEnvironment kHost{
         +[]() -> ::testing::Environment*
         {
             return new HostEnvironment; // NOLINT(cppcoreguidelines-owning-memory)
         } };
} // namespace
