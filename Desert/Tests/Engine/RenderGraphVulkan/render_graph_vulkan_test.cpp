// RenderGraphVulkan - the render graph's Vulkan executor on a real device (RDG2 acceptance). The graph
// clear -> sample -> compute -> copy runs on a windowless device picked by the engine's own DeviceCaps
// judgement, under VK_LAYER_KHRONOS_validation WITH synchronization validation; the readback must match
// byte for byte and the layer must say nothing. A control test weakens one pass's barriers and requires
// synchronization validation to report the hazard - the proof that the silence above means something.

#include <Engine/Graphic/API/Vulkan/DeviceCapsProbe.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>

#include <gtest/gtest.h>
#include <shaderc/shaderc.hpp>
#include <vk-bootstrap/VkBootstrap.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace Desert::Graphic;
using namespace Desert::Graphic::API::Vulkan;
using Desert::Core::Formats::ImageFormat;

namespace
{
    namespace fs = std::filesystem;

    constexpr uint32_t kSize = 16;

    // A message's identity (e.g. "SYNC-HAZARD-WRITE-AFTER-WRITE") is pMessageIdName; pMessage is prose.
    // Validation layers up to 1.3.290 also repeated the id inside pMessage, newer ones (SDK 1.3.296+, the
    // Homebrew layer on macOS) do not - so classifying by pMessage misses every hazard there.
    struct Message
    {
        std::string Id;
        std::string Text;
    };

    std::mutex           g_MessageMutex;
    std::vector<Message> g_Messages;

    VKAPI_ATTR VkBool32 VKAPI_CALL CollectMessage( VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                   VkDebugUtilsMessageTypeFlagsEXT,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data, void* )
    {
        const std::lock_guard lock( g_MessageMutex );
        g_Messages.push_back(
             { data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "<no id>",
               data != nullptr && data->pMessage != nullptr ? data->pMessage : "<no message>" } );
        return VK_FALSE;
    }

    std::vector<Message> TakeMessages()
    {
        const std::lock_guard lock( g_MessageMutex );
        return std::exchange( g_Messages, {} );
    }

    std::string Join( const std::vector<Message>& messages )
    {
        std::string out;
        for ( const Message& message : messages )
            std::format_to( std::back_inserter( out ), "\n  [{}] {}", message.Id, message.Text );
        return out;
    }

    // One device for the whole suite.
    struct Gpu
    {
        vkb::Instance   Instance;
        vkb::Device     Device;
        DeviceCaps      Caps;
        VkQueue         Queue       = VK_NULL_HANDLE;
        uint32_t        QueueFamily = 0;
        VmaAllocator    Allocator   = nullptr;
        VkCommandPool   CommandPool = VK_NULL_HANDLE;
        VulkanRdgDevice Rdg;
        std::string     Error;
    };

    Gpu& GetGpu()
    {
        static Gpu gpu = []
        {
            Gpu out;
            // The loader is linked (as in VulkanContext.cpp): its vkGetInstanceProcAddr is handed to vk-bootstrap,
            // whose own dlopen("libvulkan.dylib") misses Homebrew's loader on macOS and never sees the layers.
            const auto system = vkb::SystemInfo::get_system_info( vkGetInstanceProcAddr );
            if ( !system || !system.value().validation_layers_available )
            {
                out.Error = "VK_LAYER_KHRONOS_validation is not installed; the suite's verdict needs it";
                return out;
            }
            // The instance is created as the engine's is (VulkanContext.cpp), because DeviceCaps plans the
            // device's extensions for that instance: routes assume apiVersion kMaximumApiVersion (a 1.1
            // instance caps the device at 1.1, so rows planned as 1.2/1.3 core - descriptor indexing, buffer
            // device address, SPIR-V 1.4 - would be missing extensions), and the required swapchain row needs
            // VK_KHR_surface, which a headless instance lacks. Both were 01387 at vkCreateDevice (VK-EXT1).
            vkb::InstanceBuilder builder( vkGetInstanceProcAddr );
            auto                 instance =
                 builder.set_app_name( "RenderGraphVulkan" )
                      .require_api_version( kMaximumApiVersion )
                      .set_minimum_instance_version( kMinimumDeviceApiVersion )
                      .request_validation_layers( true )
                      // The same feature VulkanContext.cpp enables (census below).
                      .add_validation_feature_enable( VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT )
                      .set_debug_callback( CollectMessage )
                      .set_debug_messenger_severity( VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT )
                      .set_debug_messenger_type( VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT )
                      .build();
            // "Available" only means the manifest was found (VulkanContext.cpp, PKG2): Homebrew's names its
            // library bare, and dyld finds it only through DYLD_FALLBACK_LIBRARY_PATH (scripts/Dev/_common.sh).
            if ( !instance && instance.vk_result() == VK_ERROR_LAYER_NOT_PRESENT )
            {
                out.Error = "VK_LAYER_KHRONOS_validation is listed but its library did not load "
                            "(VK_ERROR_LAYER_NOT_PRESENT); on macOS run with "
                            "DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib, VK_LOADER_DEBUG=layer names the path";
                return out;
            }
            if ( !instance )
            {
                out.Error = std::format( "instance: {} (VkResult {})", instance.error().message(),
                                         static_cast<int>( instance.vk_result() ) );
                return out;
            }
            out.Instance = instance.value();

            // The engine's judgement of the device, not a second one.
            auto probed = SelectDevice( out.Instance );
            if ( !probed )
            {
                out.Error = probed.GetError();
                return out;
            }
            out.Caps    = probed.GetValue().Caps;
            auto device = vkb::DeviceBuilder( *probed.GetValue().Physical ).build();
            if ( !device )
            {
                out.Error = std::format( "device: {}", device.error().message() );
                return out;
            }
            out.Device = device.value();
            // Creating the instance and the device must be validation-clean too (VK-EXT1: an extension
            // enabled without its dependencies is VUID-vkCreateDevice-ppEnabledExtensionNames-01387).
            if ( const std::vector<Message> messages = TakeMessages(); !messages.empty() )
            {
                out.Error = std::format( "{} validation message(s) while creating the device:{}", messages.size(),
                                         Join( messages ) );
                return out;
            }
            out.Queue       = out.Device.get_queue( vkb::QueueType::graphics ).value();
            out.QueueFamily = out.Device.get_queue_index( vkb::QueueType::graphics ).value();

            VmaAllocatorCreateInfo allocator{};
            allocator.physicalDevice   = out.Device.physical_device.physical_device;
            allocator.device           = out.Device.device;
            allocator.instance         = out.Instance.instance;
            allocator.vulkanApiVersion = UsedApiVersion( out.Device.physical_device.properties.apiVersion );
            if ( vmaCreateAllocator( &allocator, &out.Allocator ) != VK_SUCCESS )
            {
                out.Error = "vmaCreateAllocator failed";
                return out;
            }
            VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = out.QueueFamily;
            vkCreateCommandPool( out.Device.device, &pool, nullptr, &out.CommandPool );

            out.Rdg.Device        = out.Device.device;
            out.Rdg.Allocator     = out.Allocator;
            out.Rdg.CmdBeginLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                 vkGetInstanceProcAddr( out.Instance.instance, "vkCmdBeginDebugUtilsLabelEXT" ) );
            out.Rdg.CmdEndLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                 vkGetInstanceProcAddr( out.Instance.instance, "vkCmdEndDebugUtilsLabelEXT" ) );
            return out;
        }();
        return gpu;
    }

    Common::ResultStr<std::vector<uint32_t>> CompileGlsl( const char* source, shaderc_shader_kind kind,
                                                          const char* name )
    {
        const shaderc::Compiler             compiler;
        const shaderc::CompileOptions       options;
        const shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv( source, kind, name, options );
        if ( result.GetCompilationStatus() != shaderc_compilation_status_success )
            return Common::MakeFormattedError<std::vector<uint32_t>>( "{}: {}", name, result.GetErrorMessage() );
        return Common::MakeSuccess( std::vector<uint32_t>( result.cbegin(), result.cend() ) );
    }

    VkShaderModule Module( VkDevice device, const std::vector<uint32_t>& code )
    {
        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        info.codeSize          = code.size() * sizeof( uint32_t );
        info.pCode             = code.data();
        VkShaderModule module_ = VK_NULL_HANDLE;
        vkCreateShaderModule( device, &info, nullptr, &module_ );
        return module_;
    }

    constexpr const char* kVertex   = R"(#version 450
void main() { vec2 p = vec2( ( gl_VertexIndex << 1 ) & 2, gl_VertexIndex & 2 ); gl_Position = vec4( p * 2.0 - 1.0, 0.0, 1.0 ); })";
    constexpr const char* kFragment = R"(#version 450
layout( set = 0, binding = 0 ) uniform sampler2D src;
layout( location = 0 ) out vec4 colour;
void main() { colour = texelFetch( src, ivec2( gl_FragCoord.xy ), 0 ).bgra; })";
    constexpr const char* kCompute  = R"(#version 450
layout( local_size_x = 8, local_size_y = 8 ) in;
layout( set = 0, binding = 0 ) uniform sampler2D src;
layout( set = 0, binding = 1, rgba8 ) uniform writeonly image2D dst;
void main()
{
    ivec2 p = ivec2( gl_GlobalInvocationID.xy );
    vec4  t = texelFetch( src, p, 0 );
    imageStore( dst, p, vec4( 1.0 - t.rgb, float( p.x + 16 * p.y ) / 255.0 ) );
})";

    // Pipelines, descriptor sets and a sampler for the test graph. The graphics pipeline is built against
    // the canonical compatible render pass of its target layout, the same route engine pipelines with a
    // RenderTargetLayout take.
    struct Programs
    {
        VkDevice              Device            = VK_NULL_HANDLE;
        VkSampler             Sampler           = VK_NULL_HANDLE;
        VkDescriptorSetLayout DrawLayout        = VK_NULL_HANDLE;
        VkDescriptorSetLayout ComputeLayout     = VK_NULL_HANDLE;
        VkPipelineLayout      DrawPipeLayout    = VK_NULL_HANDLE;
        VkPipelineLayout      ComputePipeLayout = VK_NULL_HANDLE;
        VkRenderPass          Compatible        = VK_NULL_HANDLE;
        VkPipeline            Draw              = VK_NULL_HANDLE;
        VkPipeline            Compute           = VK_NULL_HANDLE;
        VkDescriptorPool      Pool              = VK_NULL_HANDLE;
        VkDescriptorSet       DrawSet           = VK_NULL_HANDLE;
        VkDescriptorSet       ComputeSet        = VK_NULL_HANDLE;
        std::string           Error;

        explicit Programs( const Gpu& gpu ) : Device( gpu.Device.device )
        {
            auto vs = CompileGlsl( kVertex, shaderc_vertex_shader, "fullscreen.vert" );
            auto fs = CompileGlsl( kFragment, shaderc_fragment_shader, "swizzle.frag" );
            auto cs = CompileGlsl( kCompute, shaderc_compute_shader, "invert.comp" );
            if ( !vs || !fs || !cs )
            {
                Error = !vs ? vs.GetError() : !fs ? fs.GetError() : cs.GetError();
                return;
            }
            VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
                 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            vkCreateSampler( Device, &sampler, nullptr, &Sampler );

            const VkDescriptorSetLayoutBinding drawBinding{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            const std::array<VkDescriptorSetLayoutBinding, 2> computeBindings = {
                 VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                               VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                 VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                               nullptr } };
            VkDescriptorSetLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layout.bindingCount = 1;
            layout.pBindings    = &drawBinding;
            vkCreateDescriptorSetLayout( Device, &layout, nullptr, &DrawLayout );
            layout.bindingCount = 2;
            layout.pBindings    = computeBindings.data();
            vkCreateDescriptorSetLayout( Device, &layout, nullptr, &ComputeLayout );

            VkPipelineLayoutCreateInfo pipeLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pipeLayout.setLayoutCount = 1;
            pipeLayout.pSetLayouts    = &DrawLayout;
            vkCreatePipelineLayout( Device, &pipeLayout, nullptr, &DrawPipeLayout );
            pipeLayout.pSetLayouts = &ComputeLayout;
            vkCreatePipelineLayout( Device, &pipeLayout, nullptr, &ComputePipeLayout );

            auto compatible =
                 CreateRdgRenderPass( Device, RdgCompatibleRenderPassKey( { VK_FORMAT_R8G8B8A8_UNORM },
                                                                          VK_FORMAT_UNDEFINED, false, 1 ) );
            if ( !compatible )
            {
                Error = compatible.GetError();
                return;
            }
            Compatible = compatible.GetValue();

            const VkShaderModule                                 vsModule = Module( Device, vs.GetValue() );
            const VkShaderModule                                 fsModule = Module( Device, fs.GetValue() );
            const VkShaderModule                                 csModule = Module( Device, cs.GetValue() );
            const std::array<VkPipelineShaderStageCreateInfo, 2> stages   = {
                 VkPipelineShaderStageCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                                  VK_SHADER_STAGE_VERTEX_BIT, vsModule, "main", nullptr },
                 VkPipelineShaderStageCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                                  VK_SHADER_STAGE_FRAGMENT_BIT, fsModule, "main", nullptr } };
            const VkPipelineVertexInputStateCreateInfo vertexInput{
                 VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo assembly{
                 VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            viewport.viewportCount = 1;
            viewport.scissorCount  = 1;
            VkPipelineRasterizationStateCreateInfo raster{
                 VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode    = VK_CULL_MODE_NONE;
            raster.lineWidth   = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{
                 VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blendAttachment{};
            blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            blend.attachmentCount                        = 1;
            blend.pAttachments                           = &blendAttachment;
            const std::array<VkDynamicState, 2> dynamics = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo    dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamic.dynamicStateCount = 2;
            dynamic.pDynamicStates    = dynamics.data();
            VkGraphicsPipelineCreateInfo graphics{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            graphics.stageCount          = 2;
            graphics.pStages             = stages.data();
            graphics.pVertexInputState   = &vertexInput;
            graphics.pInputAssemblyState = &assembly;
            graphics.pViewportState      = &viewport;
            graphics.pRasterizationState = &raster;
            graphics.pMultisampleState   = &multisample;
            graphics.pColorBlendState    = &blend;
            graphics.pDynamicState       = &dynamic;
            graphics.layout              = DrawPipeLayout;
            graphics.renderPass          = Compatible;
            vkCreateGraphicsPipelines( Device, VK_NULL_HANDLE, 1, &graphics, nullptr, &Draw );

            VkComputePipelineCreateInfo compute{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            compute.stage  = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                               nullptr,
                               0,
                               VK_SHADER_STAGE_COMPUTE_BIT,
                               csModule,
                               "main",
                               nullptr };
            compute.layout = ComputePipeLayout;
            vkCreateComputePipelines( Device, VK_NULL_HANDLE, 1, &compute, nullptr, &Compute );
            vkDestroyShaderModule( Device, vsModule, nullptr );
            vkDestroyShaderModule( Device, fsModule, nullptr );
            vkDestroyShaderModule( Device, csModule, nullptr );

            const std::array<VkDescriptorPoolSize, 2> sizes = {
                 VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
                 VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 } };
            VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pool.maxSets       = 2;
            pool.poolSizeCount = 2;
            pool.pPoolSizes    = sizes.data();
            vkCreateDescriptorPool( Device, &pool, nullptr, &Pool );
            const std::array<VkDescriptorSetLayout, 2> layouts = { DrawLayout, ComputeLayout };
            std::array<VkDescriptorSet, 2>             sets{};
            VkDescriptorSetAllocateInfo                allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            allocate.descriptorPool     = Pool;
            allocate.descriptorSetCount = 2;
            allocate.pSetLayouts        = layouts.data();
            vkAllocateDescriptorSets( Device, &allocate, sets.data() );
            DrawSet    = sets[0];
            ComputeSet = sets[1];
            if ( Draw == VK_NULL_HANDLE || Compute == VK_NULL_HANDLE )
                Error = "pipeline creation failed";
        }

        ~Programs()
        {
            vkDestroyDescriptorPool( Device, Pool, nullptr );
            vkDestroyPipeline( Device, Draw, nullptr );
            vkDestroyPipeline( Device, Compute, nullptr );
            vkDestroyRenderPass( Device, Compatible, nullptr );
            vkDestroyPipelineLayout( Device, DrawPipeLayout, nullptr );
            vkDestroyPipelineLayout( Device, ComputePipeLayout, nullptr );
            vkDestroyDescriptorSetLayout( Device, DrawLayout, nullptr );
            vkDestroyDescriptorSetLayout( Device, ComputeLayout, nullptr );
            vkDestroySampler( Device, Sampler, nullptr );
        }
    };

    RDG::TextureDesc Target()
    {
        RDG::TextureDesc desc;
        desc.Size   = { kSize, kSize, 1 };
        desc.Format = ImageFormat::RGBA8F;
        return desc;
    }

    struct RunResult
    {
        std::string            Error;
        std::vector<uint8_t>   Bytes;
        std::array<VkImage, 3> Images{}; // Cleared, Sampled, Computed as the passes saw them
        bool                   CommandBufferOfMatched = false;
    };

    Common::BoolResultStr Fail( const std::string& what )
    {
        return Common::MakeError( what );
    }

    // Builds clear -> sample -> compute -> copy, executes it through @p backend on @p commandBuffer,
    // submits, waits and reads the extracted buffer back.
    RunResult RunGraph( Gpu& gpu, Programs& programs, RDG::IBackend& backend, VulkanRdgBackend& vulkan,
                        VulkanRdgPool& pool )
    {
        RunResult                   run;
        VkCommandBuffer             cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool        = gpu.CommandPool;
        allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        vkAllocateCommandBuffers( gpu.Device.device, &allocate, &cmd );
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer( cmd, &begin );
        pool.BeginFrame( 0 );
        vulkan.SetCommandBuffer( cmd );

        RDG::ExternalBuffer   readback;
        RDG::Builder          graph( "clear-sample-compute-copy" );
        const RDG::TextureRef cleared  = graph.CreateTexture( Target(), "Cleared" );
        const RDG::TextureRef sampled  = graph.CreateTexture( Target(), "Sampled" );
        const RDG::TextureRef computed = graph.CreateTexture( Target(), "Computed" );
        const RDG::BufferRef  bytes    = graph.CreateBuffer( RDG::BufferDesc{ kSize * kSize * 4 }, "Readback" );

        graph.AddPass(
             "Clear", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.ColorTarget(
                      0, cleared,
                      RDG::LoadOp::ClearColor( 64.0f / 255.0f, 128.0f / 255.0f, 192.0f / 255.0f, 1.0f ) );
             },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 auto texture = context.GetTexture( cleared, RDG::Access::ColorTarget );
                 if ( !texture )
                     return Fail( texture.GetError() );
                 auto image = VulkanRdgBackend::TextureOf( texture.GetValue() );
                 if ( !image )
                     return Fail( image.GetError() );
                 run.Images[0]              = image.GetValue()->GetImage();
                 auto recorded              = VulkanRdgBackend::CommandBufferOf( context );
                 run.CommandBufferOfMatched = recorded && recorded.GetValue() == cmd;
                 return Common::MakeSuccess( true ); // the render pass's load op is the whole pass
             } );
        graph.AddPass(
             "Sample", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( cleared, RDG::Access::SampledGraphics );
                 pass.ColorTarget( 0, sampled, RDG::LoadOp::DontCare() );
             },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 auto source = context.GetTexture( cleared, RDG::Access::SampledGraphics );
                 auto target = context.GetTexture( sampled, RDG::Access::ColorTarget );
                 if ( !source || !target )
                     return Fail( !source ? source.GetError() : target.GetError() );
                 auto sourceImage = VulkanRdgBackend::TextureOf( source.GetValue() );
                 auto targetImage = VulkanRdgBackend::TextureOf( target.GetValue() );
                 if ( !sourceImage || !targetImage )
                     return Fail( "no Vulkan image" );
                 run.Images[1] = targetImage.GetValue()->GetImage();
                 auto view     = sourceImage.GetValue()->GetView();
                 if ( !view )
                     return Fail( view.GetError() );
                 const VkDescriptorImageInfo info{ programs.Sampler, view.GetValue(),
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                 VkWriteDescriptorSet        write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                 write.dstSet          = programs.DrawSet;
                 write.dstBinding      = 0;
                 write.descriptorCount = 1;
                 write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                 write.pImageInfo      = &info;
                 vkUpdateDescriptorSets( gpu.Device.device, 1, &write, 0, nullptr );
                 vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, programs.Draw );
                 vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, programs.DrawPipeLayout, 0, 1,
                                          &programs.DrawSet, 0, nullptr );
                 vkCmdDraw( cmd, 3, 1, 0, 0 );
                 return Common::MakeSuccess( true );
             } );
        graph.AddPass(
             "Invert", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( sampled, RDG::Access::SampledCompute );
                 pass.Write( computed, RDG::Access::StorageWrite );
             },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 auto source = context.GetTexture( sampled, RDG::Access::SampledCompute );
                 auto target = context.GetTexture( computed, RDG::Access::StorageWrite );
                 if ( !source || !target )
                     return Fail( !source ? source.GetError() : target.GetError() );
                 auto sourceImage = VulkanRdgBackend::TextureOf( source.GetValue() );
                 auto targetImage = VulkanRdgBackend::TextureOf( target.GetValue() );
                 if ( !sourceImage || !targetImage )
                     return Fail( "no Vulkan image" );
                 run.Images[2]   = targetImage.GetValue()->GetImage();
                 auto sourceView = sourceImage.GetValue()->GetView();
                 auto targetView = targetImage.GetValue()->GetView();
                 if ( !sourceView || !targetView )
                     return Fail( "no view" );
                 const VkDescriptorImageInfo         sourceInfo{ programs.Sampler, sourceView.GetValue(),
                                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                 const VkDescriptorImageInfo         targetInfo{ VK_NULL_HANDLE, targetView.GetValue(),
                                                         VK_IMAGE_LAYOUT_GENERAL };
                 std::array<VkWriteDescriptorSet, 2> writes{};
                 for ( uint32_t i = 0; i < 2; ++i )
                 {
                     writes[i]                 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                     writes[i].dstSet          = programs.ComputeSet;
                     writes[i].dstBinding      = i;
                     writes[i].descriptorCount = 1;
                     writes[i].descriptorType =
                          i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                     writes[i].pImageInfo = i == 0 ? &sourceInfo : &targetInfo;
                 }
                 vkUpdateDescriptorSets( gpu.Device.device, 2, writes.data(), 0, nullptr );
                 vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, programs.Compute );
                 vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, programs.ComputePipeLayout, 0, 1,
                                          &programs.ComputeSet, 0, nullptr );
                 vkCmdDispatch( cmd, kSize / 8, kSize / 8, 1 );
                 return Common::MakeSuccess( true );
             } );
        graph.AddPass(
             "Copy", RDG::PassFlags::Copy,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( computed, RDG::Access::CopySrc );
                 pass.Write( bytes, RDG::Access::CopyDst );
             },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 auto source = context.GetTexture( computed, RDG::Access::CopySrc );
                 auto target = context.GetBuffer( bytes, RDG::Access::CopyDst );
                 if ( !source || !target )
                     return Fail( !source ? source.GetError() : target.GetError() );
                 auto image  = VulkanRdgBackend::TextureOf( source.GetValue() );
                 auto buffer = VulkanRdgBackend::BufferOf( target.GetValue() );
                 if ( !image || !buffer )
                     return Fail( "no Vulkan resource" );
                 VkBufferImageCopy region{};
                 region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                 region.imageExtent      = { kSize, kSize, 1 };
                 vkCmdCopyImageToBuffer( cmd, image.GetValue()->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         buffer.GetValue()->GetBuffer(), 1, &region );
                 return Common::MakeSuccess( true );
             } );
        graph.Extract( bytes, readback, RDG::Access::HostRead );

        const Common::BoolResultStr executed = graph.Execute( backend );
        vkEndCommandBuffer( cmd );
        if ( !executed )
        {
            run.Error = executed.GetError();
            vkFreeCommandBuffers( gpu.Device.device, gpu.CommandPool, 1, &cmd );
            return run;
        }
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;
        vkQueueSubmit( gpu.Queue, 1, &submit, VK_NULL_HANDLE );
        vkQueueWaitIdle( gpu.Queue );
        vkFreeCommandBuffers( gpu.Device.device, gpu.CommandPool, 1, &cmd );

        if ( !readback.Physical || readback.Physical->GetBackendKind() != RDG::BackendKind::Vulkan )
        {
            run.Error = "the extracted readback buffer carries no Vulkan buffer";
            return run;
        }
        const auto& buffer = static_cast<const VulkanRdgBuffer&>( *readback.Physical );
        buffer.InvalidateForHost();
        if ( buffer.GetMapped() == nullptr )
        {
            run.Error = "the readback buffer is not host-visible";
            return run;
        }
        run.Bytes.resize( kSize * kSize * 4 );
        std::memcpy( run.Bytes.data(), buffer.GetMapped(), run.Bytes.size() );
        return run;
    }

    std::vector<uint8_t> Expected()
    {
        // clear (64,128,192,255) -> sample swizzles to (192,128,64,255) -> compute stores
        // (255 - rgb, x + 16 y).
        std::vector<uint8_t> out;
        for ( uint32_t y = 0; y < kSize; ++y )
            for ( uint32_t x = 0; x < kSize; ++x )
                out.insert( out.end(), { 63, 127, 191, static_cast<uint8_t>( x + 16 * y ) } );
        return out;
    }

    std::string FirstMismatch( const std::vector<uint8_t>& got, const std::vector<uint8_t>& want )
    {
        if ( got.size() != want.size() )
            return std::format( "size {} != {}", got.size(), want.size() );
        for ( size_t i = 0; i < got.size(); ++i )
        {
            if ( got[i] != want[i] )
                return std::format( "byte {} (pixel {}, channel {}): {} != {}", i, i / 4, i % 4, got[i], want[i] );
        }
        return {};
    }

    // Forwards everything to the Vulkan backend, except that it strips the source stages and accesses
    // from the barriers of ONE pass - the layout transitions stay, the execution and memory dependency
    // on the previous pass is gone. Exactly the defect synchronization validation exists to catch.
    class WeakenedBarriers final : public RDG::IBackend
    {
    public:
        WeakenedBarriers( VulkanRdgBackend& inner, std::string pass )
             : m_Inner( inner ), m_Pass( std::move( pass ) )
        {
        }

        [[nodiscard]] RDG::BackendKind GetKind() const override
        {
            return RDG::BackendKind::Recording;
        }
        [[nodiscard]] const RDG::IMemoryRequirementsProvider& GetMemoryRequirements() const override
        {
            return m_Inner.GetMemoryRequirements();
        }
        Common::BoolResultStr BeginGraph( const RDG::GraphView& graph ) override
        {
            return m_Inner.BeginGraph( graph );
        }
        void BeginPass( const RDG::CompiledPass& pass ) override
        {
            m_Current = pass.Name;
            m_Inner.BeginPass( pass );
        }
        void RecordBarriers( std::span<const RDG::Barrier> barriers ) override
        {
            if ( m_Current != m_Pass )
            {
                m_Inner.RecordBarriers( barriers );
                return;
            }
            std::vector<RDG::Barrier> weakened( barriers.begin(), barriers.end() );
            for ( RDG::Barrier& barrier : weakened )
            {
                barrier.Before.Stages = RDG::PipelineStage_None;
                barrier.Before.Memory = RDG::MemoryAccess_None;
            }
            m_Inner.RecordBarriers( weakened );
        }
        Common::BoolResultStr BeginRenderPass( const RDG::CompiledPass& pass ) override
        {
            return m_Inner.BeginRenderPass( pass );
        }
        void EndRenderPass() override
        {
            m_Inner.EndRenderPass();
        }
        void EndPass( const RDG::CompiledPass& pass ) override
        {
            m_Inner.EndPass( pass );
        }
        Common::BoolResultStr EndGraph( std::span<const RDG::Barrier> finalBarriers ) override
        {
            return m_Inner.EndGraph( finalBarriers );
        }
        void AbandonGraph() override
        {
            m_Inner.AbandonGraph();
        }
        [[nodiscard]] std::shared_ptr<RDG::IPhysicalTexture> GetPhysicalTexture( uint32_t resource ) const override
        {
            return m_Inner.GetPhysicalTexture( resource );
        }
        [[nodiscard]] std::shared_ptr<RDG::IPhysicalBuffer> GetPhysicalBuffer( uint32_t resource ) const override
        {
            return m_Inner.GetPhysicalBuffer( resource );
        }

    private:
        VulkanRdgBackend& m_Inner;
        std::string       m_Pass;
        std::string       m_Current;
    };

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

// The acceptance graph: byte-exact readback, validation (synchronization validation included) silent,
// and on a second run of the unchanged graph the pool hands back the very same images.
TEST( RenderGraphVulkan, ClearSampleComputeCopyIsByteExactAndValidationClean )
{
    Gpu& gpu = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    {
        Programs programs( gpu );
        ASSERT_TRUE( programs.Error.empty() ) << programs.Error;
        VulkanRdgPool    pool( gpu.Rdg, 1 );
        VulkanRdgBackend backend( gpu.Rdg, pool );

        const RunResult first = RunGraph( gpu, programs, backend, backend, pool );
        ASSERT_TRUE( first.Error.empty() ) << first.Error;
        EXPECT_TRUE( first.CommandBufferOfMatched );
        EXPECT_EQ( FirstMismatch( first.Bytes, Expected() ), "" );

        const RunResult second = RunGraph( gpu, programs, backend, backend, pool );
        ASSERT_TRUE( second.Error.empty() ) << second.Error;
        EXPECT_EQ( FirstMismatch( second.Bytes, Expected() ), "" );
        EXPECT_EQ( second.Images, first.Images ) << "an unchanged graph must get the same images from the pool";
        EXPECT_EQ( pool.GetTextureCount(), 3u );
        vkDeviceWaitIdle( gpu.Device.device );
    }
    const std::vector<Message> messages = TakeMessages();
    EXPECT_TRUE( messages.empty() ) << messages.size() << " validation message(s):" << Join( messages );
}

// The control: the same graph with the Invert pass's barriers stripped of their source dependency. If
// synchronization validation were off, nothing would be reported and the clean run above would prove
// nothing.
TEST( RenderGraphVulkan, SynchronizationValidationReportsAWeakenedBarrier )
{
    Gpu& gpu = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    {
        Programs programs( gpu );
        ASSERT_TRUE( programs.Error.empty() ) << programs.Error;
        VulkanRdgPool    pool( gpu.Rdg, 1 );
        VulkanRdgBackend backend( gpu.Rdg, pool );
        WeakenedBarriers weakened( backend, "Invert" );
        const RunResult  run = RunGraph( gpu, programs, weakened, backend, pool );
        ASSERT_TRUE( run.Error.empty() ) << run.Error;
        vkDeviceWaitIdle( gpu.Device.device );
    }
    const std::vector<Message> messages = TakeMessages();
    const bool                 hazard   = std::any_of( messages.begin(), messages.end(),
                                                       []( const Message& m ) { return m.Id.starts_with( "SYNC-HAZARD-" ); } );
    EXPECT_TRUE( hazard ) << "no SYNC-HAZARD among " << messages.size() << " message(s):" << Join( messages );
}

// The engine's instance asks for the same thing the suite relies on.
TEST( RenderGraphVulkan, TheEngineInstanceEnablesSynchronizationValidation )
{
    const fs::path root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const std::ifstream file( root / "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanContext.cpp" );
    std::stringstream   text;
    text << file.rdbuf();
    const std::string source = text.str();
    const size_t      layers = source.find( "builder.request_validation_layers( true );" );
    // The call is found by its parts: the formatter may wrap it.
    const size_t call = source.find( "builder.add_validation_feature_enable(", layers );
    const size_t sync = source.find( "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", call );
    ASSERT_NE( call, std::string::npos );
    ASSERT_NE( layers, std::string::npos );
    ASSERT_NE( sync, std::string::npos );
    // In the same branch: within a few lines after the layers are requested.
    EXPECT_LT( sync - layers, 600u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
