#include <Engine/Graphic/API/Vulkan/DeviceCapsProbe.hpp>

#include <vk-bootstrap/VkBootstrap.h>

#include <format>
#include <optional>

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
        template <typename Features>
        bool EnableExtensionFeatures( vkb::PhysicalDevice& device, const Features& wanted,
                                      const CapabilitySpec& spec )
        {
            if ( !device.enable_extension_features_if_present( wanted ) )
                return false;
            return device.enable_extension_if_present( std::string( spec.Extension ).c_str() );
        }

        // One capability on its planned route: asked and, when present, enabled. The feature bits chosen
        // for DescriptorIndexing are the bindless subset VKF2 will use (runtime arrays, partially bound,
        // variable count, non-uniform sampled-image indexing), not the whole extension.
        bool ProbeAndEnable( vkb::PhysicalDevice& device, Capability capability, CapabilityRoute route )
        {
            const CapabilitySpec& spec = SpecOf( capability );
            const bool            core = route == CapabilityRoute::Core;
            switch ( capability )
            {
                case Capability::TessellationShader:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::tessellationShader );
                case Capability::WideLines:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::wideLines );
                case Capability::FillModeNonSolid:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::fillModeNonSolid );
                case Capability::SamplerAnisotropy:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::samplerAnisotropy );
                case Capability::TextureCompressionBC:
                    return EnableCore10( device, &VkPhysicalDeviceFeatures::textureCompressionBC );

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
                        f.runtimeDescriptorArray                    = VK_TRUE;
                        f.descriptorBindingPartiallyBound           = VK_TRUE;
                        f.descriptorBindingVariableDescriptorCount  = VK_TRUE;
                        f.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
                        return device.enable_extension_features_if_present( f );
                    }
                    VkPhysicalDeviceDescriptorIndexingFeatures f{
                         VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
                    f.runtimeDescriptorArray                    = VK_TRUE;
                    f.descriptorBindingPartiallyBound           = VK_TRUE;
                    f.descriptorBindingVariableDescriptorCount  = VK_TRUE;
                    f.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
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

        ProbedDevice Probe( const vkb::PhysicalDevice& candidate )
        {
            ProbedDevice probed;
            probed.Physical             = std::make_shared<vkb::PhysicalDevice>( candidate );
            vkb::PhysicalDevice& device = *probed.Physical;

            probed.Caps =
                 PlanRoutes( device.properties.deviceName, DriverOf( device ), device.properties.apiVersion,
                             [&device]( std::string_view extension )
                             { return device.is_extension_present( std::string( extension ).c_str() ); } );

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

    Common::ResultStr<ProbedDevice> SelectDevice( const vkb::Instance& instance )
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
            ProbedDevice probed = Probe( candidate );
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
