// RenderGraphVulkan - the render graph's Vulkan executor on a real device (RDG2 acceptance). The graph
// clear -> sample -> compute -> copy runs on a windowless device picked by the engine's own DeviceCaps
// judgement, under VK_LAYER_KHRONOS_validation WITH synchronization validation; the readback must match
// byte for byte and the layer must say nothing. A control test weakens one pass's barriers and requires
// synchronization validation to report the hazard - the proof that the silence above means something.

#include <Engine/Graphic/API/Vulkan/DeviceCapsProbe.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRdgTransient.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderPassDependencies.hpp>

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
#include <optional>
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
        std::lock_guard lock( g_MessageMutex );
        g_Messages.push_back( { data && data->pMessageIdName ? data->pMessageIdName : "<no id>",
                                data && data->pMessage ? data->pMessage : "<no message>" } );
        return VK_FALSE;
    }

    std::vector<Message> TakeMessages()
    {
        std::lock_guard lock( g_MessageMutex );
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
        VkQueue         ComputeQueue  = VK_NULL_HANDLE; // a queue of a family other than QueueFamily, if any
        uint32_t        ComputeFamily = 0;
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
            if ( !instance )
            {
                out.Error = std::format( "instance: {}", instance.error().message() );
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
            // vk-bootstrap creates a queue in every family; its compute index is one without graphics.
            if ( auto compute = out.Device.get_queue_index( vkb::QueueType::compute ); compute )
            {
                out.ComputeFamily = compute.value();
                out.ComputeQueue  = out.Device.get_queue( vkb::QueueType::compute ).value();
            }

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
        shaderc::Compiler                   compiler;
        shaderc::CompileOptions             options;
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
            VkPipelineVertexInputStateCreateInfo vertexInput{
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

    // The command buffer the graph records a pass into (its pipe segment's, since RDG-ASYNC B2), not the
    // caller's: VK_NULL_HANDLE for a context that is not the Vulkan backend's.
    VkCommandBuffer PassCommandBuffer( const RDG::PassContext& context )
    {
        const Common::ResultStr<VkCommandBuffer> recorded = VulkanRdgBackend::CommandBufferOf( context );
        return recorded ? recorded.GetValue() : VK_NULL_HANDLE;
    }

    // What the frame loop hands the Vulkan backend for its frame slots (RDG-ALIAS A1): the transient heaps and
    // the per-pass descriptor pools, and (B2) the segment command pools + semaphores. Without @p asyncCompute
    // (or on a device with no other family) the queue set has no compute queue: every pass runs on Graphics.
    struct FrameObjects
    {
        FrameObjects( Gpu& gpu, uint32_t frameSlots, bool asyncCompute = false )
             : Transients( gpu.Rdg, frameSlots ), Descriptors( gpu.Device.device, frameSlots ),
               Objects( gpu.Device.device, gpu.QueueFamily,
                        asyncCompute && gpu.ComputeQueue != VK_NULL_HANDLE
                             ? std::optional<uint32_t>( gpu.ComputeFamily )
                             : std::nullopt,
                        frameSlots )
        {
            Queues.GraphicsQueue  = gpu.Queue;
            Queues.GraphicsFamily = gpu.QueueFamily;
            Queues.Objects        = &Objects;
            if ( asyncCompute )
            {
                Queues.ComputeQueue  = gpu.ComputeQueue;
                Queues.ComputeFamily = gpu.ComputeFamily;
            }
        }

        VulkanRdgQueueSet           Queues;
        VulkanRdgTransientAllocator Transients;
        VulkanRdgPassDescriptors    Descriptors;
        VulkanRdgQueueObjects       Objects;
    };

    // The frame loop's submission of what was recorded (RDG-CONTRACTS B(3)): the caller's own command buffer
    // (work recorded before the graph) first, then the graph's segments in order, @p fence on the last.
    void SubmitFrame( Gpu& gpu, VulkanRdgBackend& backend, const VkSubmitInfo& before, VkFence fence )
    {
        const std::vector<VulkanRdgSubmission> segments = backend.TakeSubmissions();
        vkQueueSubmit( gpu.Queue, 1, &before, segments.empty() ? fence : VK_NULL_HANDLE );
        for ( size_t i = 0; i < segments.size(); ++i )
        {
            const VulkanRdgSubmission& entry = segments[i];
            VkSubmitInfo               info{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            info.waitSemaphoreCount   = static_cast<uint32_t>( entry.WaitSemaphores.size() );
            info.pWaitSemaphores      = entry.WaitSemaphores.data();
            info.pWaitDstStageMask    = entry.WaitStages.data();
            info.commandBufferCount   = 1;
            info.pCommandBuffers      = &entry.CommandBuffer;
            info.signalSemaphoreCount = static_cast<uint32_t>( entry.SignalSemaphores.size() );
            info.pSignalSemaphores    = entry.SignalSemaphores.data();
            vkQueueSubmit( entry.Queue, 1, &info, i + 1 == segments.size() ? fence : VK_NULL_HANDLE );
        }
    }

    // The frame loop's order after the slot's fence (here: the previous submission of the slot was waited):
    // the pool, the transient heaps and the descriptor pools are re-begun, then the backend is bound to them.
    Common::BoolResultStr BeginFrameSlot( FrameObjects& frame, VulkanRdgBackend& backend, VulkanRdgPool& pool,
                                          uint32_t slot )
    {
        pool.BeginFrame( slot );
        frame.Transients.BeginFrameSlot( slot );
        frame.Descriptors.BeginFrameSlot( slot );
        if ( Common::BoolResultStr objects = frame.Objects.BeginFrameSlot( slot ); !objects )
            return objects;
        return backend.BeginFrame( slot, frame.Queues, frame.Transients, frame.Descriptors );
    }

    // Builds clear -> sample -> compute -> copy, executes it through @p backend on @p commandBuffer,
    // submits, waits and reads the extracted buffer back.
    RunResult RunGraph( Gpu& gpu, Programs& programs, RDG::IBackend& backend, VulkanRdgBackend& vulkan,
                        VulkanRdgPool& pool, FrameObjects& frame )
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
        if ( const Common::BoolResultStr begun = BeginFrameSlot( frame, vulkan, pool, 0 ); !begun )
        {
            run.Error = begun.GetError();
            vkEndCommandBuffer( cmd );
            vkFreeCommandBuffers( gpu.Device.device, gpu.CommandPool, 1, &cmd );
            return run;
        }

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
                 run.CommandBufferOfMatched =
                      recorded && recorded.GetValue() != VK_NULL_HANDLE && recorded.GetValue() != cmd;
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
                 const VkCommandBuffer passCmd = vulkan.GetRecordingCommandBuffer();
                 if ( passCmd == VK_NULL_HANDLE )
                     return Fail( "the pass has no recording command buffer" );
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
                 vkCmdBindPipeline( passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, programs.Draw );
                 vkCmdBindDescriptorSets( passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, programs.DrawPipeLayout, 0, 1,
                                          &programs.DrawSet, 0, nullptr );
                 vkCmdDraw( passCmd, 3, 1, 0, 0 );
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
                 const VkCommandBuffer passCmd = vulkan.GetRecordingCommandBuffer();
                 if ( passCmd == VK_NULL_HANDLE )
                     return Fail( "the pass has no recording command buffer" );
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
                 vkCmdBindPipeline( passCmd, VK_PIPELINE_BIND_POINT_COMPUTE, programs.Compute );
                 vkCmdBindDescriptorSets( passCmd, VK_PIPELINE_BIND_POINT_COMPUTE, programs.ComputePipeLayout, 0, 1,
                                          &programs.ComputeSet, 0, nullptr );
                 vkCmdDispatch( passCmd, kSize / 8, kSize / 8, 1 );
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
                 const VkCommandBuffer passCmd = vulkan.GetRecordingCommandBuffer();
                 if ( passCmd == VK_NULL_HANDLE )
                     return Fail( "the pass has no recording command buffer" );
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
                 vkCmdCopyImageToBuffer( passCmd, image.GetValue()->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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
        SubmitFrame( gpu, vulkan, submit, VK_NULL_HANDLE );
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

        RDG::BackendKind GetKind() const override
        {
            return RDG::BackendKind::Recording;
        }
        const RDG::IMemoryRequirementsProvider& GetMemoryRequirements() const override
        {
            return m_Inner.GetMemoryRequirements();
        }
        // The RDG-CONTRACTS additions go to the Vulkan backend unchanged.
        RDG::ITransientAllocator& GetTransientAllocator() override
        {
            return m_Inner.GetTransientAllocator();
        }
        RDG::PipeCapabilities GetPipeCapabilities() const override
        {
            return m_Inner.GetPipeCapabilities();
        }
        RDG::AsyncComputeFallbackLog& GetAsyncComputeFallbackLog() override
        {
            return m_Inner.GetAsyncComputeFallbackLog();
        }
        Common::BoolResultStr BeginPipeSegment( const RDG::PipeSegment& segment ) override
        {
            return m_Inner.BeginPipeSegment( segment );
        }
        Common::BoolResultStr EndPipeSegment( const RDG::PipeSegment& segment ) override
        {
            return m_Inner.EndPipeSegment( segment );
        }
        void RecordEpilogueBarriers( std::span<const RDG::Barrier> barriers ) override
        {
            m_Inner.RecordEpilogueBarriers( barriers );
        }
        Common::BoolResultStr BeginGraph( const RDG::GraphView& graph ) override
        {
            if ( graph.Result != nullptr )
                Plan = graph.Result->Aliasing;
            return m_Inner.BeginGraph( graph );
        }
        void BeginPass( const RDG::CompiledPass& pass ) override
        {
            m_Current = pass.Name;
            m_Inner.BeginPass( pass );
        }
        void RecordBarriers( std::span<const RDG::Barrier> barriers ) override
        {
            for ( const RDG::Barrier& barrier : barriers )
                Recorded.push_back( { m_Current, barrier } );
            if ( m_Current != m_Pass )
                return m_Inner.RecordBarriers( barriers );
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
            ++RenderPassesBegun;
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
        std::shared_ptr<RDG::IPhysicalTexture> GetPhysicalTexture( uint32_t resource ) const override
        {
            return m_Inner.GetPhysicalTexture( resource );
        }
        std::shared_ptr<RDG::IPhysicalBuffer> GetPhysicalBuffer( uint32_t resource ) const override
        {
            return m_Inner.GetPhysicalBuffer( resource );
        }

        // Every BeginRenderPass the graph asked for: one vkCmdBeginRenderPass each in the Vulkan backend.
        int RenderPassesBegun = 0;
        // The aliasing plan of the last BeginGraph, and every barrier recorded (pass name, barrier as given).
        RDG::AliasingPlan                                 Plan;
        std::vector<std::pair<std::string, RDG::Barrier>> Recorded;

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
// and on a second run of the unchanged graph the transient allocator hands back the very same images.
TEST( RenderGraphVulkan, ClearSampleComputeCopyIsByteExactAndValidationClean )
{
    Gpu& gpu = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    {
        Programs programs( gpu );
        ASSERT_TRUE( programs.Error.empty() ) << programs.Error;
        VulkanRdgPool    pool( gpu.Rdg, 1 );
        FrameObjects     frameObjects( gpu, 1 );
        VulkanRdgBackend backend( gpu.Rdg, pool );

        const RunResult first = RunGraph( gpu, programs, backend, backend, pool, frameObjects );
        ASSERT_TRUE( first.Error.empty() ) << first.Error;
        EXPECT_TRUE( first.CommandBufferOfMatched );
        EXPECT_EQ( FirstMismatch( first.Bytes, Expected() ), "" );

        const RunResult second = RunGraph( gpu, programs, backend, backend, pool, frameObjects );
        ASSERT_TRUE( second.Error.empty() ) << second.Error;
        EXPECT_EQ( FirstMismatch( second.Bytes, Expected() ), "" );
        EXPECT_EQ( second.Images, first.Images )
             << "an unchanged plan must get the same placed images from the transient allocator";
        // The three textures are placed in the graph's heap; only the extracted readback comes from the pool.
        EXPECT_EQ( pool.GetTextureCount(), 0u );
        EXPECT_EQ( frameObjects.Transients.GetStats().PlacedResources, 3u );
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
        FrameObjects     frameObjects( gpu, 1 );
        VulkanRdgBackend backend( gpu.Rdg, pool );
        WeakenedBarriers weakened( backend, "Invert" );
        const RunResult  run = RunGraph( gpu, programs, weakened, backend, pool, frameObjects );
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
    std::ifstream     file( root / "Desert/Desert/Source/Engine/Graphic/API/Vulkan/VulkanContext.cpp" );
    std::stringstream text;
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

// An engine image imported as a framebuffer attachment: two consecutive Raster nodes on it (CLEAR, then
// LOAD) share ONE render pass the graph opens, a copy reads it back, the layer says nothing, and the layout
// handed to RecordStates at the end is the graph's final layout. The second frame imports the image from that
// recorded layout, as the engine's next frame does.
TEST( RenderGraphVulkan, AnImportedFramebufferSharesOneRenderPassAndWritesItsLayoutBack )
{
    Gpu& gpu = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent        = { kSize, kSize, 1 };
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo memory{};
    memory.usage             = VMA_MEMORY_USAGE_AUTO;
    VkImage       image      = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    ASSERT_EQ( vmaCreateImage( gpu.Allocator, &info, &memory, &image, &allocation, nullptr ), VK_SUCCESS );
    VkImageLayout recorded = VK_IMAGE_LAYOUT_UNDEFINED; // the image's own layout record, as VulkanImage2D keeps it
    {
        VulkanRdgPool    pool( gpu.Rdg, 1 );
        FrameObjects     frameObjects( gpu, 1 );
        VulkanRdgBackend backend( gpu.Rdg, pool );
        WeakenedBarriers counting( backend, "" ); // weakens no pass: it only counts the render passes
        const std::shared_ptr<VulkanRdgTexture> wrapped =
             VulkanRdgTexture::Wrap( gpu.Device.device, image, VK_FORMAT_R8G8B8A8_UNORM, Target() );
        for ( int frame = 0; frame < 2; ++frame )
        {
            const std::optional<RDG::ImageLayout> from = RdgLayoutFromVulkan( recorded );
            ASSERT_TRUE( from.has_value() ) << "frame " << frame;
            RDG::ExternalTexture target;
            target.Desc = Target();
            target.SubresourceStates.assign( target.Desc.SubresourceCount(), RDG::RecordedLayoutState( *from ) );
            target.Physical          = wrapped;
            int writeBacks           = 0;
            target.RecordStates      = [&]( const std::vector<RDG::AccessState>& states,
                                       bool                                 graphEnded ) -> Common::BoolResultStr
            {
                if ( !graphEnded )
                    return Common::MakeSuccess( true );
                ++writeBacks;
                recorded = RdgVulkanLayout( states.front().Layout );
                return Common::MakeSuccess( true );
            };

            VkCommandBuffer             cmd = VK_NULL_HANDLE;
            VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocate.commandPool        = gpu.CommandPool;
            allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            vkAllocateCommandBuffers( gpu.Device.device, &allocate, &cmd );
            VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer( cmd, &begin );
            ASSERT_TRUE( BeginFrameSlot( frameObjects, backend, pool, 0 ) );

            RDG::ExternalBuffer                        readback;
            RDG::Builder                               graph( "imported-framebuffer" );
            const std::array<RDG::ExternalTexture*, 1> colors = { &target };
            const RDG::ImportedFramebuffer             fb = graph.ImportFramebuffer( colors, nullptr, "Scene" );
            const RDG::BufferRef bytes = graph.CreateBuffer( RDG::BufferDesc{ kSize * kSize * 4 }, "Readback" );
            ASSERT_EQ( fb.Colors.size(), 1u );
            graph.AddPass(
                 "Clear", RDG::PassFlags::Raster, [&]( RDG::PassBuilder& pass )
                 { pass.ColorTarget( 0, fb.Colors[0], RDG::LoadOp::ClearColor( 0.0f, 1.0f, 0.0f, 1.0f ) ); },
                 []( RDG::PassContext& ) -> Common::BoolResultStr { return Common::MakeSuccess( true ); } );
            graph.AddPass(
                 "Overlay", RDG::PassFlags::Raster,
                 [&]( RDG::PassBuilder& pass ) { pass.ColorTarget( 0, fb.Colors[0], RDG::LoadOp::Load() ); },
                 []( RDG::PassContext& ) -> Common::BoolResultStr { return Common::MakeSuccess( true ); } );
            graph.AddPass(
                 "Readback", RDG::PassFlags::Copy,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Read( fb.Colors[0], RDG::Access::CopySrc );
                     pass.Write( bytes, RDG::Access::CopyDst );
                 },
                 [&]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     // The graph runs through the counting decorator, so the context's backend is not the
                     // Vulkan one: the segment's command buffer comes from the Vulkan backend it wraps.
                     const VkCommandBuffer passCmd = backend.GetRecordingCommandBuffer();
                     if ( passCmd == VK_NULL_HANDLE )
                         return Fail( "the pass has no recording command buffer" );
                     auto source = context.GetTexture( fb.Colors[0], RDG::Access::CopySrc );
                     auto dest   = context.GetBuffer( bytes, RDG::Access::CopyDst );
                     if ( !source || !dest )
                         return Fail( !source ? source.GetError() : dest.GetError() );
                     auto sourceImage = VulkanRdgBackend::TextureOf( source.GetValue() );
                     auto buffer      = VulkanRdgBackend::BufferOf( dest.GetValue() );
                     if ( !sourceImage || !buffer )
                         return Fail( "no Vulkan resource" );
                     VkBufferImageCopy region{};
                     region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                     region.imageExtent      = { kSize, kSize, 1 };
                     vkCmdCopyImageToBuffer( passCmd, sourceImage.GetValue()->GetImage(),
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.GetValue()->GetBuffer(),
                                             1, &region );
                     return Common::MakeSuccess( true );
                 } );
            graph.Extract( bytes, readback, RDG::Access::HostRead );

            const int                   before   = counting.RenderPassesBegun;
            const Common::BoolResultStr executed = graph.Execute( counting );
            vkEndCommandBuffer( cmd );
            ASSERT_TRUE( executed ) << executed.GetError();
            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.commandBufferCount = 1;
            submit.pCommandBuffers    = &cmd;
            SubmitFrame( gpu, backend, submit, VK_NULL_HANDLE );
            vkQueueWaitIdle( gpu.Queue );
            vkFreeCommandBuffers( gpu.Device.device, gpu.CommandPool, 1, &cmd );

            EXPECT_EQ( counting.RenderPassesBegun - before, 1 )
                 << "frame " << frame << ": Clear and Overlay share one";
            EXPECT_EQ( writeBacks, 1 ) << "frame " << frame;
            EXPECT_EQ( recorded, RdgVulkanLayout( target.SubresourceStates.front().Layout ) ) << "frame " << frame;
            EXPECT_EQ( recorded, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ) << "frame " << frame;
            ASSERT_TRUE( readback.Physical && readback.Physical->GetBackendKind() == RDG::BackendKind::Vulkan );
            const auto& buffer = static_cast<const VulkanRdgBuffer&>( *readback.Physical );
            buffer.InvalidateForHost();
            ASSERT_NE( buffer.GetMapped(), nullptr );
            const auto* texel = static_cast<const uint8_t*>( buffer.GetMapped() );
            EXPECT_EQ( std::vector<uint8_t>( texel, texel + 4 ), ( std::vector<uint8_t>{ 0, 255, 0, 255 } ) );
        }
        vkDeviceWaitIdle( gpu.Device.device );
    }
    vmaDestroyImage( gpu.Allocator, image, allocation );
    const std::vector<Message> messages = TakeMessages();
    EXPECT_TRUE( messages.empty() ) << messages.size() << " validation message(s):" << Join( messages );
}

// Every render-graph layout comes back from its Vulkan layout; a layout the graph cannot express is refused.
// A pipeline built against the ENGINE's render pass for a framebuffer (the attachment VulkanFramebuffer gives a
// single-sample colour target, and the SinglePassDependencies every engine pass takes) draws inside the render
// pass the graph opens on that framebuffer: the two are compatible by construction, no
// VUID-vkCmdDraw-renderPass-02684. The draw covers the upper half in the engine's +Y-up convention, so with the
// engine's negative-height viewport it lands in the top rows of the image; the bottom rows keep the clear.
TEST( RenderGraphVulkan, AnEnginePipelineDrawsRightSideUpInARenderPassTheGraphOpens )
{
    Gpu& gpu = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    const VkDevice device = gpu.Device.device;

    VkAttachmentDescription attachment{};
    attachment.format         = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkAttachmentReference colourRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription        subpass{};
    subpass.pipelineBindPoint                           = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount                        = 1;
    subpass.pColorAttachments                           = &colourRef;
    const std::vector<VkSubpassDependency> dependencies = SinglePassDependencies( true, false, false );
    VkRenderPassCreateInfo                 renderPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments    = &attachment;
    renderPassInfo.subpassCount    = 1;
    renderPassInfo.pSubpasses      = &subpass;
    renderPassInfo.dependencyCount = static_cast<uint32_t>( dependencies.size() );
    renderPassInfo.pDependencies   = dependencies.data();
    VkRenderPass engineRenderPass  = VK_NULL_HANDLE;
    ASSERT_EQ( vkCreateRenderPass( device, &renderPassInfo, nullptr, &engineRenderPass ), VK_SUCCESS );

    auto vs = CompileGlsl( R"(#version 450
void main() { vec2 p = vec2( ( gl_VertexIndex << 1 ) & 2, gl_VertexIndex & 2 ); gl_Position = vec4( p.x * 2.0 - 1.0, p.y, 0.0, 1.0 ); })",
                           shaderc_vertex_shader, "upper-half.vert" );
    auto fs = CompileGlsl( R"(#version 450
layout( location = 0 ) out vec4 colour;
void main() { colour = vec4( 1.0, 0.0, 0.0, 1.0 ); })",
                           shaderc_fragment_shader, "red.frag" );
    ASSERT_TRUE( vs && fs ) << ( !vs ? vs.GetError() : fs.GetError() );
    const VkShaderModule                                 vsModule = Module( device, vs.GetValue() );
    const VkShaderModule                                 fsModule = Module( device, fs.GetValue() );
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages   = {
         VkPipelineShaderStageCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                          VK_SHADER_STAGE_VERTEX_BIT, vsModule, "main", nullptr },
         VkPipelineShaderStageCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                          VK_SHADER_STAGE_FRAGMENT_BIT, fsModule, "main", nullptr } };
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout           pipelineLayout = VK_NULL_HANDLE;
    vkCreatePipelineLayout( device, &layoutInfo, nullptr, &pipelineLayout );
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
         VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
    graphics.layout              = pipelineLayout;
    graphics.renderPass          = engineRenderPass;
    VkPipeline pipeline          = VK_NULL_HANDLE;
    ASSERT_EQ( vkCreateGraphicsPipelines( device, VK_NULL_HANDLE, 1, &graphics, nullptr, &pipeline ), VK_SUCCESS );
    vkDestroyShaderModule( device, vsModule, nullptr );
    vkDestroyShaderModule( device, fsModule, nullptr );

    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent        = { kSize, kSize, 1 };
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo memory{};
    memory.usage             = VMA_MEMORY_USAGE_AUTO;
    VkImage       image      = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    ASSERT_EQ( vmaCreateImage( gpu.Allocator, &info, &memory, &image, &allocation, nullptr ), VK_SUCCESS );
    std::vector<uint8_t> pixels;
    {
        VulkanRdgPool        pool( gpu.Rdg, 1 );
        FrameObjects         frameObjects( gpu, 1 );
        VulkanRdgBackend     backend( gpu.Rdg, pool );
        RDG::ExternalTexture target;
        target.Desc = Target();
        target.SubresourceStates.assign( target.Desc.SubresourceCount(),
                                         RDG::RecordedLayoutState( RDG::ImageLayout::Undefined ) );
        target.Physical          = VulkanRdgTexture::Wrap( device, image, VK_FORMAT_R8G8B8A8_UNORM, Target() );
        target.RecordStates      = []( const std::vector<RDG::AccessState>&, bool ) -> Common::BoolResultStr
        { return Common::MakeSuccess( true ); };

        VkCommandBuffer             cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool        = gpu.CommandPool;
        allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        vkAllocateCommandBuffers( device, &allocate, &cmd );
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer( cmd, &begin );
        ASSERT_TRUE( BeginFrameSlot( frameObjects, backend, pool, 0 ) );

        RDG::ExternalBuffer                        readback;
        RDG::Builder                               graph( "engine-pipeline" );
        const std::array<RDG::ExternalTexture*, 1> colors = { &target };
        const RDG::ImportedFramebuffer             fb     = graph.ImportFramebuffer( colors, nullptr, "Scene" );
        const RDG::BufferRef bytes = graph.CreateBuffer( RDG::BufferDesc{ kSize * kSize * 4 }, "Readback" );
        graph.AddPass(
             "Draw", RDG::PassFlags::Raster, [&]( RDG::PassBuilder& pass )
             { pass.ColorTarget( 0, fb.Colors[0], RDG::LoadOp::ClearColor( 0.0f, 1.0f, 0.0f, 1.0f ) ); },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const VkCommandBuffer passCmd = PassCommandBuffer( context );
                 if ( passCmd == VK_NULL_HANDLE )
                     return Fail( "the pass has no recording command buffer" );
                 vkCmdBindPipeline( passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
                 vkCmdDraw( passCmd, 3, 1, 0, 0 );
                 return Common::MakeSuccess( true );
             } );
        graph.AddPass(
             "Readback", RDG::PassFlags::Copy,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( fb.Colors[0], RDG::Access::CopySrc );
                 pass.Write( bytes, RDG::Access::CopyDst );
             },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const VkCommandBuffer passCmd = PassCommandBuffer( context );
                 if ( passCmd == VK_NULL_HANDLE )
                     return Fail( "the pass has no recording command buffer" );
                 auto source = context.GetTexture( fb.Colors[0], RDG::Access::CopySrc );
                 auto dest   = context.GetBuffer( bytes, RDG::Access::CopyDst );
                 if ( !source || !dest )
                     return Fail( !source ? source.GetError() : dest.GetError() );
                 auto sourceImage = VulkanRdgBackend::TextureOf( source.GetValue() );
                 auto buffer      = VulkanRdgBackend::BufferOf( dest.GetValue() );
                 if ( !sourceImage || !buffer )
                     return Fail( "no Vulkan resource" );
                 VkBufferImageCopy region{};
                 region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                 region.imageExtent      = { kSize, kSize, 1 };
                 vkCmdCopyImageToBuffer( passCmd, sourceImage.GetValue()->GetImage(),
                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.GetValue()->GetBuffer(), 1,
                                         &region );
                 return Common::MakeSuccess( true );
             } );
        graph.Extract( bytes, readback, RDG::Access::HostRead );
        const Common::BoolResultStr executed = graph.Execute( backend );
        vkEndCommandBuffer( cmd );
        ASSERT_TRUE( executed ) << executed.GetError();
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;
        SubmitFrame( gpu, backend, submit, VK_NULL_HANDLE );
        vkQueueWaitIdle( gpu.Queue );
        vkFreeCommandBuffers( device, gpu.CommandPool, 1, &cmd );
        ASSERT_TRUE( readback.Physical && readback.Physical->GetBackendKind() == RDG::BackendKind::Vulkan );
        const auto& buffer = static_cast<const VulkanRdgBuffer&>( *readback.Physical );
        buffer.InvalidateForHost();
        ASSERT_NE( buffer.GetMapped(), nullptr );
        const auto* texel = static_cast<const uint8_t*>( buffer.GetMapped() );
        pixels.assign( texel, texel + kSize * kSize * 4 );
        vkDeviceWaitIdle( device );
    }
    vmaDestroyImage( gpu.Allocator, image, allocation );
    vkDestroyPipeline( device, pipeline, nullptr );
    vkDestroyPipelineLayout( device, pipelineLayout, nullptr );
    vkDestroyRenderPass( device, engineRenderPass, nullptr );

    const std::vector<Message> messages = TakeMessages();
    EXPECT_TRUE( messages.empty() ) << messages.size() << " validation message(s):" << Join( messages );
    ASSERT_EQ( pixels.size(), static_cast<size_t>( kSize * kSize * 4 ) );
    const auto row = [&]( uint32_t y )
    { return std::vector<uint8_t>( pixels.begin() + y * kSize * 4, pixels.begin() + y * kSize * 4 + 4 ); };
    EXPECT_EQ( row( 0 ), ( std::vector<uint8_t>{ 255, 0, 0, 255 } ) ) << "the upper half is drawn in the top row";
    EXPECT_EQ( row( kSize - 1 ), ( std::vector<uint8_t>{ 0, 255, 0, 255 } ) ) << "the bottom row keeps the clear";
}

// An MSAA framebuffer imported into the graph: the render pass the graph opens declares the multisampled colour AND
// the single-sample image it resolves into (PassBuilder::ResolveTarget), built through the same
// CreateSinglePassRenderPass as the engine's MSAA framebuffer (VulkanFramebuffer: 4x colour, finalLayout
// COLOR_ATTACHMENT, plus a 1x resolve). A pipeline built against the ENGINE's MSAA render pass draws in it with no
// validation message, and the resolve image receives the picture. Without the resolve in the graph's pass the
// two render passes are incompatible (VUID-vkCmdDraw-renderPass-02684) and the resolve image is never written.
TEST( RenderGraphVulkan, AnEnginePipelineDrawsIntoAnImportedMsaaFramebufferAndTheGraphResolvesIt )
{
    Gpu& gpu = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    const VkDevice                  device  = gpu.Device.device;
    constexpr VkSampleCountFlagBits kSamples = VK_SAMPLE_COUNT_4_BIT;

    // The engine's MSAA framebuffer render pass, described as VulkanFramebuffer::RT_Invalidate describes it.
    std::array<VkAttachmentDescription, 2> engineAttachments{};
    engineAttachments[0].format         = VK_FORMAT_R8G8B8A8_UNORM;
    engineAttachments[0].samples        = kSamples;
    engineAttachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    engineAttachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    engineAttachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    engineAttachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    engineAttachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    engineAttachments[0].finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    engineAttachments[1]                = engineAttachments[0];
    engineAttachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    engineAttachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    engineAttachments[1].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const std::array<VkAttachmentReference, 1> colourRefs  = { VkAttachmentReference{
         0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
    const std::array<VkAttachmentReference, 1> resolveRefs = { VkAttachmentReference{
         1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
    VkRenderPass engineRenderPass = VK_NULL_HANDLE;
    ASSERT_EQ( CreateSinglePassRenderPass( device, engineAttachments, colourRefs, resolveRefs, nullptr, false,
                                           engineRenderPass ),
               VK_SUCCESS );

    auto vs = CompileGlsl( R"(#version 450
void main() { vec2 p = vec2( ( gl_VertexIndex << 1 ) & 2, gl_VertexIndex & 2 ); gl_Position = vec4( p.x * 2.0 - 1.0, p.y, 0.0, 1.0 ); })",
                           shaderc_vertex_shader, "upper-half-msaa.vert" );
    auto fs = CompileGlsl( R"(#version 450
layout( location = 0 ) out vec4 colour;
void main() { colour = vec4( 1.0, 0.0, 0.0, 1.0 ); })",
                           shaderc_fragment_shader, "red-msaa.frag" );
    ASSERT_TRUE( vs && fs ) << ( !vs ? vs.GetError() : fs.GetError() );
    const VkShaderModule                                 vsModule = Module( device, vs.GetValue() );
    const VkShaderModule                                 fsModule = Module( device, fs.GetValue() );
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages   = {
         VkPipelineShaderStageCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                          VK_SHADER_STAGE_VERTEX_BIT, vsModule, "main", nullptr },
         VkPipelineShaderStageCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                          VK_SHADER_STAGE_FRAGMENT_BIT, fsModule, "main", nullptr } };
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout           pipelineLayout = VK_NULL_HANDLE;
    vkCreatePipelineLayout( device, &layoutInfo, nullptr, &pipelineLayout );
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = kSamples;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
         VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
    graphics.layout              = pipelineLayout;
    graphics.renderPass          = engineRenderPass;
    VkPipeline pipeline          = VK_NULL_HANDLE;
    ASSERT_EQ( vkCreateGraphicsPipelines( device, VK_NULL_HANDLE, 1, &graphics, nullptr, &pipeline ), VK_SUCCESS );
    vkDestroyShaderModule( device, vsModule, nullptr );
    vkDestroyShaderModule( device, fsModule, nullptr );

    // The two images of the engine framebuffer: the 4x colour it renders into, the 1x image it resolves into.
    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent        = { kSize, kSize, 1 };
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.samples       = kSamples;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo memory{};
    memory.usage                    = VMA_MEMORY_USAGE_AUTO;
    VkImage       msaaImage         = VK_NULL_HANDLE;
    VmaAllocation msaaAllocation    = nullptr;
    VkImage       resolveImage      = VK_NULL_HANDLE;
    VmaAllocation resolveAllocation = nullptr;
    ASSERT_EQ( vmaCreateImage( gpu.Allocator, &info, &memory, &msaaImage, &msaaAllocation, nullptr ), VK_SUCCESS );
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage   = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ASSERT_EQ( vmaCreateImage( gpu.Allocator, &info, &memory, &resolveImage, &resolveAllocation, nullptr ),
               VK_SUCCESS );
    std::vector<uint8_t> pixels;
    {
        VulkanRdgPool    pool( gpu.Rdg, 1 );
        FrameObjects     frameObjects( gpu, 1 );
        VulkanRdgBackend backend( gpu.Rdg, pool );
        RDG::TextureDesc msaaDesc = Target();
        msaaDesc.Samples          = 4;
        auto importOf             = [&]( VkImage image, const RDG::TextureDesc& desc )
        {
            RDG::ExternalTexture external;
            external.Desc = desc;
            external.SubresourceStates.assign( desc.SubresourceCount(),
                                               RDG::RecordedLayoutState( RDG::ImageLayout::Undefined ) );
            external.Physical          = VulkanRdgTexture::Wrap( device, image, VK_FORMAT_R8G8B8A8_UNORM, desc );
            external.RecordStates      = []( const std::vector<RDG::AccessState>&, bool ) -> Common::BoolResultStr
            { return Common::MakeSuccess( true ); };
            return external;
        };
        RDG::ExternalTexture msaa    = importOf( msaaImage, msaaDesc );
        RDG::ExternalTexture resolve = importOf( resolveImage, Target() );

        VkCommandBuffer             cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool        = gpu.CommandPool;
        allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        vkAllocateCommandBuffers( device, &allocate, &cmd );
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer( cmd, &begin );
        ASSERT_TRUE( BeginFrameSlot( frameObjects, backend, pool, 0 ) );

        RDG::ExternalBuffer                        readback;
        RDG::Builder                               graph( "engine-msaa-pipeline" );
        const std::array<RDG::ExternalTexture*, 1> colors   = { &msaa };
        const std::array<RDG::ExternalTexture*, 1> resolves = { &resolve };
        const RDG::ImportedFramebuffer fb = graph.ImportFramebuffer( colors, nullptr, "SceneMsaa", resolves );
        ASSERT_EQ( fb.Resolves.size(), 1u );
        const RDG::BufferRef bytes = graph.CreateBuffer( RDG::BufferDesc{ kSize * kSize * 4 }, "Readback" );
        graph.AddPass(
             "Draw", RDG::PassFlags::Raster,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.ColorTarget( 0, fb.Colors[0], RDG::LoadOp::ClearColor( 0.0f, 1.0f, 0.0f, 1.0f ) );
                 pass.ResolveTarget( 0, fb.Resolves[0] );
             },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const VkCommandBuffer passCmd = PassCommandBuffer( context );
                 if ( passCmd == VK_NULL_HANDLE )
                     return Fail( "the pass has no recording command buffer" );
                 vkCmdBindPipeline( passCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
                 vkCmdDraw( passCmd, 3, 1, 0, 0 );
                 return Common::MakeSuccess( true );
             } );
        graph.AddPass(
             "Readback", RDG::PassFlags::Copy,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( fb.Resolves[0], RDG::Access::CopySrc );
                 pass.Write( bytes, RDG::Access::CopyDst );
             },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const VkCommandBuffer passCmd = PassCommandBuffer( context );
                 if ( passCmd == VK_NULL_HANDLE )
                     return Fail( "the pass has no recording command buffer" );
                 auto source = context.GetTexture( fb.Resolves[0], RDG::Access::CopySrc );
                 auto dest   = context.GetBuffer( bytes, RDG::Access::CopyDst );
                 if ( !source || !dest )
                     return Fail( !source ? source.GetError() : dest.GetError() );
                 auto sourceImage = VulkanRdgBackend::TextureOf( source.GetValue() );
                 auto buffer      = VulkanRdgBackend::BufferOf( dest.GetValue() );
                 if ( !sourceImage || !buffer )
                     return Fail( "no Vulkan resource" );
                 VkBufferImageCopy region{};
                 region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                 region.imageExtent      = { kSize, kSize, 1 };
                 vkCmdCopyImageToBuffer( passCmd, sourceImage.GetValue()->GetImage(),
                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.GetValue()->GetBuffer(), 1,
                                         &region );
                 return Common::MakeSuccess( true );
             } );
        graph.Extract( bytes, readback, RDG::Access::HostRead );
        const Common::BoolResultStr executed = graph.Execute( backend );
        vkEndCommandBuffer( cmd );
        ASSERT_TRUE( executed ) << executed.GetError();
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;
        SubmitFrame( gpu, backend, submit, VK_NULL_HANDLE );
        vkQueueWaitIdle( gpu.Queue );
        vkFreeCommandBuffers( device, gpu.CommandPool, 1, &cmd );
        ASSERT_TRUE( readback.Physical && readback.Physical->GetBackendKind() == RDG::BackendKind::Vulkan );
        const auto& buffer = static_cast<const VulkanRdgBuffer&>( *readback.Physical );
        buffer.InvalidateForHost();
        ASSERT_NE( buffer.GetMapped(), nullptr );
        const auto* texel = static_cast<const uint8_t*>( buffer.GetMapped() );
        pixels.assign( texel, texel + kSize * kSize * 4 );
        vkDeviceWaitIdle( device );
    }
    vmaDestroyImage( gpu.Allocator, msaaImage, msaaAllocation );
    vmaDestroyImage( gpu.Allocator, resolveImage, resolveAllocation );
    vkDestroyPipeline( device, pipeline, nullptr );
    vkDestroyPipelineLayout( device, pipelineLayout, nullptr );
    vkDestroyRenderPass( device, engineRenderPass, nullptr );

    const std::vector<Message> messages = TakeMessages();
    EXPECT_TRUE( messages.empty() ) << messages.size() << " validation message(s):" << Join( messages );
    ASSERT_EQ( pixels.size(), static_cast<size_t>( kSize * kSize * 4 ) );
    const auto row = [&]( uint32_t y )
    { return std::vector<uint8_t>( pixels.begin() + y * kSize * 4, pixels.begin() + y * kSize * 4 + 4 ); };
    EXPECT_EQ( row( 0 ), ( std::vector<uint8_t>{ 255, 0, 0, 255 } ) ) << "the resolve holds the drawn upper half";
    EXPECT_EQ( row( kSize - 1 ), ( std::vector<uint8_t>{ 0, 255, 0, 255 } ) ) << "the resolve holds the clear";
}

TEST( RenderGraphVulkan, EveryGraphLayoutRoundTripsThroughItsVulkanLayout )
{
    for ( const RDG::ImageLayout layout :
          { RDG::ImageLayout::Undefined, RDG::ImageLayout::General, RDG::ImageLayout::ColorAttachment,
            RDG::ImageLayout::DepthStencilAttachment, RDG::ImageLayout::DepthStencilReadOnly,
            RDG::ImageLayout::ShaderReadOnly, RDG::ImageLayout::TransferSrc, RDG::ImageLayout::TransferDst,
            RDG::ImageLayout::Present } )
        EXPECT_EQ( RdgLayoutFromVulkan( RdgVulkanLayout( layout ) ), std::optional<RDG::ImageLayout>( layout ) )
             << static_cast<int>( layout );
    EXPECT_FALSE( RdgLayoutFromVulkan( VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL ).has_value() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// Renderer::ImportImage of a VulkanImage3D and Renderer::ImportBuffer of a persistent StorageBuffer, on the
// device: a volume and a buffer the graph does not own (VulkanRdgTexture::Wrap / VulkanRdgBuffer::Wrap), a
// compute pass that writes both, then a copy that reads the volume into the buffer. Two frames: the second
// starts from the states the first wrote back, so its compute write waits on last frame's copy. Validation
// (synchronization validation included) must stay silent, and the buffer holds the volume's texels.
TEST( RenderGraphVulkan, AnImportedVolumeAndStorageBufferAreWrittenByComputeAndReadBackValidationClean )
{
    constexpr uint32_t kEdge  = 4;
    constexpr uint32_t kBytes = kEdge * kEdge * kEdge * 4;
    Gpu&               gpu    = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    const VkDevice device = gpu.Device.device;

    constexpr const char* kWrite = R"(#version 450
layout( local_size_x = 4, local_size_y = 4, local_size_z = 4 ) in;
layout( set = 0, binding = 0, rgba8 ) uniform writeonly image3D volume;
layout( set = 0, binding = 1 ) buffer Particles { uint Values[]; };
void main()
{
    ivec3 p = ivec3( gl_GlobalInvocationID );
    imageStore( volume, p, vec4( 1.0, 0.0, float( p.z ) / 255.0, 1.0 ) );
    Values[p.x + 4 * p.y + 16 * p.z] = 7u;
})";
    auto                  code   = CompileGlsl( kWrite, shaderc_compute_shader, "volume.comp" );
    ASSERT_TRUE( code ) << code.GetError();

    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType     = VK_IMAGE_TYPE_3D;
    info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent        = { kEdge, kEdge, kEdge };
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo imageMemory{};
    imageMemory.usage        = VMA_MEMORY_USAGE_AUTO;
    VkImage       image      = VK_NULL_HANDLE;
    VmaAllocation imageAlloc = nullptr;
    ASSERT_EQ( vmaCreateImage( gpu.Allocator, &info, &imageMemory, &image, &imageAlloc, nullptr ), VK_SUCCESS );

    // The engine's persistent storage buffer: host-visible, mapped, storage + transfer destination.
    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size  = kBytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo bufferMemory{};
    bufferMemory.usage       = VMA_MEMORY_USAGE_AUTO;
    bufferMemory.flags       = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer          buffer = VK_NULL_HANDLE;
    VmaAllocation     bufferAlloc = nullptr;
    VmaAllocationInfo mapped{};
    ASSERT_EQ( vmaCreateBuffer( gpu.Allocator, &bufferInfo, &bufferMemory, &buffer, &bufferAlloc, &mapped ),
               VK_SUCCESS );

    RDG::TextureDesc volumeDesc = Target();
    volumeDesc.Size             = { kEdge, kEdge, kEdge };
    volumeDesc.Dim              = RDG::TextureDim::Tex3D;
    volumeDesc.Mips             = 1;
    volumeDesc.Layers           = 1;
    std::shared_ptr<VulkanRdgTexture> wrappedImage =
         VulkanRdgTexture::Wrap( device, image, VK_FORMAT_R8G8B8A8_UNORM, volumeDesc );
    const std::shared_ptr<VulkanRdgBuffer> wrappedBuffer = VulkanRdgBuffer::Wrap( buffer, kBytes );
    const Common::ResultStr<VkImageView>   view =
         wrappedImage->GetView( RDG::SubresourceRange{ 0, 1, 0, 1 }, false );
    ASSERT_TRUE( view ) << view.GetError();

    const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {
         VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                       nullptr },
         VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                       nullptr } };
    VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount         = static_cast<uint32_t>( bindings.size() );
    layoutInfo.pBindings            = bindings.data();
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    ASSERT_EQ( vkCreateDescriptorSetLayout( device, &layoutInfo, nullptr, &setLayout ), VK_SUCCESS );
    VkPipelineLayoutCreateInfo pipeLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipeLayoutInfo.setLayoutCount = 1;
    pipeLayoutInfo.pSetLayouts    = &setLayout;
    VkPipelineLayout pipeLayout   = VK_NULL_HANDLE;
    ASSERT_EQ( vkCreatePipelineLayout( device, &pipeLayoutInfo, nullptr, &pipeLayout ), VK_SUCCESS );
    const VkShaderModule        module_ = Module( device, code.GetValue() );
    VkComputePipelineCreateInfo pipeInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipeInfo.stage        = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    pipeInfo.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeInfo.stage.module = module_;
    pipeInfo.stage.pName  = "main";
    pipeInfo.layout       = pipeLayout;
    VkPipeline pipeline   = VK_NULL_HANDLE;
    ASSERT_EQ( vkCreateComputePipelines( device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipeline ), VK_SUCCESS );
    const std::array<VkDescriptorPoolSize, 2> sizes = {
         VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
         VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets             = 1;
    poolInfo.poolSizeCount       = static_cast<uint32_t>( sizes.size() );
    poolInfo.pPoolSizes          = sizes.data();
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    ASSERT_EQ( vkCreateDescriptorPool( device, &poolInfo, nullptr, &descriptors ), VK_SUCCESS );
    VkDescriptorSetAllocateInfo setInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    setInfo.descriptorPool     = descriptors;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts        = &setLayout;
    VkDescriptorSet set        = VK_NULL_HANDLE;
    ASSERT_EQ( vkAllocateDescriptorSets( device, &setInfo, &set ), VK_SUCCESS );
    const VkDescriptorImageInfo  imageDescriptor{ VK_NULL_HANDLE, view.GetValue(), VK_IMAGE_LAYOUT_GENERAL };
    const VkDescriptorBufferInfo bufferDescriptor{ buffer, 0, kBytes };
    std::array<VkWriteDescriptorSet, 2> writes{};
    for ( VkWriteDescriptorSet& write : writes )
    {
        write                 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = set;
        write.descriptorCount = 1;
    }
    writes[0].dstBinding     = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo     = &imageDescriptor;
    writes[1].dstBinding     = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo    = &bufferDescriptor;
    vkUpdateDescriptorSets( device, static_cast<uint32_t>( writes.size() ), writes.data(), 0, nullptr );

    // The engine images' and buffers' own records, as VulkanImage3D and VulkanStorageBuffer keep them.
    std::vector<RDG::AccessState> imageStates( 1, RDG::GetAccessState( RDG::Access::None ) );
    RDG::AccessState              bufferState = RDG::GetAccessState( RDG::Access::None );
    {
        VulkanRdgPool    pool( gpu.Rdg, 1 );
        FrameObjects     frameObjects( gpu, 1 );
        VulkanRdgBackend backend( gpu.Rdg, pool );
        for ( int frame = 0; frame < 2; ++frame )
        {
            RDG::ExternalTexture volume;
            volume.Desc              = volumeDesc;
            volume.SubresourceStates = imageStates;
            volume.Physical          = wrappedImage;
            volume.RecordStates = [&]( const std::vector<RDG::AccessState>& states, bool ) -> Common::BoolResultStr
            {
                imageStates = states;
                return Common::MakeSuccess( true );
            };
            RDG::ExternalBuffer particles( RDG::BufferDesc{ kBytes }, RDG::Access::None );
            particles.State            = bufferState;
            particles.Physical         = wrappedBuffer;
            int bufferWriteBacks       = 0;
            particles.RecordFinalState = [&]( const RDG::AccessState& state ) -> Common::BoolResultStr
            {
                ++bufferWriteBacks;
                bufferState = state;
                return Common::MakeSuccess( true );
            };

            RDG::Builder          graph( "volume" );
            const RDG::TextureRef volumeRef = graph.RegisterExternal( volume, "Volume" );
            const RDG::BufferRef  bufferRef = graph.RegisterExternal( particles, "Particles" );
            graph.AddPass(
                 "Simulate", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Write( volumeRef, RDG::Access::StorageWrite );
                     pass.Write( bufferRef, RDG::Access::StorageWrite );
                 },
                 [&]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     const Common::ResultStr<VkCommandBuffer> cmd = VulkanRdgBackend::CommandBufferOf( context );
                     if ( !cmd )
                         return Common::MakeError( cmd.GetError() );
                     vkCmdBindPipeline( cmd.GetValue(), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline );
                     vkCmdBindDescriptorSets( cmd.GetValue(), VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1,
                                              &set, 0, nullptr );
                     vkCmdDispatch( cmd.GetValue(), 1, 1, 1 );
                     return Common::MakeSuccess( true );
                 } );
            graph.AddPass(
                 "Gather", RDG::PassFlags::Copy,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Read( volumeRef, RDG::Access::CopySrc );
                     pass.Write( bufferRef, RDG::Access::CopyDst );
                 },
                 [&]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     const Common::ResultStr<VkCommandBuffer> cmd = VulkanRdgBackend::CommandBufferOf( context );
                     if ( !cmd )
                         return Common::MakeError( cmd.GetError() );
                     VkBufferImageCopy region{};
                     region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                     region.imageExtent      = { kEdge, kEdge, kEdge };
                     vkCmdCopyImageToBuffer( cmd.GetValue(), image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
                                             1, &region );
                     return Common::MakeSuccess( true );
                 } );

            VkCommandBuffer             cmd = VK_NULL_HANDLE;
            VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocate.commandPool        = gpu.CommandPool;
            allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            vkAllocateCommandBuffers( device, &allocate, &cmd );
            VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer( cmd, &begin );
            ASSERT_TRUE( BeginFrameSlot( frameObjects, backend, pool, 0 ) );
            const Common::BoolResultStr executed = graph.Execute( backend );
            vkEndCommandBuffer( cmd );
            ASSERT_TRUE( executed ) << executed.GetError();
            VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.commandBufferCount = 1;
            submit.pCommandBuffers    = &cmd;
            SubmitFrame( gpu, backend, submit, VK_NULL_HANDLE );
            vkQueueWaitIdle( gpu.Queue );
            vkFreeCommandBuffers( device, gpu.CommandPool, 1, &cmd );

            EXPECT_EQ( bufferWriteBacks, 1 ) << "frame " << frame;
            EXPECT_EQ( bufferState, RDG::GetAccessState( RDG::Access::CopyDst ) ) << "frame " << frame;
            EXPECT_EQ( imageStates.front(), RDG::GetAccessState( RDG::Access::CopySrc ) ) << "frame " << frame;
            vmaInvalidateAllocation( gpu.Allocator, bufferAlloc, 0, VK_WHOLE_SIZE );
            const auto* texel = static_cast<const uint8_t*>( mapped.pMappedData );
            ASSERT_NE( texel, nullptr );
            // The copy overwrote the compute's 7u with the volume's texels: red, blue = slice, alpha.
            EXPECT_EQ( std::vector<uint8_t>( texel, texel + 4 ), ( std::vector<uint8_t>{ 255, 0, 0, 255 } ) )
                 << "frame " << frame;
            const uint8_t* last = texel + kBytes - 4;
            EXPECT_EQ( std::vector<uint8_t>( last, last + 4 ), ( std::vector<uint8_t>{ 255, 0, kEdge - 1, 255 } ) )
                 << "frame " << frame;
        }
        vkDeviceWaitIdle( device );
    }
    vkDestroyDescriptorPool( device, descriptors, nullptr );
    vkDestroyPipeline( device, pipeline, nullptr );
    vkDestroyShaderModule( device, module_, nullptr );
    vkDestroyPipelineLayout( device, pipeLayout, nullptr );
    vkDestroyDescriptorSetLayout( device, setLayout, nullptr );
    // The graph's handles own nothing: the engine's owners release the image and the buffer. The wrapped
    // texture destroys only the view it made, so it goes first.
    wrappedImage.reset();
    vmaDestroyImage( gpu.Allocator, image, imageAlloc );
    vmaDestroyBuffer( gpu.Allocator, buffer, bufferAlloc );
    const std::vector<Message> messages = TakeMessages();
    EXPECT_TRUE( messages.empty() ) << messages.size() << " validation message(s):" << Join( messages );
}

// A RESIZED IMPORTED IMAGE'S GRAPH HANDLE OUTLIVES THE FRAMES THAT RECORDED IT (RDG-TAILS 5).
// VulkanImage2D::Release used to reset its graph handle at once, destroying views a frame in flight still
// referenced. Now the handle is retired to the pool and released at the next BeginFrame of the slot it was retired
// in. Frame 0 (slot 0) binds the old image's view and is submitted WITHOUT waiting; the image is "resized" (handle
// retired, new image wrapped); frame 1 (slot 1) uses the new one; only after waiting for frame 0 does
// BeginFrame(0) release the old view. Destroying it earlier is VUID-vkDestroyImageView-imageView-01026: validation
// must stay silent.
TEST( RenderGraphVulkan, AResizedImportedImageKeepsItsViewsUntilTheFramesUsingThemComplete )
{
    Gpu& gpu = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    const VkDevice device = gpu.Device.device;
    VulkanRdgPool  pool( gpu.Rdg, 2 );
    FrameObjects   frameObjects( gpu, 2 );

    const auto makeImage = [&]( uint32_t edge, VkImage& image, VmaAllocation& memory )
    {
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent        = { edge, edge, 1 };
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        return vmaCreateImage( gpu.Allocator, &info, &allocation, &image, &memory, nullptr );
    };
    const auto wrap = [&]( VkImage image, uint32_t edge )
    {
        RDG::TextureDesc desc = Target();
        desc.Size             = { edge, edge, 1 };
        desc.Mips             = 1;
        desc.Layers           = 1;
        return VulkanRdgTexture::Wrap( device, image, VK_FORMAT_R8G8B8A8_UNORM, desc );
    };
    // One frame: clear the imported image through the graph, submit, return the fence (not waited).
    const auto frame = [&]( uint32_t slot, const std::shared_ptr<VulkanRdgTexture>& texture, VkImage image,
                            VkCommandBuffer& cmd, VkFence& fence )
    {
        VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool        = gpu.CommandPool;
        allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        vkAllocateCommandBuffers( device, &allocate, &cmd );
        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence( device, &fenceInfo, nullptr, &fence );
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer( cmd, &begin );
        VulkanRdgBackend backend( gpu.Rdg, pool );
        const Common::BoolResultStr begun = BeginFrameSlot( frameObjects, backend, pool, slot );
        EXPECT_TRUE( begun ) << begun.GetError();
        RDG::ExternalTexture external( texture->GetDesc(), RDG::Access::None );
        external.Physical = texture;
        RDG::Builder          graph( "resize" );
        const RDG::TextureRef target = graph.RegisterExternal( external, "Resized" );
        graph.AddPass(
             "Clear", RDG::PassFlags::Copy | RDG::PassFlags::NeverCull,
             [&]( RDG::PassBuilder& pass ) { pass.Write( target, RDG::Access::CopyDst ); },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const Common::ResultStr<VkCommandBuffer> recording = VulkanRdgBackend::CommandBufferOf( context );
                 if ( !recording )
                     return Common::MakeError( recording.GetError() );
                 // The view is what a descriptor would name: made here, so the retired handle owns it.
                 const Common::ResultStr<VkImageView> view =
                      texture->GetView( RDG::SubresourceRange{ 0, 1, 0, 1 }, false );
                 if ( !view )
                     return Common::MakeError( view.GetError() );
                 const VkClearColorValue       colour{ { 0.0f, 1.0f, 0.0f, 1.0f } };
                 const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                 vkCmdClearColorImage( recording.GetValue(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour,
                                       1, &range );
                 return Common::MakeSuccess( true );
             } );
        const Common::BoolResultStr executed = graph.Execute( backend );
        vkEndCommandBuffer( cmd );
        EXPECT_TRUE( executed ) << executed.GetError();
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;
        SubmitFrame( gpu, backend, submit, fence );
    };

    VkImage       oldImage  = VK_NULL_HANDLE;
    VkImage       newImage  = VK_NULL_HANDLE;
    VmaAllocation oldMemory = nullptr;
    VmaAllocation newMemory = nullptr;
    ASSERT_EQ( makeImage( 16, oldImage, oldMemory ), VK_SUCCESS );
    ASSERT_EQ( makeImage( 32, newImage, newMemory ), VK_SUCCESS );

    std::shared_ptr<VulkanRdgTexture>     handle    = wrap( oldImage, 16 );
    const std::weak_ptr<VulkanRdgTexture> oldHandle = handle;
    std::array<VkCommandBuffer, 2>        cmds{};
    std::array<VkFence, 2>                fences{};
    frame( 0, handle, oldImage, cmds[0], fences[0] );

    // The resize, while frame 0 may still be executing: IVulkanImage::DropGraphTexture's path.
    pool.Retire( std::move( handle ) );
    EXPECT_FALSE( oldHandle.expired() ) << "retired handle released while its frame may be in flight";
    std::shared_ptr<VulkanRdgTexture> resized = wrap( newImage, 32 );
    frame( 1, resized, newImage, cmds[1], fences[1] );
    EXPECT_FALSE( oldHandle.expired() ) << "another slot's BeginFrame released the handle";

    // The renderer waits for slot 0's previous frame before BeginFrame(0): now the old views may go.
    vkWaitForFences( device, 1, &fences[0], VK_TRUE, UINT64_MAX );
    pool.BeginFrame( 0 );
    EXPECT_TRUE( oldHandle.expired() ) << "the retired handle outlived the frame slot's next BeginFrame";

    vkDeviceWaitIdle( device );
    for ( size_t i = 0; i < cmds.size(); ++i )
    {
        vkFreeCommandBuffers( device, gpu.CommandPool, 1, &cmds[i] );
        vkDestroyFence( device, fences[i], nullptr );
    }
    resized.reset();
    vmaDestroyImage( gpu.Allocator, oldImage, oldMemory );
    vmaDestroyImage( gpu.Allocator, newImage, newMemory );
    const std::vector<Message> messages = TakeMessages();
    EXPECT_TRUE( messages.empty() ) << messages.size() << " validation message(s):" << Join( messages );
}

// Tail 6 (RDG-TAILS): a sampled compute input bound inside a graph node names exactly the subresource the node
// declared. SampledSubresourceView is what VulkanPipelineCompute::SetInput( binding, image, access, range ) binds:
// All -> the image's own view, Mip -> its own mip view, a (mip, layer) rectangle -> the graph handle's 2D view of
// that one layer; a layer range the graph cannot address is refused, not widened to the whole image.
TEST( RenderGraphVulkan, ASampledInputViewAddressesExactlyTheMipAndLayerTheNodeDeclared )
{
    Gpu& gpu = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    const VkDevice device = gpu.Device.device;

    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent        = { 8, 8, 1 };
    info.mipLevels     = 2;
    info.arrayLayers   = 2;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage     = VMA_MEMORY_USAGE_AUTO;
    VkImage       image  = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    ASSERT_EQ( vmaCreateImage( gpu.Allocator, &info, &allocation, &image, &memory, nullptr ), VK_SUCCESS );
    {
        RDG::TextureDesc desc = Target();
        desc.Size             = { 8, 8, 1 };
        desc.Mips             = 2;
        desc.Layers           = 2;
        const auto texture    = VulkanRdgTexture::Wrap( device, image, VK_FORMAT_R8G8B8A8_UNORM, desc );

        // Stand-ins for the engine image's own whole / mip-1 views.
        const auto wholeView = texture->GetView();
        const auto mip1View  = texture->GetView( RDG::SubresourceRange::Mip( 1 ) );
        ASSERT_TRUE( wholeView.IsSuccess() && mip1View.IsSuccess() );
        const VkImageView whole = wholeView.GetValue();
        const VkImageView mip1  = mip1View.GetValue();
        const auto mipView      = [&]( uint32_t mip ) { return mip == 1 ? mip1 : VkImageView( VK_NULL_HANDLE ); };

        const auto all = SampledSubresourceView( RDG::SubresourceRange::All(), whole, mipView, texture.get() );
        ASSERT_TRUE( all.IsSuccess() ) << all.GetError();
        EXPECT_EQ( all.GetValue(), whole );
        const auto oneMip =
             SampledSubresourceView( RDG::SubresourceRange::Mip( 1 ), whole, mipView, texture.get() );
        ASSERT_TRUE( oneMip.IsSuccess() ) << oneMip.GetError();
        EXPECT_EQ( oneMip.GetValue(), mip1 );

        const auto layer1 =
             SampledSubresourceView( RDG::SubresourceRange::MipLayer( 1, 1 ), whole, mipView, texture.get() );
        ASSERT_TRUE( layer1.IsSuccess() ) << layer1.GetError();
        const auto expected = texture->GetView( RDG::SubresourceRange::MipLayer( 1, 1 ) );
        const auto layer0   = texture->GetView( RDG::SubresourceRange::MipLayer( 1, 0 ) );
        ASSERT_TRUE( expected.IsSuccess() && layer0.IsSuccess() );
        EXPECT_EQ( layer1.GetValue(), expected.GetValue() );
        EXPECT_NE( layer1.GetValue(), layer0.GetValue() ) << "layer 1 must not bind layer 0's view";
        EXPECT_NE( layer1.GetValue(), mip1 ) << "one layer must not bind the view of every layer";

        EXPECT_FALSE( SampledSubresourceView( RDG::SubresourceRange::MipLayer( 0, 1 ), whole, mipView, nullptr )
                           .IsSuccess() )
             << "a layer of an image the graph did not import has no view to bind";
        EXPECT_FALSE(
             SampledSubresourceView( RDG::SubresourceRange::MipLayer( 0, 2 ), whole, mipView, texture.get() )
                  .IsSuccess() )
             << "a layer outside the image is refused";
        EXPECT_FALSE(
             SampledSubresourceView( RDG::SubresourceRange::Mip( 0 ), whole, mipView, texture.get() ).IsSuccess() )
             << "a mip the image has no view of is refused";
    }
    vmaDestroyImage( gpu.Allocator, image, memory );
}

namespace
{
    // One frame of the aliasing graph: First is cleared and copied out, then Second (the same description, a later
    // lifetime) is cleared and copied out - so the plan may give Second First's bytes. Recorded on @p slot and
    // submitted with a fence; FinishAliasFrame waits for it.
    struct AliasFrame
    {
        std::string            Error;
        VkCommandBuffer        CommandBuffer = VK_NULL_HANDLE;
        VkFence                Fence         = VK_NULL_HANDLE;
        RDG::ExternalBuffer    FirstBytes;
        RDG::ExternalBuffer    SecondBytes;
        std::array<VkImage, 2> Images{}; // First, Second as their clear passes saw them
        uint32_t               Size = 0;
    };

    constexpr std::array<uint8_t, 4> kFirstTexel  = { 255, 0, 0, 255 };
    constexpr std::array<uint8_t, 4> kSecondTexel = { 0, 0, 255, 255 };

    void RecordAliasFrame( Gpu& gpu, VulkanRdgBackend& vulkan, WeakenedBarriers& witness, VulkanRdgPool& pool,
                           FrameObjects& frame, uint32_t slot, uint32_t size, AliasFrame& out )
    {
        out.Size = size;
        VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool        = gpu.CommandPool;
        allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        vkAllocateCommandBuffers( gpu.Device.device, &allocate, &out.CommandBuffer );
        const VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence( gpu.Device.device, &fenceInfo, nullptr, &out.Fence );
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer( out.CommandBuffer, &begin );
        if ( const Common::BoolResultStr begun = BeginFrameSlot( frame, vulkan, pool, slot ); !begun )
        {
            out.Error = begun.GetError();
            vkEndCommandBuffer( out.CommandBuffer );
            return;
        }

        RDG::TextureDesc desc;
        desc.Size   = { size, size, 1 };
        desc.Format = ImageFormat::RGBA8F;
        const RDG::BufferDesc                bytes{ uint64_t( size ) * size * 4 };
        RDG::Builder                         graph( "alias-first-second" );
        const std::array<RDG::TextureRef, 2> textures  = { graph.CreateTexture( desc, "First" ),
                                                           graph.CreateTexture( desc, "Second" ) };
        const std::array<RDG::BufferRef, 2>  readbacks = { graph.CreateBuffer( bytes, "FirstBytes" ),
                                                           graph.CreateBuffer( bytes, "SecondBytes" ) };
        const VkCommandBuffer                cmd       = out.CommandBuffer;
        for ( uint32_t i = 0; i < 2; ++i )
        {
            graph.AddPass(
                 i == 0 ? "ClearFirst" : "ClearSecond", RDG::PassFlags::Raster,
                 [&textures, i]( RDG::PassBuilder& pass )
                 {
                     const std::array<uint8_t, 4>& texel = i == 0 ? kFirstTexel : kSecondTexel;
                     pass.ColorTarget( 0, textures[i],
                                       RDG::LoadOp::ClearColor( texel[0] / 255.0f, texel[1] / 255.0f,
                                                                texel[2] / 255.0f, texel[3] / 255.0f ) );
                 },
                 [&textures, &out, i]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     auto texture = context.GetTexture( textures[i], RDG::Access::ColorTarget );
                     if ( !texture )
                         return Fail( texture.GetError() );
                     auto image = VulkanRdgBackend::TextureOf( texture.GetValue() );
                     if ( !image )
                         return Fail( image.GetError() );
                     out.Images[i] = image.GetValue()->GetImage();
                     return Common::MakeSuccess( true ); // the render pass's load op is the whole pass
                 } );
            graph.AddPass(
                 i == 0 ? "CopyFirst" : "CopySecond", RDG::PassFlags::Copy,
                 [&textures, &readbacks, i]( RDG::PassBuilder& pass )
                 {
                     pass.Read( textures[i], RDG::Access::CopySrc );
                     pass.Write( readbacks[i], RDG::Access::CopyDst );
                 },
                 [&textures, &readbacks, &vulkan, size, i]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     // The graph runs through the witness decorator, so the context's backend is not the
                     // Vulkan one: the segment's command buffer comes from the Vulkan backend it wraps.
                     const VkCommandBuffer passCmd = vulkan.GetRecordingCommandBuffer();
                     if ( passCmd == VK_NULL_HANDLE )
                         return Fail( "the pass has no recording command buffer" );
                     auto source = context.GetTexture( textures[i], RDG::Access::CopySrc );
                     auto target = context.GetBuffer( readbacks[i], RDG::Access::CopyDst );
                     if ( !source || !target )
                         return Fail( !source ? source.GetError() : target.GetError() );
                     auto image  = VulkanRdgBackend::TextureOf( source.GetValue() );
                     auto buffer = VulkanRdgBackend::BufferOf( target.GetValue() );
                     if ( !image || !buffer )
                         return Fail( "no Vulkan resource" );
                     VkBufferImageCopy region{};
                     region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                     region.imageExtent      = { size, size, 1 };
                     vkCmdCopyImageToBuffer( passCmd, image.GetValue()->GetImage(),
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.GetValue()->GetBuffer(),
                                             1, &region );
                     return Common::MakeSuccess( true );
                 } );
        }
        graph.Extract( readbacks[0], out.FirstBytes, RDG::Access::HostRead );
        graph.Extract( readbacks[1], out.SecondBytes, RDG::Access::HostRead );

        const Common::BoolResultStr executed = graph.Execute( witness );
        vkEndCommandBuffer( cmd );
        if ( !executed )
        {
            out.Error = executed.GetError();
            return;
        }
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cmd;
        SubmitFrame( gpu, vulkan, submit, out.Fence );
    }

    // Waits for the frame's fence, releases its command buffer and fence, and checks both readbacks texel by
    // texel. Empty when both hold their own clear colour.
    std::string FinishAliasFrame( Gpu& gpu, AliasFrame& frame )
    {
        vkWaitForFences( gpu.Device.device, 1, &frame.Fence, VK_TRUE, UINT64_MAX );
        vkDestroyFence( gpu.Device.device, frame.Fence, nullptr );
        vkFreeCommandBuffers( gpu.Device.device, gpu.CommandPool, 1, &frame.CommandBuffer );
        for ( uint32_t i = 0; i < 2; ++i )
        {
            const RDG::ExternalBuffer&    readback = i == 0 ? frame.FirstBytes : frame.SecondBytes;
            const std::array<uint8_t, 4>& texel    = i == 0 ? kFirstTexel : kSecondTexel;
            if ( !readback.Physical || readback.Physical->GetBackendKind() != RDG::BackendKind::Vulkan )
                return std::format( "readback {} carries no Vulkan buffer", i );
            const auto& buffer = static_cast<const VulkanRdgBuffer&>( *readback.Physical );
            buffer.InvalidateForHost();
            const auto* mapped = static_cast<const uint8_t*>( buffer.GetMapped() );
            if ( mapped == nullptr )
                return std::format( "readback {} is not host-visible", i );
            for ( uint64_t b = 0; b < uint64_t( frame.Size ) * frame.Size * 4; ++b )
            {
                if ( mapped[b] != texel[b % 4] )
                    return std::format( "{} byte {}: {} instead of {}", i == 0 ? "First" : "Second", b,
                                        int( mapped[b] ), int( texel[b % 4] ) );
            }
        }
        return {};
    }
} // namespace

// RDG-ALIAS A1: two transients with disjoint lifetimes are PLACED over the same heap bytes - two VkImages bound
// to one VkDeviceMemory at one offset - and the second is ordered after the first only by its AliasAcquire
// barrier. Each is cleared to its own colour and copied out: both readbacks are exact and validation
// (synchronization validation included) is silent. The slot's heap is exactly the plan's peak; a heap the next
// plan outgrows is retired, still reserved while other frames are in flight, and freed only when its slot is
// re-begun after that slot's fence.
TEST( RenderGraphVulkan, TwoTransientsWithDisjointLifetimesArePlacedOverTheSameBytesValidationClean )
{
    Gpu& gpu = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    {
        VulkanRdgPool    pool( gpu.Rdg, 2 );
        FrameObjects     frame( gpu, 2 );
        VulkanRdgBackend backend( gpu.Rdg, pool );
        WeakenedBarriers witness( backend, "" ); // weakens no pass: it records the plan and the barriers

        AliasFrame a;
        RecordAliasFrame( gpu, backend, witness, pool, frame, 0, kSize, a );
        ASSERT_TRUE( a.Error.empty() ) << a.Error;
        EXPECT_EQ( FinishAliasFrame( gpu, a ), "" );

        const RDG::AliasingPlan plan = witness.Plan;
        ASSERT_EQ( plan.Allocations.size(), 2u );
        const auto second =
             std::find_if( plan.Allocations.begin(), plan.Allocations.end(),
                           []( const RDG::Allocation& entry ) { return !entry.AliasPredecessors.empty(); } );
        ASSERT_NE( second, plan.Allocations.end() ) << "the plan did not alias Second over First";
        const RDG::Allocation& first =
             second == plan.Allocations.begin() ? plan.Allocations[1] : plan.Allocations[0];
        EXPECT_EQ( second->AliasPredecessors, std::vector<uint32_t>{ first.Resource } );
        EXPECT_EQ( second->Heap, first.Heap );
        EXPECT_EQ( second->Offset, first.Offset );
        EXPECT_NE( a.Images[0], a.Images[1] ) << "two transients sharing bytes must be two images";
        EXPECT_TRUE( std::any_of( witness.Recorded.begin(), witness.Recorded.end(),
                                  [&]( const std::pair<std::string, RDG::Barrier>& recorded )
                                  {
                                      return recorded.second.BarrierType == RDG::BarrierKind::AliasAcquire &&
                                             recorded.second.Resource == second->Resource;
                                  } ) )
             << "Second's first use must be an AliasAcquire";

        uint64_t heapBytes = 0;
        for ( const RDG::TransientHeapDesc& heap : plan.Heaps )
            heapBytes += heap.Bytes;
        EXPECT_EQ( heapBytes, plan.TotalPeakBytes );
        const RDG::TransientAllocatorStats placed = frame.Transients.GetStats();
        EXPECT_EQ( placed.HeapCount, 1u );
        EXPECT_EQ( placed.ReservedBytes, plan.TotalPeakBytes );
        EXPECT_EQ( placed.PlacedResources, 2u );

        // Frame B (slot 1) stays in flight while frame C re-begins slot 0 with textures four times larger: slot
        // 0's heap grows, so its old heap is retired and still reserved; slot 1's heap is not touched.
        AliasFrame b;
        RecordAliasFrame( gpu, backend, witness, pool, frame, 1, kSize, b );
        ASSERT_TRUE( b.Error.empty() ) << b.Error;
        AliasFrame c;
        RecordAliasFrame( gpu, backend, witness, pool, frame, 0, kSize * 2, c );
        ASSERT_TRUE( c.Error.empty() ) << c.Error;
        const uint64_t grown = witness.Plan.TotalPeakBytes;
        EXPECT_GT( grown, plan.TotalPeakBytes );
        const RDG::TransientAllocatorStats inFlight = frame.Transients.GetStats();
        EXPECT_EQ( inFlight.HeapCount, 3u ) << "slot 0's new heap, slot 0's retired heap, slot 1's heap";
        EXPECT_EQ( inFlight.ReservedBytes, grown + 2 * plan.TotalPeakBytes );
        EXPECT_EQ( FinishAliasFrame( gpu, b ), "" );
        EXPECT_EQ( FinishAliasFrame( gpu, c ), "" );

        // Slot 0 re-begun after its fence: the retired heap is freed only now.
        ASSERT_TRUE( BeginFrameSlot( frame, backend, pool, 0 ) );
        const RDG::TransientAllocatorStats retired = frame.Transients.GetStats();
        EXPECT_EQ( retired.HeapCount, 2u );
        EXPECT_EQ( retired.ReservedBytes, grown + plan.TotalPeakBytes );
        vkDeviceWaitIdle( gpu.Device.device );
    }
    const std::vector<Message> messages = TakeMessages();
    EXPECT_TRUE( messages.empty() ) << messages.size() << " validation message(s):" << Join( messages );
}

namespace
{
    // One async graph: "Produce" (Compute | AsyncCompute) writes a transient through a storage-buffer dispatch - a
    // constant fill, or with @p source a shader copy of that external at position 0 (amendment B: the graphics
    // prologue releases it) - and "Consume" (Compute, graphics pipe) copies it by dispatch into a host-read buffer.
    // Both passes are dispatches: an AsyncCompute pass is a Compute pass, and a Compute pass may not declare
    // CopySrc/CopyDst (RdgPassKindAllows). The readback is a storage buffer so the second graph can read it.
    struct AsyncCopyRun
    {
        std::string          Error;
        std::vector<uint8_t> Bytes;
        RDG::ExternalBuffer  Readback;
        VkCommandBuffer      ProduceBuffer = VK_NULL_HANDLE; // what each pass recorded into
        VkCommandBuffer      ConsumeBuffer = VK_NULL_HANDLE;
    };

    // Stands in for VulkanRendererAPI: its recording target is written ONLY by the backend's RecordingListener,
    // exactly as VulkanRendererAPI::SetGraphRecordingTarget is, and a pass that records "through the renderer"
    // records into whatever that target is at the time.
    struct RecordingTarget
    {
        VkCommandBuffer Current = VK_NULL_HANDLE;
        int             Changes = 0;
    };

    constexpr uint32_t kAsyncBytes      = 4096;
    constexpr uint32_t kAsyncGroupSize  = 64;
    constexpr uint32_t kAsyncGroupCount = kAsyncBytes / 4 / kAsyncGroupSize;

    constexpr const char* kAsyncFill = R"(#version 450
layout( local_size_x = 64 ) in;
layout( set = 0, binding = 1 ) writeonly buffer Target { uint Words[]; };
void main() { Words[gl_GlobalInvocationID.x] = 0xA5C3E1F7u; })";

    constexpr const char* kAsyncCopy = R"(#version 450
layout( local_size_x = 64 ) in;
layout( set = 0, binding = 0 ) readonly buffer Source { uint From[]; };
layout( set = 0, binding = 1 ) writeonly buffer Target { uint Words[]; };
void main() { Words[gl_GlobalInvocationID.x] = From[gl_GlobalInvocationID.x]; })";

    // The two dispatches' pipelines: "fill" writes binding 1, "copy" copies binding 0 into binding 1. Each pass
    // allocates its own set from the pool and points it at the buffers the graph hands it at execution.
    struct AsyncDispatch
    {
        explicit AsyncDispatch( VkDevice device ) : Device( device )
        {
            const auto fill = CompileGlsl( kAsyncFill, shaderc_compute_shader, "async_fill.comp" );
            const auto copy = CompileGlsl( kAsyncCopy, shaderc_compute_shader, "async_copy.comp" );
            if ( !fill || !copy )
            {
                Error = !fill ? fill.GetError() : copy.GetError();
                return;
            }
            const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {
                 VkDescriptorSetLayoutBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                               nullptr },
                 VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                               nullptr } };
            VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutInfo.bindingCount = static_cast<uint32_t>( bindings.size() );
            layoutInfo.pBindings    = bindings.data();
            vkCreateDescriptorSetLayout( Device, &layoutInfo, nullptr, &SetLayout );
            VkPipelineLayoutCreateInfo pipeLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pipeLayoutInfo.setLayoutCount = 1;
            pipeLayoutInfo.pSetLayouts    = &SetLayout;
            vkCreatePipelineLayout( Device, &pipeLayoutInfo, nullptr, &PipeLayout );
            Fill = MakePipeline( fill.GetValue() );
            Copy = MakePipeline( copy.GetValue() );
            const VkDescriptorPoolSize size{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 };
            VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            poolInfo.maxSets       = 2;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes    = &size;
            vkCreateDescriptorPool( Device, &poolInfo, nullptr, &Pool );
            if ( SetLayout == VK_NULL_HANDLE || PipeLayout == VK_NULL_HANDLE || Fill == VK_NULL_HANDLE ||
                 Copy == VK_NULL_HANDLE || Pool == VK_NULL_HANDLE )
                Error = "the async dispatch pipelines or descriptor pool could not be created";
        }

        ~AsyncDispatch()
        {
            vkDeviceWaitIdle( Device );
            vkDestroyDescriptorPool( Device, Pool, nullptr );
            vkDestroyPipeline( Device, Fill, nullptr );
            vkDestroyPipeline( Device, Copy, nullptr );
            vkDestroyPipelineLayout( Device, PipeLayout, nullptr );
            vkDestroyDescriptorSetLayout( Device, SetLayout, nullptr );
        }

        AsyncDispatch( const AsyncDispatch& )            = delete;
        AsyncDispatch& operator=( const AsyncDispatch& ) = delete;

        // Records one dispatch: @p from == VK_NULL_HANDLE fills @p to, otherwise copies @p from into @p to.
        Common::BoolResultStr Record( VkCommandBuffer cmd, VkBuffer from, VkBuffer to )
        {
            VkDescriptorSetAllocateInfo setInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            setInfo.descriptorPool     = Pool;
            setInfo.descriptorSetCount = 1;
            setInfo.pSetLayouts        = &SetLayout;
            VkDescriptorSet set        = VK_NULL_HANDLE;
            if ( const VkResult allocated = vkAllocateDescriptorSets( Device, &setInfo, &set ); allocated != VK_SUCCESS )
                return Fail( std::format( "async dispatch: vkAllocateDescriptorSets returned {}",
                                          static_cast<int>( allocated ) ) );
            const VkDescriptorBufferInfo        source{ from, 0, kAsyncBytes };
            const VkDescriptorBufferInfo        target{ to, 0, kAsyncBytes };
            std::array<VkWriteDescriptorSet, 2> writes{};
            for ( VkWriteDescriptorSet& write : writes )
            {
                write                 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                write.dstSet          = set;
                write.descriptorCount = 1;
                write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            writes[0].dstBinding  = 1;
            writes[0].pBufferInfo = &target;
            writes[1].dstBinding  = 0;
            writes[1].pBufferInfo = &source;
            // The fill shader does not use binding 0, so its set leaves it unwritten.
            const uint32_t count = from == VK_NULL_HANDLE ? 1u : 2u;
            vkUpdateDescriptorSets( Device, count, writes.data(), 0, nullptr );
            vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, from == VK_NULL_HANDLE ? Fill : Copy );
            vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, PipeLayout, 0, 1, &set, 0, nullptr );
            vkCmdDispatch( cmd, kAsyncGroupCount, 1, 1 );
            return Common::MakeSuccess( true );
        }

        VkDevice              Device     = VK_NULL_HANDLE;
        VkDescriptorSetLayout SetLayout  = VK_NULL_HANDLE;
        VkPipelineLayout      PipeLayout = VK_NULL_HANDLE;
        VkPipeline            Fill       = VK_NULL_HANDLE;
        VkPipeline            Copy       = VK_NULL_HANDLE;
        VkDescriptorPool      Pool       = VK_NULL_HANDLE;
        std::string           Error;

    private:
        VkPipeline MakePipeline( const std::vector<uint32_t>& code )
        {
            const VkShaderModule        module_ = Module( Device, code );
            VkComputePipelineCreateInfo pipeInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pipeInfo.stage        = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            pipeInfo.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            pipeInfo.stage.module = module_;
            pipeInfo.stage.pName  = "main";
            pipeInfo.layout       = PipeLayout;
            VkPipeline pipeline   = VK_NULL_HANDLE;
            vkCreateComputePipelines( Device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipeline );
            vkDestroyShaderModule( Device, module_, nullptr );
            return pipeline;
        }
    };

    // The Vulkan buffer behind @p ref at @p access in this pass, or VK_NULL_HANDLE with @p error set.
    VkBuffer PassBuffer( RDG::PassContext& context, RDG::BufferRef ref, RDG::Access access, std::string& error )
    {
        auto buffer = context.GetBuffer( ref, access );
        if ( !buffer )
        {
            error = buffer.GetError();
            return VK_NULL_HANDLE;
        }
        auto vulkan = VulkanRdgBackend::BufferOf( buffer.GetValue() );
        if ( !vulkan )
        {
            error = vulkan.GetError();
            return VK_NULL_HANDLE;
        }
        return vulkan.GetValue()->GetBuffer();
    }

    // With @p target, each pass records its dispatch into target->Current (a renderer Record()) after checking it
    // IS the pass's segment buffer; without, into CommandBufferOf(context).
    AsyncCopyRun RunAsyncCopy( Gpu& gpu, VulkanRdgBackend& backend, VulkanRdgPool& pool, FrameObjects& frame,
                               RDG::ExternalBuffer* source, const RecordingTarget* target = nullptr )
    {
        AsyncCopyRun  run;
        AsyncDispatch dispatch( gpu.Device.device );
        if ( !dispatch.Error.empty() )
        {
            run.Error = dispatch.Error;
            return run;
        }
        if ( const Common::BoolResultStr begun = BeginFrameSlot( frame, backend, pool, 0 ); !begun )
        {
            run.Error = begun.GetError();
            return run;
        }
        RDG::Builder         graph( "async-copy" );
        const RDG::BufferRef produced = graph.CreateBuffer( RDG::BufferDesc{ kAsyncBytes }, "Produced" );
        const RDG::BufferRef bytes    = graph.CreateBuffer( RDG::BufferDesc{ kAsyncBytes }, "Readback" );
        RDG::BufferRef       input;
        if ( source != nullptr )
            input = graph.RegisterExternal( *source, "Previous" );
        graph.AddPass(
             "Produce", RDG::PassFlags::Compute | RDG::PassFlags::AsyncCompute,
             [&]( RDG::PassBuilder& pass )
             {
                 if ( source != nullptr )
                     pass.Read( input, RDG::Access::StorageRead );
                 pass.Write( produced, RDG::Access::StorageWrite );
             },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 auto cmd = VulkanRdgBackend::CommandBufferOf( context );
                 if ( !cmd )
                     return Fail( cmd.GetError() );
                 std::string    error;
                 const VkBuffer to   = PassBuffer( context, produced, RDG::Access::StorageWrite, error );
                 const VkBuffer from = source != nullptr
                                            ? PassBuffer( context, input, RDG::Access::StorageRead, error )
                                            : VK_NULL_HANDLE;
                 if ( !error.empty() )
                     return Fail( std::format( "Produce: {}", error ) );
                 if ( target != nullptr && target->Current != cmd.GetValue() )
                     return Fail( "Produce: the recording target is not the pass's segment command buffer" );
                 run.ProduceBuffer = target != nullptr ? target->Current : cmd.GetValue();
                 return dispatch.Record( run.ProduceBuffer, from, to );
             } );
        graph.AddPass(
             "Consume", RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( produced, RDG::Access::StorageRead );
                 pass.Write( bytes, RDG::Access::StorageWrite );
             },
             [&]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 auto cmd = VulkanRdgBackend::CommandBufferOf( context );
                 if ( !cmd )
                     return Fail( cmd.GetError() );
                 std::string    error;
                 const VkBuffer from = PassBuffer( context, produced, RDG::Access::StorageRead, error );
                 const VkBuffer to   = PassBuffer( context, bytes, RDG::Access::StorageWrite, error );
                 if ( !error.empty() )
                     return Fail( std::format( "Consume: {}", error ) );
                 if ( target != nullptr && target->Current != cmd.GetValue() )
                     return Fail( "Consume: the recording target is not the pass's segment command buffer" );
                 run.ConsumeBuffer = target != nullptr ? target->Current : cmd.GetValue();
                 return dispatch.Record( run.ConsumeBuffer, from, to );
             } );
        graph.Extract( bytes, run.Readback, RDG::Access::HostRead );

        const Common::BoolResultStr executed = graph.Execute( backend );
        if ( !executed )
        {
            run.Error = executed.GetError();
            return run;
        }
        const std::vector<VulkanRdgSubmission> segments = backend.TakeSubmissions();
        for ( const VulkanRdgSubmission& entry : segments )
        {
            VkSubmitInfo info{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            info.waitSemaphoreCount   = static_cast<uint32_t>( entry.WaitSemaphores.size() );
            info.pWaitSemaphores      = entry.WaitSemaphores.data();
            info.pWaitDstStageMask    = entry.WaitStages.data();
            info.commandBufferCount   = 1;
            info.pCommandBuffers      = &entry.CommandBuffer;
            info.signalSemaphoreCount = static_cast<uint32_t>( entry.SignalSemaphores.size() );
            info.pSignalSemaphores    = entry.SignalSemaphores.data();
            vkQueueSubmit( entry.Queue, 1, &info, VK_NULL_HANDLE );
        }
        vkDeviceWaitIdle( gpu.Device.device );
        if ( !run.Readback.Physical )
        {
            run.Error = "the extracted readback buffer carries no physical buffer";
            return run;
        }
        const auto& buffer = static_cast<const VulkanRdgBuffer&>( *run.Readback.Physical );
        buffer.InvalidateForHost();
        if ( buffer.GetMapped() == nullptr )
        {
            run.Error = "the readback buffer is not host-visible";
            return run;
        }
        run.Bytes.resize( kAsyncBytes );
        std::memcpy( run.Bytes.data(), buffer.GetMapped(), kAsyncBytes );
        return run;
    }

    bool AllFilled( const std::vector<uint8_t>& bytes )
    {
        for ( size_t i = 0; i + 4 <= bytes.size(); i += 4 )
        {
            uint32_t word = 0;
            std::memcpy( &word, bytes.data() + i, 4 );
            if ( word != 0xA5C3E1F7u )
                return false;
        }
        return !bytes.empty();
    }
} // namespace

// B2 on the GPU, sync validation on: an AsyncCompute pass writes, a later graphics pass reads (fork-free, joined,
// ownership released on compute and acquired on graphics); then a second graph whose async pass sits at position
// 0 and consumes the first graph's output (amendment B: the graphics prologue releases it and forks).
TEST( RenderGraphVulkan, AsyncComputeAcrossQueuesAndFromTheGraphStartIsValidationClean )
{
    Gpu& gpu = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    if ( gpu.ComputeQueue == VK_NULL_HANDLE )
        GTEST_SKIP() << "the device has no compute family other than the graphics one";
    {
        VulkanRdgPool    pool( gpu.Rdg, 1 );
        FrameObjects     frame( gpu, 1, true );
        VulkanRdgBackend backend( gpu.Rdg, pool );
        ASSERT_TRUE( frame.Queues.GetCapabilities().SeparateComputeFamily );

        AsyncCopyRun first = RunAsyncCopy( gpu, backend, pool, frame, nullptr );
        ASSERT_TRUE( first.Error.empty() ) << first.Error;
        EXPECT_TRUE( AllFilled( first.Bytes ) );
        AsyncCopyRun second = RunAsyncCopy( gpu, backend, pool, frame, &first.Readback );
        ASSERT_TRUE( second.Error.empty() ) << second.Error;
        EXPECT_TRUE( AllFilled( second.Bytes ) );
        EXPECT_EQ( backend.GetAsyncComputeFallbackLog().GetLinesLogged(), 0u );
    }
    const std::vector<Message> messages = TakeMessages();
    EXPECT_TRUE( messages.empty() ) << messages.size() << " validation message(s):" << Join( messages );
}

// The architect's condition on the B2 recording target: a renderer Record() inside a pass lands in that pass's
// segment command buffer. The recording target is written only by the backend's listener (as the engine's
// VulkanRendererAPI::SetGraphRecordingTarget is); every pass records its dispatch THROUGH it into an
// async-adjacent graph (async Produce, graphics Consume), and the frame is byte-exact and validation-clean.
TEST( RenderGraphVulkan, ARecordThroughTheRecordingTargetLandsInThePassSegmentBuffer )
{
    Gpu& gpu = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    if ( gpu.ComputeQueue == VK_NULL_HANDLE )
        GTEST_SKIP() << "the device has no compute family other than the graphics one";
    {
        VulkanRdgPool    pool( gpu.Rdg, 1 );
        FrameObjects     frame( gpu, 1, true );
        VulkanRdgBackend backend( gpu.Rdg, pool );
        RecordingTarget  target;
        backend.SetRecordingListener(
             [&target]( VkCommandBuffer commandBuffer )
             {
                 target.Current = commandBuffer;
                 ++target.Changes;
             } );

        AsyncCopyRun run = RunAsyncCopy( gpu, backend, pool, frame, nullptr, &target );
        ASSERT_TRUE( run.Error.empty() ) << run.Error;
        EXPECT_TRUE( AllFilled( run.Bytes ) );
        // Two pipes, two segments: the async pass and the graphics pass recorded into different buffers, and
        // neither is the one the target holds once the submissions are handed over.
        EXPECT_NE( run.ProduceBuffer, VK_NULL_HANDLE );
        EXPECT_NE( run.ConsumeBuffer, VK_NULL_HANDLE );
        EXPECT_NE( run.ProduceBuffer, run.ConsumeBuffer );
        EXPECT_EQ( target.Current, VK_NULL_HANDLE ) << "TakeSubmissions must clear the recording target";
        // Graph begin, then at least one begin and one end per segment, then the hand-over.
        EXPECT_GE( target.Changes, 6 );
    }
    const std::vector<Message> messages = TakeMessages();
    EXPECT_TRUE( messages.empty() ) << messages.size() << " validation message(s):" << Join( messages );
}

// The same two graphs with a queue set that has no compute queue - what the frame loop builds on a device with
// one family: every pass runs on Graphics, the bytes are the same, and the backend logs the fallback ONCE.
TEST( RenderGraphVulkan, AsyncComputeWithoutASeparateFamilyRunsOnGraphicsAndLogsOnce )
{
    Gpu& gpu = GetGpu();
    ASSERT_TRUE( gpu.Error.empty() ) << gpu.Error;
    {
        VulkanRdgPool    pool( gpu.Rdg, 1 );
        FrameObjects     frame( gpu, 1, false );
        VulkanRdgBackend backend( gpu.Rdg, pool );
        ASSERT_FALSE( backend.GetPipeCapabilities().SeparateComputeFamily );

        AsyncCopyRun first = RunAsyncCopy( gpu, backend, pool, frame, nullptr );
        ASSERT_TRUE( first.Error.empty() ) << first.Error;
        EXPECT_TRUE( AllFilled( first.Bytes ) );
        AsyncCopyRun second = RunAsyncCopy( gpu, backend, pool, frame, &first.Readback );
        ASSERT_TRUE( second.Error.empty() ) << second.Error;
        EXPECT_TRUE( AllFilled( second.Bytes ) );
        EXPECT_EQ( backend.GetAsyncComputeFallbackLog().GetLinesLogged(), 1u );
    }
    const std::vector<Message> messages = TakeMessages();
    EXPECT_TRUE( messages.empty() ) << messages.size() << " validation message(s):" << Join( messages );
}
