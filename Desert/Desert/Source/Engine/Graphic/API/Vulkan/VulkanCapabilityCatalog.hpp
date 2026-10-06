#pragma once

#include <Common/Settings/CapabilityCatalog.hpp>
#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <string>
#include <vector>

// THE VULKAN BACKEND'S CATALOG FILL (SCAL1) — the same two-step shape as DeviceCaps: a PROBE that asks the driver
// (VulkanDevice / VulkanSwapChain construction, the only code that calls vkGet*), and a PURE builder that turns
// the probed facts into Common::Scalability::CapabilityCatalog. Pure so that every device the engine must handle —
// an RTX card, an AMD card without DLSS, MoltenVK on Apple Silicon, MoltenVK on an Intel Mac — is a test fixture
// (Desert/Tests/Engine/ScalabilityContract), not a machine someone has to own.
//
// MoltenVK is not a second backend: it is this builder with `Portability` set, and every MoltenVK rule is written
// once below, in BuildCapabilityCatalog, keyed on probed facts (never on "#ifdef __APPLE__").
namespace Desert::Graphic::API::Vulkan
{
    // Everything the builder needs, as the driver reported it. Filled once at device creation; the surface half
    // is filled when the first swapchain's surface exists (the catalog is completed then, before any selector or
    // QualityState::Initialize runs).
    struct CatalogProbe
    {
        // From DeviceCaps (route + presence): RayQuery, RayTracingPipeline, AccelerationStructure,
        // SamplerAnisotropy, TextureCompressionBC, PortabilitySubset.
        DeviceCaps Caps;

        uint32_t             VendorId         = 0; // 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel, 0x106B Apple
        VkPhysicalDeviceType Type             = VK_PHYSICAL_DEVICE_TYPE_OTHER;
        uint64_t             DeviceLocalBytes = 0; // Mac: Metal recommendedMaxWorkingSetSize

        VkSampleCountFlags ColorSampleCounts    = VK_SAMPLE_COUNT_1_BIT; // framebufferColorSampleCounts
        VkSampleCountFlags DepthSampleCounts    = VK_SAMPLE_COUNT_1_BIT; // framebufferDepthSampleCounts
        float              MaxSamplerAnisotropy = 1.0f;
        uint32_t           MaxImageDimension2D  = 0;

        bool     AstcLdrSampled = false; // vkGetPhysicalDeviceFormatProperties: ASTC_4x4_UNORM sampled+filter
        uint32_t TimestampValidBitsGraphics  = 0; // queue family timestampValidBits of the graphics family
        bool     TimestampComputeAndGraphics = false;
        bool     Portability = false; // VK_KHR_portability_subset present: MoltenVK (timestamps at encoder bounds)
        bool     SeparateComputeFamily = false; // a compute-capable family without graphics

        // Third-party upscaler runtimes, each true only when its SDK is integrated AND its runtime loaded AND it
        // reported the device as supported (NGX "DLSS available", ffx context created, XeSS query, MetalFX
        // MTLFXTemporalScalerDescriptor.supportsDevice). None is integrated at SCAL1-C0: all false.
        bool DlssAvailable    = false;
        bool FsrAvailable     = false;
        bool XessAvailable    = false;
        bool MetalFxAvailable = false;

        std::vector<VkSurfaceFormatKHR> SurfaceFormats; // vkGetPhysicalDeviceSurfaceFormatsKHR
        std::vector<VkPresentModeKHR>   PresentModes;   // vkGetPhysicalDeviceSurfacePresentModesKHR
    };

    // PURE. The rules (each list's comment in CapabilityCatalog.hpp says who reads it):
    //   MSAACounts        = counts in Color & Depth, ascending from 1.
    //   AntiAliasing      = None, FXAA, SMAA, TAA always; MSAA iff a count > 1; FSRNative iff FSR; DLAA iff DLSS.
    //   Upscalers         = None, TAAU always; then DLSS (NVIDIA only), FSR, XeSS, MetalFX (Portability only)
    //                       each iff available — best first: DLSS, XeSS, MetalFX, FSR, TAAU, None.
    //   RayTracingModes   = None; RayQuery iff RayQuery+AccelerationStructure present; RayTracingPipeline iff
    //                       present and not Portability.
    //   AnisotropyLevels  = 1,2,4,8,16 up to MaxSamplerAnisotropy; {1} without SamplerAnisotropy.
    //   DisplayOutputs    = SDR_sRGB; HDR10_PQ / scRGB_Linear iff the surface lists their (format, colour space).
    //   PresentModes      = Fifo always (spec); the rest as listed by the surface.
    //   TextureCompression= ASTC first iff AstcLdrSampled and Portability on an Apple GPU; BC iff
    //                       TextureCompressionBC present.
    //   AsyncCompute      = SeparateComputeFamily and not Portability.
    //   Timing            = None without timestamp bits; EncoderBounds under Portability on Apple; else AnyStage.
    //   Class             = Discrete / Integrated from Type; AppleUnified for Apple vendor id.
    //   RenderScale.Max   = min(200, 100 * MaxImageDimension2D / output dimension at the reference 3840)
    [[nodiscard]] Common::Scalability::CapabilityCatalog BuildCapabilityCatalog( const CatalogProbe& probe );

    // The probe's driver half: fills CatalogProbe from the physical device and (when non-null) the surface.
    // Implemented beside DeviceCapsProbe.cpp; the only caller is VulkanPhysicalDevice construction plus the first
    // swapchain's creation.
    [[nodiscard]] CatalogProbe ProbeCatalog( VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                             const DeviceCaps& caps );
} // namespace Desert::Graphic::API::Vulkan
