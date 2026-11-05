#include <Engine/Graphic/API/Vulkan/VulkanCapabilityCatalog.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

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

        // NVIDIA's PCI device ids by die (the upper bits name the chip): TU10x / TU11x 0x1E00-0x21FF, except
        // GA100 0x20B0-0x20FF; GA10x 0x2200-0x25FF, except GH100 0x2330-0x233F (Hopper: FP8 tensor cores, so
        // costed as Ada); AD10x 0x2600-0x28FF; GB1xx / GB20x 0x2900 and up. Below 0x1E00 (Pascal and older, no
        // tensor cores): Other. A GTX 16 (TU116 / TU117) is a Turing die without tensor cores; Streamline offers
        // it no DLSS, so RecommendUpscaler stops at the offer before it reads the architecture.
        GpuArchitecture NvidiaArchitecture( const uint32_t deviceId )
        {
            if ( deviceId >= 0x2900 )
                return GpuArchitecture::Blackwell;
            if ( deviceId >= 0x2600 || ( deviceId >= 0x2330 && deviceId < 0x2340 ) )
                return GpuArchitecture::Ada;
            if ( deviceId >= 0x2200 || ( deviceId >= 0x20B0 && deviceId < 0x2100 ) )
                return GpuArchitecture::Ampere;
            if ( deviceId >= 0x1E00 )
                return GpuArchitecture::Turing;
            return GpuArchitecture::Other;
        }

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

    Common::BoolResultStr FsrDeviceSupport( const DeviceCaps& caps, const bool built )
    {
        std::string missing;
        const auto  add = [&missing]( std::string_view what )
        {
            if ( !missing.empty() )
                missing += "; ";
            missing += what;
        };
        if ( !built )
            add( "this binary has no FSR (the FidelityFX SDK's shader compiler, FidelityFX_SC.exe, is a Windows "
                 "binary: only a Windows build links it)" );
        if ( caps.DeviceApiVersion < VK_API_VERSION_1_2 )
            add( std::format( "Vulkan 1.2 (the SDK's SPIR-V targets vulkan1.2; the device reports {})",
                              FormatApiVersion( caps.DeviceApiVersion ) ) );
        if ( !caps.Has( Capability::StorageImageWriteWithoutFormat ) )
            add( "shaderStorageImageWriteWithoutFormat (FSR writes its output through a storage image declared "
                 "without a format)" );
        if ( missing.empty() )
            return Common::MakeSuccess( true );
        return Common::MakeError<bool>(
             std::format( "FSR 3.1 is not offered on '{}': {}", caps.DeviceName, missing ) );
    }

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

        // AA methods: the post AA methods, MSAA iff a count > 1, our temporal methods (TAA, TSR: nothing beyond
        // the required feature set — pure compute, buffer atomics only, MoltenVK has them), then each vendor
        // temporal method the build and the device run. In enum order: there is no single quality order across
        // methods (MSAA vs SMAA depends on the content), so Resolve's rule 4 names each method's fallback
        // explicitly instead of walking this. DLSS is NVIDIA-only even if a runtime claims otherwise; MetalFX
        // exists only through MoltenVK.
        catalog.AntiAliasingMethods = { AntiAliasingMethod::None, AntiAliasingMethod::FXAA,
                                        AntiAliasingMethod::SMAA };
        if ( catalog.MSAACounts.size() > 1 )
            catalog.AntiAliasingMethods.push_back( AntiAliasingMethod::MSAA );
        catalog.AntiAliasingMethods.push_back( AntiAliasingMethod::TAA );
        if ( probe.FsrAvailable )
            catalog.AntiAliasingMethods.push_back( AntiAliasingMethod::FSR );
        const bool dlss = probe.DlssAvailable && probe.VendorId == kVendorNvidia;
        if ( dlss )
            catalog.AntiAliasingMethods.push_back( AntiAliasingMethod::DLSS );
        catalog.AntiAliasingMethods.push_back( AntiAliasingMethod::TSR );
        if ( dlss && probe.DlssRayReconstructionAvailable )
            catalog.AntiAliasingMethods.push_back( AntiAliasingMethod::DLSSRayReconstruction );
        // What the runtime offered and then refused (CapabilityCatalog::RefusedAtRuntime): DLSS is NVIDIA-only
        // here too, so a refusal on another vendor's device was never an offer.
        if ( probe.VendorId == kVendorNvidia && probe.DlssRefused )
            catalog.RefusedAtRuntime.push_back( AntiAliasingMethod::DLSS );
        if ( probe.VendorId == kVendorNvidia && probe.DlssRayReconstructionRefused )
            catalog.RefusedAtRuntime.push_back( AntiAliasingMethod::DLSSRayReconstruction );
        if ( probe.XessAvailable )
            catalog.AntiAliasingMethods.push_back( AntiAliasingMethod::XeSS );
        if ( probe.MetalFxAvailable && probe.Portability )
            catalog.AntiAliasingMethods.push_back( AntiAliasingMethod::MetalFX );

        // Render scale: the SSAA ceiling is bounded by the largest 2D image the device creates; never below
        // native (a device whose 4K target cannot exceed 100 % simply has no supersampling).
        const uint32_t maxByImage = probe.MaxImageDimension2D * 100u / kReferenceOutputWidth;
        catalog.RenderScale.MaxPercent =
             std::max( 100, std::min( catalog.RenderScale.MaxPercent, static_cast<int>( maxByImage ) ) );

        catalog.MaxRenderArea  = probe.MaxRenderArea;
        catalog.FragmentStores = probe.Caps.Has( Capability::FragmentStoresAndAtomics );

        // GPU sharing (OIDN on the path tracer's buffers): the extensions enabled AND the driver exporting both a
        // storage buffer and a timeline semaphore of that handle type. Win32 first (the Windows handle type).
        if ( probe.Caps.Has( Capability::ExternalMemoryWin32 ) &&
             probe.Caps.Has( Capability::ExternalSemaphoreWin32 ) && probe.ExternalBufferExportWin32 &&
             probe.ExternalTimelineExportWin32 )
            catalog.Sharing = Common::Scalability::GpuSharing::Win32;
        else if ( probe.Caps.Has( Capability::ExternalMemoryFd ) &&
                  probe.Caps.Has( Capability::ExternalSemaphoreFd ) && probe.ExternalBufferExportFd &&
                  probe.ExternalTimelineExportFd )
            catalog.Sharing = Common::Scalability::GpuSharing::Fd;

        // Ray tracing, as UE keeps GRHISupportsRayTracing apart from GRHISupportsInlineRayTracing and offers
        // Lumen HWRT only on hardware RT. Ray query and the pipeline are two facts, never one implied by the
        // other; the pipeline is never offered through portability (MoltenVK has no SBT path even when a feature
        // bit leaks through).
        const bool accelerationStructures = probe.Caps.Has( Capability::AccelerationStructure );
        const bool rayQuery               = accelerationStructures && probe.Caps.Has( Capability::RayQuery );
        const bool rayTracingPipeline =
             accelerationStructures && probe.Caps.Has( Capability::RayTracingPipeline ) && !probe.Portability;
        // How the device executes those rays. A played device profile names it (the device it plays, not this
        // one); else ray query is hardware RT: every hardware implementation exposes VK_KHR_ray_query (NVIDIA
        // RTX, AMD RDNA 2+, Intel Arc, Apple7+ through MoltenVK), so the pipeline alone is the driver's
        // shader-core path (GeForce GTX 10/16).
        if ( !rayQuery && !rayTracingPipeline )
            catalog.RayTracingAccel = RayTracingAcceleration::None;
        else if ( probe.ProfileRayTracingAccel.has_value() )
            catalog.RayTracingAccel = *probe.ProfileRayTracingAccel;
        else if ( rayQuery )
            catalog.RayTracingAccel = RayTracingAcceleration::Hardware;
        else
            catalog.RayTracingAccel = RayTracingAcceleration::Emulated;
        // Render methods trace only on hardware RT: an emulated device lists None alone, so no traced method, no
        // ray-query program and no BLAS (MeshGpuUploader) is ever asked of it. The pipeline stays the device's
        // (DeviceCaps) for offline work.
        catalog.RayTracingModes.push_back( RayTracingMode::None );
        if ( catalog.RayTracingAccel == RayTracingAcceleration::Hardware )
        {
            if ( rayQuery )
                catalog.RayTracingModes.push_back( RayTracingMode::RayQuery );
            if ( rayTracingPipeline )
                catalog.RayTracingModes.push_back( RayTracingMode::RayTracingPipeline );
        }

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
        // A played profile is the card it names (an RTX 2060 is Turing on any host); else the die's.
        if ( probe.ProfileArchitecture.has_value() )
            catalog.Architecture = *probe.ProfileArchitecture;
        else if ( probe.VendorId == kVendorNvidia )
            catalog.Architecture = NvidiaArchitecture( probe.DeviceId );

        return catalog;
    }
} // namespace Desert::Graphic::API::Vulkan
