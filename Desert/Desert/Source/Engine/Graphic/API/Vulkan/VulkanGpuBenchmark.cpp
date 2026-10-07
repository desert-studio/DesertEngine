#include <Engine/Core/GpuBenchmark.hpp>

#include <Engine/Core/Formats/Shader.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCompiler.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// THE VULKAN GPU BENCHMARK (SCAL1). Two synthetic compute workloads, each the same shape as UE FSynthBenchmark's
// GPU passes: a fixed amount of work, timed with vkCmdWriteTimestamp on the graphics queue, best of kRepeats.
//   * Alu       — independent FMA chains per invocation; work = FLOPs (an FMA is 2).
//   * Bandwidth — a storage-buffer copy of kBandwidthBytes, back and forth; work = bytes read + written.
// The perf index is GpuPerfIndex over the passes against kReference below. Nothing here survives Run: every
// object is created for the run and destroyed before it returns, so the benchmark costs no memory afterwards.
namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        constexpr uint32_t kGroupSize         = 256;
        constexpr uint32_t kAluGroups         = 4096; // 1 Mi invocations
        constexpr uint32_t kAluIterations     = 2048; // per invocation, 4 FMA chains each
        constexpr uint32_t kAluChains         = 4;
        constexpr uint64_t kBandwidthBytes    = 128ull << 20; // per buffer; a copy reads and writes this much
        constexpr uint32_t kDispatchesPerPass = 4;            // back-to-back dispatches between the two timestamps
        constexpr uint32_t kRepeats           = 3;            // the pass is timed this often; the fastest counts

        // The reference rates (work units per ms) of these exact workloads: the reference GPU's (RTX 3070 Ti)
        // datasheet peaks — 21.75 TFLOP/s FP32, 608 GB/s. A real kernel reaches a fraction of a peak, so the
        // reference GPU itself scores that fraction (below 100); the table's thresholds are read against the
        // same scale. Re-measure and replace both together with the workloads above when either changes.
        const std::vector<Common::Scalability::BenchmarkReference> kReference = {
             { "Alu", 21.75e12 / 1000.0 },
             { "Bandwidth", 608.0e9 / 1000.0 },
        };

        std::string AluSource()
        {
            return std::format( "#version 450\n"
                                "layout(local_size_x = {}) in;\n"
                                "layout(std430, set = 0, binding = 1) writeonly buffer Out {{ vec4 Values[]; }} o;\n"
                                "void main()\n"
                                "{{\n"
                                "    float seed = float(gl_GlobalInvocationID.x) * 1.0e-6;\n"
                                "    vec4 a = vec4(seed, seed + 0.1, seed + 0.2, seed + 0.3);\n"
                                "    vec4 b = a + 0.5, c = a + 0.25, d = a + 0.75;\n"
                                "    for (uint i = 0u; i < {}u; ++i)\n"
                                "    {{\n"
                                "        a = fma(a, vec4(0.9999), vec4(1.0e-4));\n"
                                "        b = fma(b, vec4(0.9998), vec4(2.0e-4));\n"
                                "        c = fma(c, vec4(0.9997), vec4(3.0e-4));\n"
                                "        d = fma(d, vec4(0.9996), vec4(4.0e-4));\n"
                                "    }}\n"
                                "    o.Values[gl_GlobalInvocationID.x] = a + b + c + d;\n"
                                "}}\n",
                                kGroupSize, kAluIterations );
        }
        // kAluChains vec4 FMAs per iteration: 4 lanes x 2 FLOPs each.
        constexpr double kAluFlopsPerDispatch =
             double( kAluGroups ) * kGroupSize * kAluIterations * kAluChains * 4.0 * 2.0;

        std::string BandwidthSource()
        {
            return std::format(
                 "#version 450\n"
                 "layout(local_size_x = {}) in;\n"
                 "layout(std430, set = 0, binding = 0) readonly buffer In {{ vec4 Values[]; }} i;\n"
                 "layout(std430, set = 0, binding = 1) writeonly buffer Out {{ vec4 Values[]; }} o;\n"
                 "void main() {{ o.Values[gl_GlobalInvocationID.x] = i.Values[gl_GlobalInvocationID.x]; }}\n",
                 kGroupSize );
        }
        constexpr uint32_t kBandwidthGroups = static_cast<uint32_t>( kBandwidthBytes / 16 / kGroupSize );
        static_assert( kBandwidthBytes % ( 16ull * kGroupSize ) == 0 );
        static_assert( uint64_t( kAluGroups ) * kGroupSize * 16 <= kBandwidthBytes,
                       "the ALU pass writes one vec4 per invocation into the second buffer" );

        // Destroys what was created, in reverse, however Run leaves.
        class Cleanup
        {
        public:
            ~Cleanup()
            {
                for ( auto it = m_Steps.rbegin(); it != m_Steps.rend(); ++it )
                    ( *it )();
            }
            void Add( std::function<void()> step )
            {
                m_Steps.push_back( std::move( step ) );
            }

        private:
            std::vector<std::function<void()>> m_Steps;
        };

        class VulkanGpuBenchmark final : public Engine::GpuBenchmark
        {
        public:
            Common::ResultStr<Common::Scalability::BenchmarkResult> Run( Engine::Device& engineDevice ) override;
        };

        using Result = Common::ResultStr<Common::Scalability::BenchmarkResult>;

        Result Fail( const std::string& what )
        {
            return Common::MakeFormattedError<Common::Scalability::BenchmarkResult>( "GPU benchmark: {}", what );
        }

        Result VulkanGpuBenchmark::Run( Engine::Device& engineDevice )
        {
            const Engine::DeviceCapabilities&    caps = engineDevice.GetCapabilities();
            Common::Scalability::BenchmarkResult result;
            result.Class       = caps.Catalog.Class;
            result.VideoMemory = caps.Catalog.VideoMemory;

            // A device that cannot time a pass gives no index: RecommendLevels takes the device-class fallback
            // (Timed false says so). Not a wall-clock guess.
            if ( caps.Catalog.Timing != Common::Scalability::GpuTiming::AnyStage )
                return Common::MakeSuccess( std::move( result ) );
            if ( !( caps.TimestampPeriodNs > 0.0f ) )
                return Fail( std::format( "the catalog says AnyStage timing but timestampPeriod is {}",
                                          caps.TimestampPeriodNs ) );

            auto&          device = static_cast<VulkanLogicalDevice&>( engineDevice );
            const VkDevice vk     = device.GetVulkanLogicalDevice();
            Cleanup        cleanup;

            // ---- Buffers: two device-local storage buffers --------------------------------------------------
            VmaAllocator&                vma = VulkanAllocator::GetVMAAllocator();
            std::array<VkBuffer, 2>      buffers{};
            std::array<VmaAllocation, 2> allocations{};
            for ( std::size_t b = 0; b < buffers.size(); ++b )
            {
                VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bufferInfo.size        = kBandwidthBytes;
                bufferInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                VmaAllocationCreateInfo allocInfo{};
                allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
                const VkResult r =
                     vmaCreateBuffer( vma, &bufferInfo, &allocInfo, &buffers[b], &allocations[b], nullptr );
                if ( r != VK_SUCCESS )
                    return Fail( std::format( "vmaCreateBuffer ({} MiB) returned {}", kBandwidthBytes >> 20,
                                              static_cast<int>( r ) ) );
                cleanup.Add( [&vma, &buffers, &allocations, b]
                             { vmaDestroyBuffer( vma, buffers[b], allocations[b] ); } );
            }

            // ---- Descriptors: binding 0 = in, binding 1 = out; two sets, A->B and B->A ---------------------
            std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
            for ( uint32_t i = 0; i < 2; ++i )
            {
                bindings[i].binding         = i;
                bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo setLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            setLayoutInfo.bindingCount      = static_cast<uint32_t>( bindings.size() );
            setLayoutInfo.pBindings         = bindings.data();
            VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
            if ( const VkResult r = vkCreateDescriptorSetLayout( vk, &setLayoutInfo, nullptr, &setLayout );
                 r != VK_SUCCESS )
                return Fail( std::format( "vkCreateDescriptorSetLayout returned {}", static_cast<int>( r ) ) );
            cleanup.Add( [vk, setLayout] { vkDestroyDescriptorSetLayout( vk, setLayout, nullptr ); } );

            const VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 };
            VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            poolInfo.maxSets       = 2;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes    = &poolSize;
            VkDescriptorPool pool  = VK_NULL_HANDLE;
            if ( const VkResult r = vkCreateDescriptorPool( vk, &poolInfo, nullptr, &pool ); r != VK_SUCCESS )
                return Fail( std::format( "vkCreateDescriptorPool returned {}", static_cast<int>( r ) ) );
            cleanup.Add( [vk, pool] { vkDestroyDescriptorPool( vk, pool, nullptr ); } );

            const std::array<VkDescriptorSetLayout, 2> setLayouts{ setLayout, setLayout };
            VkDescriptorSetAllocateInfo setAllocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            setAllocInfo.descriptorPool     = pool;
            setAllocInfo.descriptorSetCount = 2;
            setAllocInfo.pSetLayouts        = setLayouts.data();
            std::array<VkDescriptorSet, 2> sets{};
            if ( const VkResult r = vkAllocateDescriptorSets( vk, &setAllocInfo, sets.data() ); r != VK_SUCCESS )
                return Fail( std::format( "vkAllocateDescriptorSets returned {}", static_cast<int>( r ) ) );
            for ( std::size_t s = 0; s < sets.size(); ++s )
            {
                const std::array<VkDescriptorBufferInfo, 2> infos{
                     VkDescriptorBufferInfo{ buffers[s], 0, VK_WHOLE_SIZE },
                     VkDescriptorBufferInfo{ buffers[1 - s], 0, VK_WHOLE_SIZE } };
                std::array<VkWriteDescriptorSet, 2> writes{};
                for ( uint32_t i = 0; i < 2; ++i )
                {
                    writes[i]                 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    writes[i].dstSet          = sets[s];
                    writes[i].dstBinding      = i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[i].pBufferInfo     = &infos[i];
                }
                vkUpdateDescriptorSets( vk, 2, writes.data(), 0, nullptr );
            }

            VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            layoutInfo.setLayoutCount       = 1;
            layoutInfo.pSetLayouts          = &setLayout;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            if ( const VkResult r = vkCreatePipelineLayout( vk, &layoutInfo, nullptr, &pipelineLayout );
                 r != VK_SUCCESS )
                return Fail( std::format( "vkCreatePipelineLayout returned {}", static_cast<int>( r ) ) );
            cleanup.Add( [vk, pipelineLayout] { vkDestroyPipelineLayout( vk, pipelineLayout, nullptr ); } );

            // ---- Pipelines -------------------------------------------------------------------------------------
            struct Pass
            {
                const char* Name;
                std::string Source;
                uint32_t    Groups;
                double      WorkPerDispatch;
                VkPipeline  Pipeline = VK_NULL_HANDLE;
            };
            std::array<Pass, 2> passes{ Pass{ "Alu", AluSource(), kAluGroups, kAluFlopsPerDispatch },
                                        Pass{ "Bandwidth", BandwidthSource(), kBandwidthGroups,
                                              2.0 * static_cast<double>( kBandwidthBytes ) } };
            for ( Pass& pass : passes )
            {
                const std::string path  = std::format( "<GpuBenchmark:{}>", pass.Name );
                auto              spirv = ::Desert::Core::ShaderCompiler::CompileGLSLToSPIRV(
                     ::Desert::Core::Formats::ShaderStage::Compute, pass.Source, path );
                if ( !spirv.IsSuccess() )
                    return Fail( std::format( "pass '{}' did not compile: {}", pass.Name, spirv.GetError() ) );
                const std::vector<uint32_t>& code = spirv.GetValue();

                VkShaderModuleCreateInfo moduleInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                moduleInfo.codeSize   = code.size() * sizeof( uint32_t );
                moduleInfo.pCode      = code.data();
                VkShaderModule module = VK_NULL_HANDLE;
                if ( const VkResult r = vkCreateShaderModule( vk, &moduleInfo, nullptr, &module );
                     r != VK_SUCCESS )
                    return Fail( std::format( "vkCreateShaderModule ({}) returned {}", pass.Name,
                                              static_cast<int>( r ) ) );

                VkComputePipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
                pipelineInfo.stage        = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
                pipelineInfo.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
                pipelineInfo.stage.module = module;
                pipelineInfo.stage.pName  = "main";
                pipelineInfo.layout       = pipelineLayout;
                const VkResult r = vkCreateComputePipelines( vk, device.GetPipelineCache(), 1, &pipelineInfo,
                                                             nullptr, &pass.Pipeline );
                vkDestroyShaderModule( vk, module, nullptr );
                if ( r != VK_SUCCESS )
                    return Fail( std::format( "vkCreateComputePipelines ({}) returned {}", pass.Name,
                                              static_cast<int>( r ) ) );
                cleanup.Add( [vk, p = pass.Pipeline] { vkDestroyPipeline( vk, p, nullptr ); } );
            }

            // ---- Timestamps: a begin/end pair per pass per repeat ----------------------------------------------
            const uint32_t        queryCount = static_cast<uint32_t>( passes.size() ) * kRepeats * 2;
            VkQueryPoolCreateInfo queryInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            queryInfo.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = queryCount;
            VkQueryPool queries  = VK_NULL_HANDLE;
            if ( const VkResult r = vkCreateQueryPool( vk, &queryInfo, nullptr, &queries ); r != VK_SUCCESS )
                return Fail( std::format( "vkCreateQueryPool returned {}", static_cast<int>( r ) ) );
            cleanup.Add( [vk, queries] { vkDestroyQueryPool( vk, queries, nullptr ); } );

            // ---- Record: one command buffer, one submit, one wait ----------------------------------------------
            auto& commands = CommandBufferAllocator::GetInstance();
            auto  cmdOr    = commands.RT_AllocateCommandBufferGraphic( true );
            if ( !cmdOr.IsSuccess() )
                return Fail( std::format( "no command buffer: {}", cmdOr.GetError() ) );
            const VkCommandBuffer cmd = cmdOr.GetValue();

            vkCmdResetQueryPool( cmd, queries, 0, queryCount );
            VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            const auto serialise  = [&]
            {
                vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                                      nullptr );
            };
            uint32_t query = 0;
            for ( uint32_t repeat = 0; repeat < kRepeats; ++repeat )
            {
                for ( const Pass& pass : passes )
                {
                    vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass.Pipeline );
                    serialise();
                    vkCmdWriteTimestamp( cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, query++ );
                    for ( uint32_t d = 0; d < kDispatchesPerPass; ++d )
                    {
                        // Alternate A->B and B->A so each copy reads what the previous one wrote.
                        vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                                                 &sets[d % 2], 0, nullptr );
                        vkCmdDispatch( cmd, pass.Groups, 1, 1 );
                        serialise();
                    }
                    vkCmdWriteTimestamp( cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, query++ );
                }
            }
            if ( const VkResult r = vkEndCommandBuffer( cmd ); r != VK_SUCCESS )
                return Fail( std::format( "vkEndCommandBuffer returned {}", static_cast<int>( r ) ) );

            auto flushed = commands.RT_FlushCommandBufferGraphic( cmd );
            if ( !flushed.IsSuccess() )
                return Fail( std::format( "the submit failed: {}", flushed.GetError() ) );
            if ( flushed.GetValue() != VK_SUCCESS )
                return Fail( std::format( "the submit returned {}", static_cast<int>( flushed.GetValue() ) ) );

            // The flush waited on the fence, so every timestamp is written: no WAIT_BIT needed, but a missing
            // one is still an error (availability), never a zero.
            std::vector<uint64_t> ticks( queryCount * 2 );
            if ( const VkResult r = vkGetQueryPoolResults(
                      vk, queries, 0, queryCount, ticks.size() * sizeof( uint64_t ), ticks.data(),
                      2 * sizeof( uint64_t ), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );
                 r != VK_SUCCESS )
                return Fail( std::format( "vkGetQueryPoolResults returned {}", static_cast<int>( r ) ) );

            // ---- Best of kRepeats per pass ---------------------------------------------------------------------
            std::vector<Common::Scalability::BenchmarkPass> timed;
            for ( std::size_t p = 0; p < passes.size(); ++p )
            {
                double best = 0.0;
                for ( uint32_t repeat = 0; repeat < kRepeats; ++repeat )
                {
                    const std::size_t begin = ( repeat * passes.size() + p ) * 2;
                    if ( ticks[begin * 2 + 1] == 0 || ticks[( begin + 1 ) * 2 + 1] == 0 )
                        return Fail( std::format( "pass '{}' repeat {}: a timestamp is unavailable after the wait",
                                                  passes[p].Name, repeat ) );
                    const uint64_t start = ticks[begin * 2], end = ticks[( begin + 1 ) * 2];
                    if ( end <= start )
                        return Fail( std::format( "pass '{}' repeat {}: end tick {} is not after start {}",
                                                  passes[p].Name, repeat, end, start ) );
                    const double ms = double( end - start ) * caps.TimestampPeriodNs * 1.0e-6;
                    best            = repeat == 0 ? ms : std::min( best, ms );
                }
                timed.push_back( { passes[p].Name, best, passes[p].WorkPerDispatch * kDispatchesPerPass } );
            }

            auto index = Common::Scalability::GpuPerfIndex( timed, kReference );
            if ( !index.IsSuccess() )
                return Fail( index.GetError() );
            result.GpuPerfIndex = index.GetValue();
            result.Timed        = true;
            result.Passes       = std::move( timed );
            return Common::MakeSuccess( std::move( result ) );
        }
    } // namespace
} // namespace Desert::Graphic::API::Vulkan

namespace Desert::Engine
{
    std::unique_ptr<GpuBenchmark> GpuBenchmark::Create()
    {
        // Vulkan is the engine's only backend; a second one chooses here by the active device's API.
        return std::make_unique<Graphic::API::Vulkan::VulkanGpuBenchmark>();
    }
} // namespace Desert::Engine
