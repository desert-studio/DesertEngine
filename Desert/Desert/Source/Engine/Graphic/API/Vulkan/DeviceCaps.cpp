#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>
#include <Engine/Core/ShaderCompiler/BindlessSetIndex.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include <Common/Json/Template.hpp>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        using C = Capability;
        using N = CapabilityNeed;

        constexpr uint32_t V10 = VK_API_VERSION_1_0;
        constexpr uint32_t V11 = VK_API_VERSION_1_1;
        constexpr uint32_t V12 = VK_API_VERSION_1_2;
        constexpr uint32_t V13 = VK_API_VERSION_1_3;

        // THE TABLE. Order matters twice: it is the enum's order (checked below), and it is the probe
        // order, so every row listed in DependsOn must come before the row that depends on it.
        constexpr std::array<CapabilitySpec, kCapabilityCount> kTable = { {
             { C::TessellationShader, "tessellationShader", N::Required, V10, {} },
             // Every view pipeline blends per attachment: an integer target never blends and a slot the fragment
             // stage does not write is masked (ShaderReflection::BuildColorBlendAttachments), so pAttachments
             // differ (VUID-VkPipelineColorBlendStateCreateInfo-pAttachments-00605). Universal on desktop and
             // MoltenVK.
             { C::IndependentBlend, "independentBlend", N::Required, V10, {} },
             // The reflection captures live in one cube array (samplerCubeArray / imageCubeArray). Universal on
             // desktop and MoltenVK.
             { C::ImageCubeArray, "imageCubeArray", N::Required, V10, {} },
             // The sun's virtual shadow map draws its casters with depth clamp (UE VSM: casters behind a clipmap
             // level's depth range are flattened onto its near plane instead of clipped away, so a mountain
             // between the sun and the level still shadows it). Universal on desktop and MoltenVK.
             { C::DepthClamp, "depthClamp", N::Required, V10, {} },
             // The sun's virtual shadow map rasterizes its casters with no attachment: the fragment stage
             // atomic-maxes its depth into the physical pool, a storage image (Common/VirtualShadowRaster.glslh;
             // UE ShadowDepthPixelShader.usf VIRTUAL_TEXTURE_TARGET). Optional, as in UE, where a platform without
             // the feature has no virtual shadow maps and keeps the shadow maps: ShadowMethod::VirtualMaps needs
             // it (NeedsFragmentStores) and both method ladders lower it to CascadedMaps by name where it is
             // absent. Desktop GPUs and MoltenVK on Apple GPUs carry it.
             { C::FragmentStoresAndAtomics, "fragmentStoresAndAtomics", N::Optional, V10, {} },
             { C::Swapchain, "swapchain", N::Required, 0, VK_KHR_SWAPCHAIN_EXTENSION_NAME },
             // MoltenVK has no wide lines; the debug-line path clamps the width instead.
             { C::WideLines, "wideLines", N::Optional, V10, {} },
             { C::FillModeNonSolid, "fillModeNonSolid", N::Optional, V10, {} },
             // GPU-DRIVEN (GpuDriven/IndirectDrawSubmission.hpp): one vkCmdDrawIndexedIndirect over an
             // IndirectGroup's bins. Optional (decision 10-27, no device refused): without it the group's records
             // go one call each.
             { C::MultiDrawIndirect, "multiDrawIndirect", N::Optional, V10, {} },
             { C::SamplerAnisotropy, "samplerAnisotropy", N::Optional, V10, {} },
             { C::TextureCompressionBC, "textureCompressionBC", N::Optional, V10, {} },
             // A storage image declared without a format and written (OpTypeImage Unknown + OpImageWrite): the FSR
             // upscaler's output binding, whose format is the application's (FidelityFX
             // ffx_fsr3upscaler_callbacks_glsl.h rw_upscaled_output). Without it FSR is refused by name
             // (FsrDeviceSupport); nothing else reads it.
             { C::StorageImageWriteWithoutFormat, "shaderStorageImageWriteWithoutFormat", N::Optional, V10, {} },
             // Not a capability the engine uses but a spec rule: an implementation that advertises it
             // (MoltenVK) must have it enabled. "Optional" gives exactly that: enabled iff present.
             { C::PortabilitySubset, "portability_subset", N::Optional, 0, "VK_KHR_portability_subset" },
             { C::MemoryBudget, "memory_budget", N::Optional, 0, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME },
             // Required: one-off uploads signal a timeline that the async-compute pipe and the compute/transfer
             // one-offs wait on the GPU (CommandBufferAllocator::NewestUploadSync). Core in 1.2, the minimum.
             { C::TimelineSemaphore, "timelineSemaphore", N::Required, V12,
               VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME },
             // Required: the bindless texture table (runtime array of sampled images, update-after-bind,
             // partially bound, variable count, non-uniform indexing). Core in 1.2; every desktop GPU and
             // MoltenVK (argument buffers) carries it, and there is no non-bindless second path.
             { C::DescriptorIndexing, "descriptorIndexing", N::Required, V12,
               VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME },
             // Optional, its own row, independent of ray tracing: mesh buffers carry SHADER_DEVICE_ADDRESS under
             // it (MeshBufferUsage.hpp) and the GI emissive light list names emitter triangles by address for both
             // tracers; without it the list cannot name an emitter and the surface cache reports that error
             // (REMAINDER GI-ALL-LIGHTS-3: no desktop 1.2 device lacks it, the branch is not built yet).
             { C::BufferDeviceAddress, "bufferDeviceAddress", N::Optional, V12,
               VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME },
             // Below 1.2 the extension needs depth_stencil_resolve and create_renderpass2 as extensions.
             { C::DynamicRendering, "dynamicRendering", N::Available, V13, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
               V12 },
             { C::Synchronization2, "synchronization2", N::Available, V13,
               VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME },
             { C::DeferredHostOperations, "deferred_host_operations", N::Optional, 0,
               VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, V12 },
             // Ray tracing: below 1.2 its dependencies (spirv_1_4, buffer_device_address, ...) are
             // extensions of their own, so the extension route starts at 1.2.
             { C::AccelerationStructure,
               "acceleration_structure",
               N::Optional,
               0,
               VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
               V12,
               { C::BufferDeviceAddress, C::DescriptorIndexing, C::DeferredHostOperations } },
             { C::RayQuery,
               "ray_query",
               N::Optional,
               0,
               VK_KHR_RAY_QUERY_EXTENSION_NAME,
               V12,
               { C::AccelerationStructure, C::Count, C::Count } },
             { C::RayTracingPipeline,
               "ray_tracing_pipeline",
               N::Optional,
               0,
               VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
               V12,
               { C::AccelerationStructure, C::Count, C::Count } },
             // The NN layer (NN-RUNTIME; NeuralComputeCaps.hpp). Optional: SelectNeuralKernelPath chooses the
             // coopmat fp16 / fp16 / fp32 kernels from them, and every device runs fp32. The 8-bit pair is
             // recorded for int8 weights, which no kernel uses yet.
             { C::StorageBuffer16BitAccess, "storageBuffer16BitAccess", N::Optional, V11,
               VK_KHR_16BIT_STORAGE_EXTENSION_NAME },
             { C::ShaderFloat16, "shaderFloat16", N::Optional, V12, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME },
             { C::ShaderInt8, "shaderInt8", N::Available, V12, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME },
             { C::StorageBuffer8BitAccess, "storageBuffer8BitAccess", N::Available, V12,
               VK_KHR_8BIT_STORAGE_EXTENSION_NAME },
             // subgroupSizeControl + computeFullSubgroups: a kernel that names its subgroup size (a coopmat tile
             // is one subgroup) and needs every lane of it. The size range is read with it (DeviceCapsProbe).
             { C::SubgroupSizeControl, "subgroupSizeControl", N::Available, V13,
               VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME },
             // Tensor cores / WMMA / XMX. Never core in 1.3. Its shapes are listed by the probe.
             { C::CooperativeMatrix, "cooperative_matrix", N::Optional, 0,
               VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME },
             // Every cooperative-matrix SPIR-V module glslang emits is declared under the Vulkan memory model
             // (Shaders/Programs/Compute/NeuralFusedMlp.shader), so the coopmat path needs it. Optional: only that
             // path reads it (SelectNeuralKernelPath).
             { C::VulkanMemoryModel, "vulkanMemoryModel", N::Optional, V12,
               VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME },
             // Memory and timeline semaphores exported to another API on the same GPU (OIDN on CUDA / HIP shares
             // the path tracer's buffers instead of a host copy). VK_KHR_external_memory / _semaphore are core in
             // 1.1; the handle-type extensions are not core anywhere. Optional: only PathTracing.Denoiser OIDN
             // reads them (CapabilityCatalog::GpuSharing), and it is refused by name without them.
             { C::ExternalMemoryWin32, "external_memory_win32", N::Optional, 0, "VK_KHR_external_memory_win32" },
             { C::ExternalSemaphoreWin32, "external_semaphore_win32", N::Optional, 0,
               "VK_KHR_external_semaphore_win32" },
             { C::ExternalMemoryFd, "external_memory_fd", N::Optional, 0, "VK_KHR_external_memory_fd" },
             { C::ExternalSemaphoreFd, "external_semaphore_fd", N::Optional, 0, "VK_KHR_external_semaphore_fd" },
        } };

        // Row i describes enum value i, so SpecOf() and DeviceCaps::Rows index the same thing; and every
        // dependency precedes its dependant, so the probe (table order) has settled it first.
        constexpr bool TableIsWellOrdered()
        {
            for ( std::size_t i = 0; i < kCapabilityCount; ++i )
            {
                if ( static_cast<std::size_t>( kTable[i].Id ) != i )
                    return false;
                for ( const Capability dependency : kTable[i].DependsOn )
                    if ( dependency != Capability::Count && static_cast<std::size_t>( dependency ) >= i )
                        return false;
            }
            return true;
        }
        static_assert( TableIsWellOrdered(), "CapabilityTable must follow the enum, dependencies first" );

        uint32_t MajorMinor( uint32_t version )
        {
            return VK_MAKE_API_VERSION( 0, VK_API_VERSION_MAJOR( version ), VK_API_VERSION_MINOR( version ), 0 );
        }
    } // namespace

    const std::array<CapabilitySpec, kCapabilityCount>& CapabilityTable()
    {
        return kTable;
    }

    const CapabilitySpec& SpecOf( Capability capability )
    {
        return kTable[static_cast<std::size_t>( capability )];
    }

    uint32_t UsedApiVersion( uint32_t deviceApiVersion )
    {
        return std::min( MajorMinor( deviceApiVersion ), kMaximumApiVersion );
    }

    std::string FormatApiVersion( uint32_t version )
    {
        return std::format( "{}.{}.{}", VK_API_VERSION_MAJOR( version ), VK_API_VERSION_MINOR( version ),
                            VK_API_VERSION_PATCH( version ) );
    }

    DeviceCaps PlanRoutes( std::string deviceName, std::string driver, uint32_t deviceApiVersion,
                           const std::function<bool( std::string_view )>& hasExtension )
    {
        DeviceCaps caps;
        caps.DeviceName       = std::move( deviceName );
        caps.Driver           = std::move( driver );
        caps.DeviceApiVersion = deviceApiVersion;
        caps.UsedApiVersion   = UsedApiVersion( deviceApiVersion );

        for ( std::size_t i = 0; i < kCapabilityCount; ++i )
        {
            const CapabilitySpec& spec  = kTable[i];
            CapabilityRoute       route = CapabilityRoute::Unreachable;
            // Plans against the USED version: a 1.4 device is driven as 1.3, so a 1.4-core row would still
            // go through its extension — the engine never relies on a version it does not target.
            if ( spec.CoreSince != 0 && caps.UsedApiVersion >= spec.CoreSince )
                route = CapabilityRoute::Core;
            else if ( !spec.Extension.empty() && caps.UsedApiVersion >= spec.ExtensionMinimumApi &&
                      hasExtension( spec.Extension ) )
                route = CapabilityRoute::Extension;
            caps.Rows[i].Route = route;
        }
        return caps;
    }

    bool IsRayTracingCapability( Capability capability )
    {
        return capability == Capability::AccelerationStructure || capability == Capability::RayQuery ||
               capability == Capability::RayTracingPipeline;
    }

    void HideRayTracing( DeviceCaps& caps )
    {
        for ( std::size_t i = 0; i < kCapabilityCount; ++i )
        {
            if ( IsRayTracingCapability( static_cast<Capability>( i ) ) )
                caps.Rows[i] = CapabilityRow{ .Route = CapabilityRoute::Unreachable, .Present = false };
        }
        caps.RayTracingHidden = true;
    }

    bool DependenciesPresent( const DeviceCaps& caps, Capability capability )
    {
        for ( const Capability dependency : SpecOf( capability ).DependsOn )
        {
            if ( dependency != Capability::Count && !caps.Has( dependency ) )
                return false;
        }
        return true;
    }

    Common::BoolResultStr CheckRequired( const DeviceCaps& caps )
    {
        std::vector<std::string> missing;
        if ( MajorMinor( caps.DeviceApiVersion ) < kMinimumDeviceApiVersion )
        {
            missing.push_back( std::format( "Vulkan {} (device reports {})",
                                            FormatApiVersion( kMinimumDeviceApiVersion ),
                                            FormatApiVersion( caps.DeviceApiVersion ) ) );
        }
        for ( std::size_t i = 0; i < kCapabilityCount; ++i )
        {
            const CapabilitySpec& spec = kTable[i];
            if ( spec.Need != CapabilityNeed::Required || caps.Rows[i].Present )
                continue;
            missing.push_back( spec.Extension.empty() ? std::format( "{}", spec.Name )
                                                      : std::format( "{} ({})", spec.Name, spec.Extension ) );
        }
        if ( caps.Has( Capability::DescriptorIndexing ) )
        {
            const DescriptorIndexingLimits& limits = caps.DescriptorIndexing;
            // BindlessCapacityCeiling's rule: the table keeps at most half the ceiling for itself.
            if ( limits.SampledImageCeiling() < 2 * kBindlessInitialTextureCapacity )
            {
                missing.push_back( std::format( "{} update-after-bind sampled images (per stage {}, per set {})",
                                        2 * kBindlessInitialTextureCapacity,
                                        limits.MaxPerStageUpdateAfterBindSampledImages,
                                        limits.MaxSetUpdateAfterBindSampledImages ) );
            }
            if ( limits.SamplerCeiling() < ::Desert::Core::kBindlessSamplerStateCount )
            {
                missing.push_back( std::format( "{} update-after-bind samplers (per stage {}, per set {})",
                                        ::Desert::Core::kBindlessSamplerStateCount,
                                        limits.MaxPerStageUpdateAfterBindSamplers,
                                        limits.MaxSetUpdateAfterBindSamplers ) );
            }
        }
        if ( missing.empty() )
            return Common::MakeSuccess( true );
        return Common::MakeError<bool>(
             std::format( "the Vulkan device '{}' ({}) lacks what this engine requires: {}", caps.DeviceName,
                          caps.Driver, Common::Text::Join( missing, ", " ) ) );
    }

    std::string FormatCapsTable( const DeviceCaps& caps )
    {
        std::string line =
             std::format( "device {} | driver {} | Vulkan {} (used {}) |", caps.DeviceName, caps.Driver,
                          FormatApiVersion( caps.DeviceApiVersion ), FormatApiVersion( caps.UsedApiVersion ) );
        for ( std::size_t i = 0; i < kCapabilityCount; ++i )
        {
            const CapabilitySpec& spec = kTable[i];
            const CapabilityRow&  row  = caps.Rows[i];
            const char* how = caps.RayTracingHidden && IsRayTracingCapability( static_cast<Capability>( i ) )
                                   ? "hidden"
                              : !row.Present                            ? "no"
                              : row.Route == CapabilityRoute::Extension ? "ext"
                              : spec.CoreSince == VK_API_VERSION_1_0    ? "1.0"
                              : spec.CoreSince == VK_API_VERSION_1_1    ? "core1.1"
                              : spec.CoreSince == VK_API_VERSION_1_2    ? "core1.2"
                                                                        : "core1.3";
            const char*           need = spec.Need == CapabilityNeed::Required ? "!" : "";
            line += std::format( " {}{}={}", need, spec.Name, how );
        }
        return line;
    }

    std::string FormatDescriptorLimits( const DeviceCaps& caps )
    {
        const PerStageDescriptorLimits& stage = caps.PerStage;
        const DescriptorIndexingLimits& uab   = caps.DescriptorIndexing;
        // descriptorBindingSampledImageUpdateAfterBind is enabled with the descriptorIndexing row, never alone.
        return std::format(
             "per-stage limits: samplers {}, sampled images {}, uniform buffers {}, storage buffers "
             "{}, storage images {}, resources {} | update-after-bind (sampledImageUpdateAfterBind="
             "{}): per stage samplers {}, sampled images {}, resources {}; per set samplers {}, "
             "sampled images {}",
             stage.MaxSamplers, stage.MaxSampledImages, stage.MaxUniformBuffers, stage.MaxStorageBuffers,
             stage.MaxStorageImages, stage.MaxResources, caps.Has( Capability::DescriptorIndexing ) ? "yes" : "no",
             uab.MaxPerStageUpdateAfterBindSamplers, uab.MaxPerStageUpdateAfterBindSampledImages,
             uab.MaxPerStageUpdateAfterBindResources, uab.MaxSetUpdateAfterBindSamplers,
             uab.MaxSetUpdateAfterBindSampledImages );
    }
} // namespace Desert::Graphic::API::Vulkan
