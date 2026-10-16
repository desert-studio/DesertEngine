#include <Engine/Graphic/API/Vulkan/FidelityFX/FidelityFXDevice.hpp>

#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanCapabilityCatalog.hpp>

#include <vk-bootstrap/VkBootstrap.h>

#include <format>
#include <string>

namespace Desert::Graphic::API::Vulkan::FidelityFX
{
    namespace
    {
        // Why FSR cannot run on the selected device, empty when it can. Written once, before vkCreateDevice
        // (OnPhysicalDeviceSelected); read by every catalog probe after it.
        std::string& Refusal()
        {
            static std::string refusal;
            return refusal;
        }

        void Refuse( std::string_view why )
        {
            if ( !Refusal().empty() )
                return; // the first reason is the one named
            Refusal() = std::format( "FSR 3.1 is not offered: {}", why );
        }

        // An extension whose feature struct the device reports: the struct first (only a supported bit is worth
        // the extension), then the extension. False when either is absent.
        template <typename Features>
        bool EnableExtensionWithFeatures( vkb::PhysicalDevice& device, const char* extension,
                                          const Features& wanted )
        {
            if ( !device.enable_extension_features_if_present( wanted ) )
                return false;
            return device.enable_extension_if_present( extension );
        }
    } // namespace

    DeviceRequirements& DeviceRequirements::Get()
    {
        static DeviceRequirements requirements;
        return requirements;
    }

    std::string_view DeviceRequirements::Name() const
    {
        return "FidelityFX";
    }

    std::vector<const char*> DeviceRequirements::InstanceExtensions()
    {
        return {}; // the backend asks the instance for nothing (it is handed the device, ffx_vk.h VkDeviceContext)
    }

    void DeviceRequirements::OnPhysicalDeviceSelected( VkInstance /*instance*/, vkb::PhysicalDevice& device )
    {
        Refusal().clear();
        const uint32_t api = UsedApiVersion( device.properties.apiVersion );

        // VK_KHR_dedicated_allocation listed -> CreateBackendContextVK sizes its breadcrumbs buffer through
        // vkGetBufferMemoryRequirements2KHR (ffx_vk.cpp:1646-1663), a pointer it loads by that KHR name (:1487).
        // vkGetDeviceProcAddr answers a promoted command's KHR alias only when its extension is enabled, so
        // VK_KHR_get_memory_requirements2 is enabled (legal beside 1.1+ core: it adds no feature struct).
        if ( device.is_extension_present( VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME ) )
        {
            if ( !device.enable_extension_if_present( VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME ) )
                Refuse( std::format( "'{}' lists {} but not {}, and the FidelityFX backend calls "
                                     "vkGetBufferMemoryRequirements2KHR whenever the first is listed",
                                     device.properties.deviceName, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                                     VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME ) );
            device.enable_extension_if_present( VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME );
        }

        // VK_EXT_subgroup_size_control listed -> the wave64 permutations chain a required subgroup size of 64
        // (:3627-3633), which needs subgroupSizeControl enabled. The extension makes the feature mandatory, so a
        // refusal here names a driver that breaks the spec.
        if ( device.is_extension_present( VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME ) )
        {
            bool enabled = false;
            if ( api >= VK_API_VERSION_1_3 )
            {
                VkPhysicalDeviceVulkan13Features f{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
                f.subgroupSizeControl = VK_TRUE;
                enabled               = device.enable_extension_features_if_present( f );
            }
            else
            {
                VkPhysicalDeviceSubgroupSizeControlFeatures f{
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES };
                f.subgroupSizeControl = VK_TRUE;
                enabled = EnableExtensionWithFeatures( device, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME, f );
            }
            if ( !enabled )
                Refuse( std::format( "'{}' lists {} without the subgroupSizeControl feature the FidelityFX "
                                     "backend's wave64 pipelines need",
                                     device.properties.deviceName, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME ) );
        }

        // VK_KHR_shader_float16_int8 listed with shaderFloat16 -> the 16-bit permutations (:2008-2020). The
        // backend reads the same feature bit, so an unsupported bit selects the 32-bit shaders and needs nothing.
        if ( device.is_extension_present( VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME ) )
        {
            if ( api >= VK_API_VERSION_1_2 )
            {
                VkPhysicalDeviceVulkan12Features f{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
                f.shaderFloat16 = VK_TRUE;
                device.enable_extension_features_if_present( f );
            }
            else
            {
                VkPhysicalDeviceShaderFloat16Int8Features f{
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES };
                f.shaderFloat16 = VK_TRUE;
                EnableExtensionWithFeatures( device, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, f );
            }
        }

        // VK_KHR_synchronization2 listed with the feature -> vkCmdWriteBufferMarker2AMD for breadcrumbs (:1718).
        if ( device.is_extension_present( VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME ) )
        {
            if ( api >= VK_API_VERSION_1_3 )
            {
                VkPhysicalDeviceVulkan13Features f{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
                f.synchronization2 = VK_TRUE;
                device.enable_extension_features_if_present( f );
            }
            else
            {
                VkPhysicalDeviceSynchronization2Features f{
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
                f.synchronization2 = VK_TRUE;
                EnableExtensionWithFeatures( device, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, f );
            }
        }

        // AMD only: VK_AMD_device_coherent_memory with its feature -> breadcrumbs memory type (:2036), and
        // VK_AMD_buffer_marker -> vkCmdWriteBufferMarkerAMD (:1714, :4323). Neither has a core form.
        if ( device.is_extension_present( VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME ) )
        {
            VkPhysicalDeviceCoherentMemoryFeaturesAMD f{
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD };
            f.deviceCoherentMemory = VK_TRUE;
            EnableExtensionWithFeatures( device, VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME, f );
        }
        device.enable_extension_if_present( VK_AMD_BUFFER_MARKER_EXTENSION_NAME );

        if ( !Refusal().empty() )
            LOG_ERROR( "[FidelityFX] {}", Refusal() );
    }

    void DeviceRequirements::AmendCatalogProbe( CatalogProbe& probe ) const
    {
        // A device the backend would call an absent entry point on is not an FSR device, whatever the build and
        // the API version say (FsrDeviceSupport).
        if ( !Refusal().empty() )
            probe.FsrAvailable = false;
    }

    void DeviceRequirements::OnDeviceCreated( const VulkanDeviceHandles& /*handles*/ )
    {
        // Nothing to hand over: Fsr3Upscaler gives the backend the engine's device per context (VkDeviceContext).
    }

    void DeviceRequirements::OnDeviceDestroying()
    {
        // Nothing owned: every FidelityFX object belongs to an Fsr3Upscaler context, released with its view.
    }
} // namespace Desert::Graphic::API::Vulkan::FidelityFX
