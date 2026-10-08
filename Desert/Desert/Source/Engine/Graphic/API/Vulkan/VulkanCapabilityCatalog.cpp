#include <Engine/Graphic/API/Vulkan/VulkanCapabilityCatalog.hpp>

#include <algorithm>

// The pure half of the catalog fill. No vkGet* call lives in this file — ProbeCatalog
// (VulkanCapabilityCatalogProbe.cpp) asks the driver — so the ScalabilityContract suite compiles it against
// probe fixtures without a loader or a GPU.
namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        using namespace Common::Scalability;

        constexpr uint32_t kVendorNvidia = 0x10DE;
        constexpr uint32_t kVendorApple  = 0x106B;

        // The render-scale ceiling is stated against a 4K output: a supersampled 3840-wide frame at P % needs a
        // 3840 * P / 100 wide target, which the device must be able to create.
        constexpr uint32_t kReferenceOutputWidth = 3840;

        bool SurfaceLists( const CatalogProbe& probe, VkFormat format, VkColorSpaceKHR colorSpace )
        {
            return std::any_of( probe.SurfaceFormats.begin(), probe.SurfaceFormats.end(),
                                [&]( const VkSurfaceFormatKHR& f )
                                { return f.format == format && f.colorSpace == colorSpace; } );
        }

        bool SurfacePresents( const CatalogProbe& probe, VkPresentModeKHR mode )
        {
            return std::find( probe.PresentModes.begin(), probe.PresentModes.end(), mode ) !=
                   probe.PresentModes.end();
        }
    } // namespace

    Common::Scalability::CapabilityCatalog BuildCapabilityCatalog( const CatalogProbe& probe )
    {
        CapabilityCatalog catalog;
        const bool        apple = probe.VendorId == kVendorApple;

        // MSAA: a count is selectable only when colour AND depth attachments take it — the scene framebuffer
        // multisamples both.
        const VkSampleCountFlags both = probe.ColorSampleCounts & probe.DepthSampleCounts;
        catalog.MSAACounts.push_back( 1 );
        for ( const int count : { 2, 4, 8, 16, 32, 64 } )
            if ( ( both & static_cast<VkSampleCountFlags>( count ) ) != 0 )
                catalog.MSAACounts.push_back( count );

        // Upscalers, best first. DLSS is NVIDIA-only even if a runtime claims otherwise; MetalFX exists only
        // through MoltenVK.
        if ( probe.DlssAvailable && probe.VendorId == kVendorNvidia )
            catalog.Upscalers.push_back( Upscaler::DLSS );
        if ( probe.XessAvailable )
            catalog.Upscalers.push_back( Upscaler::XeSS );
        if ( probe.MetalFxAvailable && probe.Portability )
            catalog.Upscalers.push_back( Upscaler::MetalFX );
        if ( probe.FsrAvailable )
            catalog.Upscalers.push_back( Upscaler::FSR );
        catalog.Upscalers.push_back( Upscaler::TAAU );
        catalog.Upscalers.push_back( Upscaler::None );

        // AA methods in enum order: there is no single quality order across methods (MSAA vs SMAA depends on
        // the content), so Resolve's rule 4 names each method's fallback explicitly instead of walking this.
        catalog.AntiAliasingMethods = { AntiAliasingMethod::None, AntiAliasingMethod::FXAA,
                                        AntiAliasingMethod::SMAA };
        if ( catalog.MSAACounts.size() > 1 )
            catalog.AntiAliasingMethods.push_back( AntiAliasingMethod::MSAA );
        catalog.AntiAliasingMethods.push_back( AntiAliasingMethod::TAA );
        if ( CapabilityCatalog::Offers( catalog.Upscalers, Upscaler::FSR ) )
            catalog.AntiAliasingMethods.push_back( AntiAliasingMethod::FSRNative );
        if ( CapabilityCatalog::Offers( catalog.Upscalers, Upscaler::DLSS ) )
            catalog.AntiAliasingMethods.push_back( AntiAliasingMethod::DLAA );

        // Render scale: the SSAA ceiling is bounded by the largest 2D image the device creates; never below
        // native (a device whose 4K target cannot exceed 100 % simply has no supersampling).
        const uint32_t maxByImage = probe.MaxImageDimension2D * 100u / kReferenceOutputWidth;
        catalog.RenderScale.MaxPercent =
             std::max( 100, std::min( catalog.RenderScale.MaxPercent, static_cast<int>( maxByImage ) ) );

        // Ray tracing: the pipeline is never offered through portability (MoltenVK has no SBT path even when a
        // feature bit leaks through).
        catalog.RayTracingModes.push_back( RayTracingMode::None );
        if ( probe.Caps.Has( Capability::RayQuery ) && probe.Caps.Has( Capability::AccelerationStructure ) )
            catalog.RayTracingModes.push_back( RayTracingMode::RayQuery );
        if ( probe.Caps.Has( Capability::RayTracingPipeline ) &&
             probe.Caps.Has( Capability::AccelerationStructure ) && !probe.Portability )
            catalog.RayTracingModes.push_back( RayTracingMode::RayTracingPipeline );

        // Anisotropy: the standard levels up to the device's limit.
        catalog.AnisotropyLevels.push_back( 1 );
        if ( probe.Caps.Has( Capability::SamplerAnisotropy ) )
            for ( const int level : { 2, 4, 8, 16 } )
                if ( static_cast<float>( level ) <= probe.MaxSamplerAnisotropy )
                    catalog.AnisotropyLevels.push_back( level );

        catalog.DisplayOutputs.push_back( DisplayOutput::SDR_sRGB );
        if ( SurfaceLists( probe, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT ) )
            catalog.DisplayOutputs.push_back( DisplayOutput::HDR10_PQ );
        if ( SurfaceLists( probe, VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT ) )
            catalog.DisplayOutputs.push_back( DisplayOutput::scRGB_Linear );

        // FIFO is guaranteed by the Vulkan spec, so it is listed even before a surface was probed.
        catalog.PresentModes.push_back( PresentMode::Fifo );
        if ( SurfacePresents( probe, VK_PRESENT_MODE_FIFO_RELAXED_KHR ) )
            catalog.PresentModes.push_back( PresentMode::FifoRelaxed );
        if ( SurfacePresents( probe, VK_PRESENT_MODE_MAILBOX_KHR ) )
            catalog.PresentModes.push_back( PresentMode::Mailbox );
        if ( SurfacePresents( probe, VK_PRESENT_MODE_IMMEDIATE_KHR ) )
            catalog.PresentModes.push_back( PresentMode::Immediate );

        // ASTC first on Apple GPUs (native, smaller at equal quality); BC wherever it is enabled.
        if ( probe.AstcLdrSampled && probe.Portability && apple )
            catalog.TextureCompression.push_back( TextureCompressionFamily::ASTC );
        if ( probe.Caps.Has( Capability::TextureCompressionBC ) )
            catalog.TextureCompression.push_back( TextureCompressionFamily::BC );

        catalog.AsyncCompute = probe.SeparateComputeFamily && !probe.Portability;

        if ( probe.TimestampValidBitsGraphics == 0 )
            catalog.Timing = GpuTiming::None;
        else if ( probe.Portability && apple )
            catalog.Timing = GpuTiming::EncoderBounds;
        else
            catalog.Timing = GpuTiming::AnyStage;

        if ( apple )
            catalog.Class = DeviceClass::AppleUnified;
        else if ( probe.Type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU )
            catalog.Class = DeviceClass::Discrete;
        else if ( probe.Type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU )
            catalog.Class = DeviceClass::Integrated;
        else
            catalog.Class = DeviceClass::Unknown;
        catalog.VideoMemory = probe.DeviceLocalBytes;

        return catalog;
    }
} // namespace Desert::Graphic::API::Vulkan
