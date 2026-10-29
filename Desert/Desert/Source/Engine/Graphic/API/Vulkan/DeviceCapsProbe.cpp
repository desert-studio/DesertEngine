#include <Engine/Graphic/API/Vulkan/DeviceCapsProbe.hpp>

#include <vk-bootstrap/VkBootstrap.h>

#include <format>
#include <optional>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        bool EnableCore10( vkb::PhysicalDevice& device, VkBool32 VkPhysicalDeviceFeatures::*feature )
        {
            VkPhysicalDeviceFeatures wanted{};
            wanted.*feature = VK_TRUE;
            return device.enable_features_if_present( wanted );
        }

        // The feature struct goes first: an extension is only worth enabling when the device also reports
        // the feature bit behind it, and enable_extension_features_if_present asks exactly that.
        // Two rows may share one extension (shaderFloat16 and shaderInt8: VK_KHR_shader_float16_int8);
        // vk-bootstrap lists an extension once per call, and vkCreateDevice must not see it twice.
        bool EnableExtensionOnce( vkb::PhysicalDevice& device, std::string_view extension )
        {
            for ( const std::string& enabled : device.get_extensions() )
                if ( enabled == extension )
                    return true;
            return device.enable_extension_if_present( std::string( extension ).c_str() );
        }

        template <typename Features>
        bool EnableExtensionFeatures( vkb::PhysicalDevice& device, const Features& wanted,
                                      const CapabilitySpec& spec )
        {
            if ( !device.enable_extension_features_if_present( wanted ) )
                return false;
            return EnableExtensionOnce( device, spec.Extension );
        }

        // One capability on its planned route: asked and, when present, enabled. The feature bits chosen
        // for DescriptorIndexing are the bindless table's subset (runtime arrays, partially bound, variable
        // count, non-uniform sampled-image indexing, sampled-image update-after-bind, update-unused-while-
        // pending: the table is written while earlier frames are still in flight), not the whole extension.
        bool ProbeAndEnable( vkb::PhysicalDevice& device, Capability capability, CapabilityRoute route )
        {
            const CapabilitySpec& spec = SpecOf( capability );
            const bool            core = route == CapabilityRoute::Core;
            switch ( capability )
            {
                case Capability::TessellationShader:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::tessellationShader );
                case Capability::IndependentBlend:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::independentBlend );
                case Capability::ImageCubeArray:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::imageCubeArray );
                case Capability::DepthClamp:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::depthClamp );
                case Capability::FragmentStoresAndAtomics:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::fragmentStoresAndAtomics );
                case Capability::WideLines:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::wideLines );
                case Capability::FillModeNonSolid:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::fillModeNonSolid );
                case Capability::MultiDrawIndirect:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::multiDrawIndirect );
                case Capability::SamplerAnisotropy:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::samplerAnisotropy );
                case Capability::TextureCompressionBC:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::textureCompressionBC );
                case Capability::StorageImageWriteWithoutFormat:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::shaderStorageImageWriteWithoutFormat );

                case Capability::Swapchain:
                case Capability::PortabilitySubset:
                case Capability::MemoryBudget:
                case Capability::DeferredHostOperations:
                    return device.enable_extension_if_present( std::string( spec.Extension ).c_str() );

                case Capability::TimelineSemaphore:
                {
                    if ( core )
                    {
                        VkPhysicalDeviceVulkan12Features f{
                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
                        f.timelineSemaphore = VK_TRUE;
                        return device.enable_extension_features_if_present( f );
                    }
                    VkPhysicalDeviceTimelineSemaphoreFeatures f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
                    f.timelineSemaphore = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::DescriptorIndexing:
                {
                    if ( core )
                    {
                        VkPhysicalDeviceVulkan12Features f{
                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
                        f.runtimeDescriptorArray                       = VK_TRUE;
                        f.descriptorBindingPartiallyBound              = VK_TRUE;
                        f.descriptorBindingVariableDescriptorCount     = VK_TRUE;
                        f.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
                        f.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
                        f.descriptorBindingUpdateUnusedWhilePending    = VK_TRUE;
                        return device.enable_extension_features_if_present( f );
                    }
                    VkPhysicalDeviceDescriptorIndexingFeatures f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
                    f.runtimeDescriptorArray                       = VK_TRUE;
                    f.descriptorBindingPartiallyBound              = VK_TRUE;
                    f.descriptorBindingVariableDescriptorCount     = VK_TRUE;
                    f.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
                    f.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
                    f.descriptorBindingUpdateUnusedWhilePending    = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::BufferDeviceAddress:
                {
                    if ( core )
                    {
                        VkPhysicalDeviceVulkan12Features f{
                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
                        f.bufferDeviceAddress = VK_TRUE;
                        return device.enable_extension_features_if_present( f );
                    }
                    VkPhysicalDeviceBufferDeviceAddressFeatures f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
                    f.bufferDeviceAddress = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::DynamicRendering:
                {
                    if ( core )
                    {
                        VkPhysicalDeviceVulkan13Features f{
                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
                        f.dynamicRendering = VK_TRUE;
                        return device.enable_extension_features_if_present( f );
                    }
                    VkPhysicalDeviceDynamicRenderingFeatures f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
                    f.dynamicRendering = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::Synchronization2:
                {
                    if ( core )
                    {
                        VkPhysicalDeviceVulkan13Features f{
                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
                        f.synchronization2 = VK_TRUE;
                        return device.enable_extension_features_if_present( f );
                    }
                    VkPhysicalDeviceSynchronization2Features f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
                    f.synchronization2 = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::AccelerationStructure:
                {
                    VkPhysicalDeviceAccelerationStructureFeaturesKHR f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
                    f.accelerationStructure = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::RayQuery:
                {
                    VkPhysicalDeviceRayQueryFeaturesKHR f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
                    f.rayQuery = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::RayTracingPipeline:
                {
                    VkPhysicalDeviceRayTracingPipelineFeaturesKHR f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
                    f.rayTracingPipeline = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                // The NN rows. Core routes go through the VkPhysicalDeviceVulkan1xFeatures structs, never the
                // per-feature ones: both in one chain is VUID-VkDeviceCreateInfo-pNext-02829/02830/06532, and the
                // engine and the vendor runtimes already chain VkPhysicalDeviceVulkan12/13Features.
                case Capability::StorageBuffer16BitAccess:
                {
                    if ( core )
                    {
                        // Vulkan11Features exists from 1.2; a 1.1 device answers no here and is refused by
                        // CheckRequired (kMinimumDeviceApiVersion) anyway.
                        VkPhysicalDeviceVulkan11Features f{
                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
                        f.storageBuffer16BitAccess = VK_TRUE;
                        return device.enable_extension_features_if_present( f );
                    }
                    VkPhysicalDevice16BitStorageFeatures f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES };
                    f.storageBuffer16BitAccess = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::ShaderFloat16:
                case Capability::ShaderInt8:
                {
                    const bool float16 = capability == Capability::ShaderFloat16;
                    if ( core )
                    {
                        VkPhysicalDeviceVulkan12Features f{
                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
                        if ( float16 )
                            f.shaderFloat16 = VK_TRUE;
                        else
                            f.shaderInt8 = VK_TRUE;
                        return device.enable_extension_features_if_present( f );
                    }
                    VkPhysicalDeviceShaderFloat16Int8Features f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES };
                    if ( float16 )
                        f.shaderFloat16 = VK_TRUE;
                    else
                        f.shaderInt8 = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::StorageBuffer8BitAccess:
                {
                    if ( core )
                    {
                        VkPhysicalDeviceVulkan12Features f{
                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
                        f.storageBuffer8BitAccess = VK_TRUE;
                        return device.enable_extension_features_if_present( f );
                    }
                    VkPhysicalDevice8BitStorageFeatures f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES };
                    f.storageBuffer8BitAccess = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::SubgroupSizeControl:
                {
                    if ( core )
                    {
                        VkPhysicalDeviceVulkan13Features f{
                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
                        f.subgroupSizeControl  = VK_TRUE;
                        f.computeFullSubgroups = VK_TRUE;
                        return device.enable_extension_features_if_present( f );
                    }
                    VkPhysicalDeviceSubgroupSizeControlFeatures f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES };
                    f.subgroupSizeControl  = VK_TRUE;
                    f.computeFullSubgroups = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::VulkanMemoryModel:
                {
                    if ( core )
                    {
                        VkPhysicalDeviceVulkan12Features f{
                             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
                        f.vulkanMemoryModel = VK_TRUE;
                        return device.enable_extension_features_if_present( f );
                    }
                    VkPhysicalDeviceVulkanMemoryModelFeatures f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES };
                    f.vulkanMemoryModel = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::CooperativeMatrix:
                {
                    VkPhysicalDeviceCooperativeMatrixFeaturesKHR f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
                    f.cooperativeMatrix = VK_TRUE;
                    return EnableExtensionFeatures( device, f, spec );
                }
                case Capability::Count:
                    break;
            }
            return false;
        }

        // driverName/driverInfo are the only human-readable driver identity (driverVersion's encoding
        // is vendor-specific); they need 1.2 core or VK_KHR_driver_properties.
        std::string DriverOf( const vkb::PhysicalDevice& device )
        {
            const bool known = UsedApiVersion( device.properties.apiVersion ) >= VK_API_VERSION_1_2 ||
                               device.is_extension_present( VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME );
            if ( !known )
                return std::format( "driverVersion 0x{:08x}", device.properties.driverVersion );
            VkPhysicalDeviceDriverProperties driver{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
            VkPhysicalDeviceProperties2      properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver };
            vkGetPhysicalDeviceProperties2( device.physical_device, &properties );
            // The fixed-size arrays hold NUL-terminated text; formatted as arrays they print all 256 bytes.
            return std::format( "{} {}", std::string( driver.driverName ), std::string( driver.driverInfo ) );
        }

        // The bindless table's ceilings. The properties struct is core 1.2 and the extension's own on 1.1
        // with one layout, so a device on either route answers through the same chain.
        DescriptorIndexingLimits DescriptorIndexingLimitsOf( const vkb::PhysicalDevice& device )
        {
            VkPhysicalDeviceDescriptorIndexingProperties indexing{
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES };
            VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &indexing };
            vkGetPhysicalDeviceProperties2( device.physical_device, &properties );
            DescriptorIndexingLimits limits;
            limits.MaxPerStageUpdateAfterBindSampledImages =
                 indexing.maxPerStageDescriptorUpdateAfterBindSampledImages;
            limits.MaxSetUpdateAfterBindSampledImages  = indexing.maxDescriptorSetUpdateAfterBindSampledImages;
            limits.MaxPerStageUpdateAfterBindSamplers  = indexing.maxPerStageDescriptorUpdateAfterBindSamplers;
            limits.MaxSetUpdateAfterBindSamplers       = indexing.maxDescriptorSetUpdateAfterBindSamplers;
            limits.MaxPerStageUpdateAfterBindResources = indexing.maxPerStageUpdateAfterBindResources;
            return limits;
        }

        PerStageDescriptorLimits PerStageDescriptorLimitsOf( const vkb::PhysicalDevice& device )
        {
            const VkPhysicalDeviceLimits& limits = device.properties.limits;
            return PerStageDescriptorLimits{ .MaxSamplers       = limits.maxPerStageDescriptorSamplers,
                                             .MaxSampledImages  = limits.maxPerStageDescriptorSampledImages,
                                             .MaxUniformBuffers = limits.maxPerStageDescriptorUniformBuffers,
                                             .MaxStorageBuffers = limits.maxPerStageDescriptorStorageBuffers,
                                             .MaxStorageImages  = limits.maxPerStageDescriptorStorageImages,
                                             .MaxResources      = limits.maxPerStageResources };
        }

        ::Desert::Engine::NeuralScalar ScalarOf( VkComponentTypeKHR type )
        {
            switch ( type )
            {
                case VK_COMPONENT_TYPE_FLOAT16_KHR:
                    return ::Desert::Engine::NeuralScalar::Float16;
                case VK_COMPONENT_TYPE_FLOAT32_KHR:
                    return ::Desert::Engine::NeuralScalar::Float32;
                case VK_COMPONENT_TYPE_FLOAT64_KHR:
                    return ::Desert::Engine::NeuralScalar::Float64;
                case VK_COMPONENT_TYPE_SINT8_KHR:
                    return ::Desert::Engine::NeuralScalar::Sint8;
                case VK_COMPONENT_TYPE_UINT8_KHR:
                    return ::Desert::Engine::NeuralScalar::Uint8;
                case VK_COMPONENT_TYPE_SINT32_KHR:
                    return ::Desert::Engine::NeuralScalar::Sint32;
                case VK_COMPONENT_TYPE_UINT32_KHR:
                    return ::Desert::Engine::NeuralScalar::Uint32;
                default:
                    // bf16 (VK_KHR_shader_bfloat16) and the 16/64-bit integers: recorded as Other, never dropped.
                    return ::Desert::Engine::NeuralScalar::Other;
            }
        }

        // The NN layer's half of the probe, after every row is settled: the features from the rows, the limits
        // and subgroup properties, and the cooperative-matrix shapes. Then the kernel path, once.
        ::Desert::Engine::NeuralComputeCaps
        NeuralComputeCapsOf( VkInstance instance, const vkb::PhysicalDevice& device, const DeviceCaps& caps )
        {
            ::Desert::Engine::NeuralComputeCaps neural;
            neural.ShaderFloat16              = caps.Has( Capability::ShaderFloat16 );
            neural.ShaderInt8                 = caps.Has( Capability::ShaderInt8 );
            neural.StorageBuffer16BitAccess   = caps.Has( Capability::StorageBuffer16BitAccess );
            neural.StorageBuffer8BitAccess    = caps.Has( Capability::StorageBuffer8BitAccess );
            neural.SubgroupSizeControl        = caps.Has( Capability::SubgroupSizeControl );
            neural.CooperativeMatrix          = caps.Has( Capability::CooperativeMatrix );
            neural.VulkanMemoryModel          = caps.Has( Capability::VulkanMemoryModel );
            neural.MaxPushConstantsSize       = device.properties.limits.maxPushConstantsSize;
            neural.MaxComputeSharedMemorySize = device.properties.limits.maxComputeSharedMemorySize;

            // Core 1.1 subgroup properties; the size range only where size control is enabled (the struct has
            // one layout on the extension route and in core 1.3).
            VkPhysicalDeviceSubgroupSizeControlProperties sizeControl{
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES };
            VkPhysicalDeviceCooperativeMatrixPropertiesKHR coopmat{
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR };
            VkPhysicalDeviceSubgroupProperties subgroup{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
            void**                             tail = &subgroup.pNext;
            if ( neural.SubgroupSizeControl )
            {
                *tail = &sizeControl;
                tail  = &sizeControl.pNext;
            }
            if ( neural.CooperativeMatrix )
                *tail = &coopmat;
            VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup };
            vkGetPhysicalDeviceProperties2( device.physical_device, &properties );
            neural.SubgroupSize = subgroup.subgroupSize;
            if ( neural.SubgroupSizeControl )
            {
                neural.MinSubgroupSize = sizeControl.minSubgroupSize;
                neural.MaxSubgroupSize = sizeControl.maxSubgroupSize;
                neural.ComputeSubgroupSizeRequirable =
                     ( sizeControl.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT ) != 0;
            }

            if ( neural.CooperativeMatrix )
            {
                neural.CooperativeMatrixInCompute =
                     ( coopmat.cooperativeMatrixSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT ) != 0;
                // An instance-level entry point of an enabled extension. One that does not resolve leaves the
                // shape list empty, and SelectNeuralKernelPath names the missing shape instead of guessing one.
                const auto listShapes = reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
                     vkGetInstanceProcAddr( instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR" ) );
                uint32_t count = 0;
                if ( listShapes != nullptr && listShapes( device.physical_device, &count, nullptr ) == VK_SUCCESS )
                {
                    VkCooperativeMatrixPropertiesKHR blank{};
                    blank.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
                    std::vector<VkCooperativeMatrixPropertiesKHR> shapes( count, blank );
                    if ( listShapes( device.physical_device, &count, shapes.data() ) >= VK_SUCCESS )
                    {
                        shapes.resize( count );
                        for ( const VkCooperativeMatrixPropertiesKHR& shape : shapes )
                            neural.CooperativeMatrixShapes.push_back( ::Desert::Engine::CooperativeMatrixShape{
                                 .M                      = shape.MSize,
                                 .N                      = shape.NSize,
                                 .K                      = shape.KSize,
                                 .A                      = ScalarOf( shape.AType ),
                                 .B                      = ScalarOf( shape.BType ),
                                 .C                      = ScalarOf( shape.CType ),
                                 .Result                 = ScalarOf( shape.ResultType ),
                                 .SaturatingAccumulation = shape.saturatingAccumulation == VK_TRUE,
                                 .SubgroupScope          = shape.scope == VK_SCOPE_SUBGROUP_KHR } );
                    }
                }
            }
            ::Desert::Engine::SelectNeuralKernelPath( neural );
            return neural;
        }

        ProbedDevice Probe( VkInstance instance, const vkb::PhysicalDevice& candidate,
                            const DeviceRestrictions& restrictions )
        {
            ProbedDevice probed;
            probed.Physical             = std::make_shared<vkb::PhysicalDevice>( candidate );
            vkb::PhysicalDevice& device = *probed.Physical;

            probed.Caps =
                 PlanRoutes( device.properties.deviceName, DriverOf( device ), device.properties.apiVersion,
                             [&device]( std::string_view extension )
                             { return device.is_extension_present( std::string( extension ).c_str() ); } );
            // Before the loop, so a hidden row is neither asked for nor enabled: the device built from this
            // object is the one a card without ray tracing would give.
            if ( restrictions.HideRayTracing )
                HideRayTracing( probed.Caps );

            // Table order: every dependency is settled before the row that needs it (static_assert in
            // DeviceCaps.cpp), and a row whose dependencies are absent is not even asked.
            for ( std::size_t i = 0; i < kCapabilityCount; ++i )
            {
                const auto     capability = static_cast<Capability>( i );
                CapabilityRow& row        = probed.Caps.Rows[i];
                row.Present               = row.Route != CapabilityRoute::Unreachable &&
                              DependenciesPresent( probed.Caps, capability ) &&
                              ProbeAndEnable( device, capability, row.Route );
            }
            if ( probed.Caps.Has( Capability::DescriptorIndexing ) )
                probed.Caps.DescriptorIndexing = DescriptorIndexingLimitsOf( device );
            probed.Caps.PerStage = PerStageDescriptorLimitsOf( device );
            probed.Caps.Neural   = NeuralComputeCapsOf( instance, device, probed.Caps );
            return probed;
        }

        int TypeRank( VkPhysicalDeviceType type )
        {
            switch ( type )
            {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    return 4;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    return 3;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                    return 2;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:
                    return 1;
                default:
                    return 0;
            }
        }
    } // namespace

    Common::ResultStr<ProbedDevice> SelectDevice( const vkb::Instance& instance, const DeviceRestrictions& restrictions )
    {
        // vk-bootstrap lists and describes the devices; the judgement is DeviceCaps'. So the selector is
        // told to reject nothing: version 1.0 (CheckRequired names a too-old version itself), no present
        // check (the surface is created later, by the swapchain), and no automatic portability_subset
        // or swapchain — both are table rows and are enabled by Probe, once.
        vkb::PhysicalDeviceSelector selector( instance );
        selector.set_minimum_version( 1, 0 )
             .require_present( false )
             .disable_portability_subset()
             .allow_any_gpu_device_type( true );
        auto listed = selector.select_devices( vkb::DeviceSelectionMode::partially_and_fully_suitable );
        if ( !listed )
            return Common::MakeFormattedError<ProbedDevice>( "vk-bootstrap could not list the Vulkan devices: {}",
                                                             listed.error().message() );
        if ( listed.value().empty() )
            return Common::MakeError<ProbedDevice>( "the Vulkan instance lists no physical device" );

        std::optional<ProbedDevice> best;
        std::string                 refusals;
        for ( const vkb::PhysicalDevice& candidate : listed.value() )
        {
            ProbedDevice probed = Probe( instance.instance, candidate, restrictions );
            if ( const auto accepted = CheckRequired( probed.Caps ); !accepted )
            {
                refusals += std::format( "\n  {}", accepted.GetError() );
                continue;
            }
            if ( !best || TypeRank( probed.Physical->properties.deviceType ) >
                               TypeRank( best->Physical->properties.deviceType ) )
                best = std::move( probed );
        }
        if ( !best )
            return Common::MakeFormattedError<ProbedDevice>( "no Vulkan device meets the required set:{}",
                                                             refusals );
        return Common::MakeSuccess( std::move( *best ) );
    }
} // namespace Desert::Graphic::API::Vulkan
