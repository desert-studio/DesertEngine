// EngineHost: Renderer::DispatchCompute through RDG::PassBindings on the ENGINE's own device, booted headless.
//
// The boot is Application's (Engine/Core/Application.cpp) without the window and the swapchain:
// RendererContext::Create -> EngineContext::Initialize -> Device::Create -> SetDevice -> RendererContext::Init
// -> Renderer::Init. A frame with no window records into the graph's own command buffer and
// Renderer::PresentFinalImage submits it and waits for the device (VulkanRendererAPI::SubmitHeadlessFrame).
//
// Mutations that turn this suite red:
//   * VulkanRenderer.cpp DispatchCompute: drop the vkCmdBindDescriptorSets call -> the bytes differ.
//   * VulkanRenderer.cpp DispatchCompute: pass groupCountX - 1 to vkCmdDispatch -> the last 64 words differ.
//   * VulkanRenderer.cpp SubmitHeadlessFrame: delete device->WaitIdle() -> the readback races the GPU.
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

#include <cstring>
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

    constexpr const char* kFillShader = R"DSL(Shader "EngineHostFill"
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

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    ::testing::AddGlobalTestEnvironment( new HostEnvironment );
    return RUN_ALL_TESTS();
}
