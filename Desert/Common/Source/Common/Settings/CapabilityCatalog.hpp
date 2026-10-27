#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// THE CAPABILITY CATALOG — WHAT THIS DEVICE OFFERS, PER SETTING, AS A LIST (SCAL1).
//
// WHY A LIST AND NOT A CHECK. Until SCAL1 every selector asked the device at the moment of choice — the
// Scalability panel read a device MSAA maximum to trim its MSAA combo, SceneRenderer clamped the sample
// count again when it built its framebuffers, the swapchain walks IMMEDIATE -> MAILBOX -> FIFO on its own, the
// GPU profiler switches itself off. Four places, four answers to one question, and two of them silent. UE has
// the same shape scattered across RHI feature flags and per-CVar clamps. The catalog is the better shape: built
// ONCE when the device is created, it is the only source of "what this device can do", every selector (editor
// UI, game menu, the console, the scalability resolver) OFFERS its list, and a saved value the list does not
// contain is resolved ONCE by Scalability::Resolve with one logged fallback (the pattern the retired
// per-setting AA resolver had, generalised).
//
// WHY IT LIVES IN COMMON. It is engine-core DATA — no Vulkan, no ImGui, no Editor type. The resolver that turns
// a saved choice into an effective one lives in Common/Settings next to machine.json (the packaged Runtime runs
// it too), and Common does not link the engine; so the TYPE is here and the engine's DeviceCapabilities carries
// one (Engine/Core/Device.hpp). Filled by a backend through a pure function of probed facts
// (Graphic/API/Vulkan/VulkanCapabilityCatalog.hpp), never by a renderer.
//
// THE FIELD RULE OF DeviceCapabilities APPLIES: a list belongs here only once something reads it. Each list
// below names its reader. A list added "for completeness" is a dead setting (contract §1.3).
//
// MAC (MoltenVK) COLUMN. Each list says what a MoltenVK device fills, because the catalog is the one place the
// difference is allowed to exist: no ray-tracing pipeline, ASTC on Apple Silicon, MetalFX instead of DLSS,
// timestamps only at encoder boundaries.
namespace Common::Scalability
{
    // ---- The two axes of image quality (owner, 2026-10-05; one temporal method, owner 2026-10-27) ----------
    //
    // AXIS 1 — the ANTI-ALIASING METHOD: one choice, mutually exclusive (UE r.AntiAliasingMethod, a vendor
    //          upscaler plugin taking TAA/TSR's place). Either a post AA without history (None / FXAA / SMAA /
    //          MSAA) or ONE temporal method (TAA, TSR, DLSS, DLSS Ray Reconstruction, FSR, XeSS, MetalFX).
    // AXIS 2 — the RENDER SCALE (UE r.ScreenPercentage). A temporal method runs at any percent <= 100 and
    //          reconstructs the output itself (DLSS at 100 % is what NVIDIA calls DLAA, FSR at 100 % is "Native
    //          AA"); a method without history is brought to the output below 100 % by the spatial upscale; above
    //          100 % the frame is supersampled (SSAA) and the vendor networks and TSR do not run (Resolve refuses
    //          them by name and TAA resolves instead).
    //
    // The percent is NOT part of the method (that is the rejected flat "AA mode" enum: DLSS-Quality,
    // FSR-Balanced, ... — it multiplies upscaler x scale and cannot say "DLSS at 77 %"). A MENU PRESET
    // ("DLSS Quality", "Native + TAA") decomposes into one value of each axis (AntiAliasingPreset,
    // Scalability.hpp). Until 2026-10-27 the temporal fact lived in two settings (AntiAliasingMethod
    // TAA/FSRNative/DLAA at 100 % and a Resolution.Upscaler below it, reconciled by Resolve); MachineSettings
    // migrates a file that still has them.
    //
    // STORED BY NAME in Scalability.json (kAntiAliasingMethodNames) and by integer in a machine.json override, so
    // the values only ever grow at the end. 5 and 6 are the integers the retired FSRNative and DLAA had: an
    // override written by an older build means the same vendor method.
    enum class AntiAliasingMethod : int
    {
        None = 0,
        FXAA,
        SMAA,
        // Multisampling; the count is the separate parameter AntiAliasingSamples, offered from
        // CapabilityCatalog::MSAACounts. Forward path only (EffectiveAntiAliasing table in MachineSettings.hpp),
        // native scale only.
        MSAA,
        // The engine's own temporal AA: a same-extent resolve at 100 % and above, TAAU below. Always offered.
        TAA,
        // AMD FidelityFX Super Resolution 3.1 (Graphic::Fsr3Upscaler); at 100 % its Native AA. Offered once the
        // SDK is in the build.
        FSR,
        // NVIDIA DLSS Super Resolution (Graphic::DLSSUpscaler, Streamline); at 100 % NVIDIA's DLAA. NVIDIA RTX
        // with the runtime loaded; never on Mac.
        DLSS,
        // The engine's temporal super resolution (UE TSR: Graphic::TemporalSuperResolution). Always offered: pure
        // Vulkan compute, nothing beyond the required feature set (MoltenVK included).
        TSR,
        // NVIDIA DLSS Ray Reconstruction (Streamline sl::kFeatureDLSS_RR): DLSS's network that is also the
        // denoiser of the view's noisy lighting signals, which then reach it raw (Graphic::ITemporalUpscaler::
        // TakesRawLightingSignals). Any percent <= 100. A view with no noisy signal refuses it by name and renders
        // DLSS Super Resolution (SceneRenderer, Graphic::RayReconstructionRefusal). Never on Mac.
        DLSSRayReconstruction,
        // Intel XeSS — offered once its SDK is integrated.
        XeSS,
        // Apple MetalFX temporal scaler — Mac only, through the MoltenVK/Metal interop; offered once integrated.
        MetalFX,
    };

    // THE RECONSTRUCTION PASS OF A FRAME — derived, never stored. Scalability UpscalerForScale(method, percent) is
    // the one rule that gives it; the renderer reads it per view (Graphic::SelectTemporalMethod). `None` = nothing
    // reconstructs (native / supersampled, or TAA's same-extent resolve), `TAAU` = TAA below 100 %, `Spatial` =
    // the engine's edge-adaptive spatial upscale (FSR1 EASU port) of a method without history below 100 %; every
    // other value is the temporal method of that name, at any percent <= 100.
    enum class Upscaler : int
    {
        None = 0,
        TAAU,
        FSR,
        DLSS,
        XeSS,
        MetalFX,
        Spatial,
        TSR,
        DLSSRayReconstruction,
    };

    // How rays may be traced on this device. Reader: Graphic::ResolveRenderMethods — a traced render method
    // (Common/Settings/RenderMethods.hpp NeedsRayQuery) on a device that lists only None is lowered down its
    // ladder with one logged line.
    enum class RayTracingMode : int
    {
        None = 0,
        RayQuery,           // VK_KHR_ray_query from any shader stage (inline RT); needs AccelerationStructure
        RayTracingPipeline, // VK_KHR_ray_tracing_pipeline (raygen/hit/miss + SBT); NEVER on MoltenVK
    };

    // The swapchain's output encoding. Reader: the swapchain (format + colour space) and the tonemapper's
    // output transform. SDR is always present; the HDR entries exist only when the SURFACE lists the pair.
    enum class DisplayOutput : int
    {
        SDR_sRGB = 0, // 8-bit UNORM/SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
        HDR10_PQ,     // A2B10G10R10, VK_COLOR_SPACE_HDR10_ST2084_EXT
        scRGB_Linear, // R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT (also MoltenVK EDR)
    };

    // Present modes, in the engine's names. Reader: the swapchain (VSync / latency setting). FIFO is always
    // present (Vulkan guarantees it), so the list is never empty.
    enum class PresentMode : int
    {
        Fifo = 0,    // VSync
        FifoRelaxed, // VSync, tears when late
        Mailbox,     // low latency, no tearing
        Immediate,   // no VSync; tears. MoltenVK offers it only when the CAMetalLayer allows displaySync off
    };

    // Block-compression families the device samples natively. Reader: the cooker's target-format choice and
    // the DDC key (Content/DerivedDataCache) — a texture is cooked into the FIRST family of this list that its
    // usage has a format for. BC on every desktop GPU (on MoltenVK only on Intel/AMD Macs and on Apple Silicon
    // with textureCompressionBC); ASTC on Apple Silicon.
    enum class TextureCompressionFamily : int
    {
        BC = 0,
        ASTC,
    };

    // How the device can time GPU work. Reader: the GPU profiler and the recommended-settings benchmark.
    enum class GpuTiming : int
    {
        None = 0,      // no timestamp queries — profiler is CPU-only, benchmark uses the device-class fallback
        EncoderBounds, // MoltenVK on Apple GPUs: timestamps land only at render/compute encoder boundaries, so a
                       // pass-level reading inside one encoder is fiction — profiler shows encoders, benchmark
                       // uses the device-class fallback
        AnyStage,      // vkCmdWriteTimestamp at any pipeline stage on the graphics queue
    };

    // A coarse device class. Reader: the recommended-settings fallback when the benchmark cannot time
    // (GpuTiming != AnyStage), and the default level table for a first launch before a benchmark exists.
    enum class DeviceClass : int
    {
        Unknown = 0,
        Integrated,   // shared memory, x86 iGPU
        AppleUnified, // Apple Silicon: unified memory, tile-based; VRAM figure = Metal
                      // recommendedMaxWorkingSetSize
        Discrete,
    };

    // The render-scale axis' legal range, in percent of output size. Reader: the ResolutionScale group.
    struct RenderScaleRange
    {
        int MinPercent = 33;  // the vendors' Ultra Performance mode (1/3.0 per axis: FSR 3.1, DLSS); below it
                              // no upscaler has a mode
        int MaxPercent = 200; // SSAA ceiling: 4x the pixels; bounded further by MaxTexture2DSize at output size

        bool operator==( const RenderScaleRange& ) const = default;
    };

    // The written names of the enums a quality parameter stores, and of the derived Upscaler (log lines), indexed
    // by the enum's value: the Scalability.json loader reads them, the catalog log line and the editor's labels
    // print them. One list per enum so a name can never differ between the data file and the log.
    inline constexpr std::array<std::string_view, 11> kAntiAliasingMethodNames{
         "None", "FXAA", "SMAA", "MSAA", "TAA", "FSR", "DLSS", "TSR", "DLSSRayReconstruction", "XeSS", "MetalFX" };
    inline constexpr std::array<std::string_view, 9> kUpscalerNames{ "None",    "TAAU",    "FSR",
                                                                     "DLSS",    "XeSS",    "MetalFX",
                                                                     "Spatial", "TSR",     "DLSSRayReconstruction" };
    inline constexpr std::array<std::string_view, 3> kRayTracingModeNames{ "None", "RayQuery",
                                                                           "RayTracingPipeline" };
    static_assert( static_cast<std::size_t>( AntiAliasingMethod::MetalFX ) + 1 ==
                   kAntiAliasingMethodNames.size() );
    // The integers an older build's machine.json override wrote for its FSRNative / DLAA (see AntiAliasingMethod).
    static_assert( static_cast<int>( AntiAliasingMethod::FSR ) == 5 &&
                   static_cast<int>( AntiAliasingMethod::DLSS ) == 6 );
    static_assert( static_cast<std::size_t>( Upscaler::DLSSRayReconstruction ) + 1 == kUpscalerNames.size() );
    static_assert( static_cast<std::size_t>( RayTracingMode::RayTracingPipeline ) + 1 ==
                   kRayTracingModeNames.size() );

    // WHICH DLSS NETWORK A DLSS MODE RUNS (UE: r.NGX.DLSS.DLAAPreset / QualityPreset / ... PerformancePreset /
    // UltraPerformancePreset), one Parameter per mode (Parameter::DlssPresetNative ..; "Native" is the 100 % mode
    // NVIDIA calls DLAA). Default is NVIDIA's choice for the mode, which a driver-side DLSS update may move. Only
    // the presets the Streamline SDK still recommends are offered (sl_dlss.h: A-D were removed, E-F are
    // deprecated, G-I and N-O revert to the default). Stored by name; the Streamline runtime maps a value to its
    // sl::DLSSPreset (StreamlineRuntime.cpp).
    enum class DlssPreset : int
    {
        Default = 0,
        J, // transformer; slightly less ghosting than K, more flicker
        K, // transformer; NVIDIA's default for DLAA / Quality / Balanced
        L, // NVIDIA's default for Ultra Performance
        M, // NVIDIA's default for Performance
    };
    inline constexpr std::array<std::string_view, 5> kDlssPresetNames{ "Default", "J", "K", "L", "M" };
    static_assert( static_cast<std::size_t>( DlssPreset::M ) + 1 == kDlssPresetNames.size() );

    // WHICH RAY RECONSTRUCTION NETWORK A DLSS MODE RUNS under Upscaler::DLSSRayReconstruction (UE: r.NGX.DLSS.RR.
    // <Mode>Preset), one Parameter per mode (Parameter::DlssRRPresetNative ..). Ray Reconstruction has its own
    // networks (sl_dlss_d.h DLSSDPreset): A-C were removed, D and E are the transformer models, F-O revert to the
    // default; only D and E are offered. Stored by name; the Streamline runtime maps a value to its
    // sl::DLSSDPreset.
    enum class DlssRRPreset : int
    {
        Default = 0,
        D, // transformer
        E, // transformer; NVIDIA's latest
    };
    inline constexpr std::array<std::string_view, 3> kDlssRRPresetNames{ "Default", "D", "E" };
    static_assert( static_cast<std::size_t>( DlssRRPreset::E ) + 1 == kDlssRRPresetNames.size() );

    // THE CATALOG. Filled once per device (VulkanCapabilityCatalog::Build), then immutable for the device's
    // life; a device loss rebuilds it with the new device. Every list is ordered BEST FIRST where an order
    // exists, so "the best this device offers" is front() and a fallback walks the list.
    struct CapabilityCatalog
    {
        // Reader: AntiAliasing group (AntiAliasingMethod parameter), every AA selector. Always contains None,
        // FXAA, SMAA, TAA, TSR. MSAA iff MSAACounts has a count > 1. FSR / DLSS / DLSSRayReconstruction / XeSS /
        // MetalFX iff the build has the SDK and the device runs it (VulkanCapabilityCatalog). Mac:
        // None/FXAA/SMAA/MSAA/TAA/TSR (+FSR, MetalFX once integrated); never DLSS. The spatial upscale of a
        // method without history is always available and is not listed (it is not a choice).
        std::vector<AntiAliasingMethod> AntiAliasingMethods;

        // Reader: AntiAliasing group (AntiAliasingSamples). Colour AND depth sample counts both supported,
        // ascending, always starting with 1. Mac: Apple GPUs report 1/2/4 (8 on some), MoltenVK passes them on.
        std::vector<int> MSAACounts;

        // Reader: ResolutionScale group (RenderScalePercent).
        RenderScaleRange RenderScale;

        // Reader: Graphic::ResolveRenderMethods (the traced GI / reflection / shadow methods). Always contains None.
        // Mac: None, plus RayQuery where MoltenVK exposes VK_KHR_ray_query (Apple7+ GPU, MoltenVK >= 1.2.x);
        // RayTracingPipeline never.
        std::vector<RayTracingMode> RayTracingModes;

        // Reader: Graphic::ResolveRenderMethods (NeedsRenderArea: the virtual shadow map's attachment-less
        // raster). The largest square render area the device rasterizes: min of maxFramebufferWidth / Height and
        // maxViewportDimensions. Mac: 16384 on Apple GPUs (Metal's texture and render-target limit).
        uint32_t MaxRenderArea = 0;

        // Reader: Graphic::ResolveRenderMethods and Scalability's method ceilings (NeedsFragmentStores: the
        // virtual shadow map's caster raster atomic-maxes into a storage image from the fragment stage). The
        // device's fragmentStoresAndAtomics, enabled (DeviceCaps). Mac: true on Apple GPUs (MoltenVK).
        bool FragmentStores = false;

        // Reader: Filtering group (Anisotropy parameter). The selectable levels (1, 2, 4, 8, 16) up to the
        // device's maxSamplerAnisotropy; {1} when samplerAnisotropy is absent. Mac: 1..16.
        std::vector<int> AnisotropyLevels;

        // Reader: swapchain + tonemapper output (display settings). Always contains SDR_sRGB. Mac: SDR_sRGB, plus
        // scRGB_Linear / HDR10_PQ on EDR-capable displays (MoltenVK exposes them via
        // VK_EXT_swapchain_colorspace).
        std::vector<DisplayOutput> DisplayOutputs;

        // Reader: swapchain (VSync setting). Always contains Fifo.
        std::vector<PresentMode> PresentModes;

        // Reader: cooker target + DDC key. Never empty: the required feature set refuses a device that samples
        // neither (DeviceCaps::CheckRequired). Windows: {BC}. Apple Silicon: {ASTC, BC} (BC only when
        // textureCompressionBC is enabled). Intel/AMD Mac: {BC}.
        std::vector<TextureCompressionFamily> TextureCompression;

        // Reader: RDG queue planner (async compute passes) — replaces VulkanRdgQueues' SeparateComputeFamily
        // question asked at compile time. Mac: false (MoltenVK exposes one queue family; Metal overlaps
        // encoders itself).
        bool AsyncCompute = false;

        // Reader: GPU profiler, benchmark. Mac (Apple GPU): EncoderBounds.
        GpuTiming Timing = GpuTiming::None;

        // Reader: recommended settings (benchmark fallback + VRAM-scaled texture / shadow levels).
        DeviceClass Class       = DeviceClass::Unknown;
        uint64_t    VideoMemory = 0; // device-local bytes (Mac: recommendedMaxWorkingSetSize); 0 = unknown

        // Does `list` offer `value`? The one membership question every selector and the resolver ask.
        template <typename T>
        [[nodiscard]] static bool Offers( const std::vector<T>& list, const T& value )
        {
            return std::find( list.begin(), list.end(), value ) != list.end();
        }

        bool operator==( const CapabilityCatalog& ) const = default;
    };

    // The catalog as one log line, written once at device creation beside DeviceCaps' table:
    // "aa=None,FXAA,SMAA,MSAA,TAA,TSR msaa=1,2,4,8 rt=None,RayQuery aniso=1..16 out=SDR present=..."
    [[nodiscard]] std::string FormatCatalog( const CapabilityCatalog& catalog );
} // namespace Common::Scalability
