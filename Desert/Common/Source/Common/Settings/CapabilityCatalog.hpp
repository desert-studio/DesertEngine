#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// THE CAPABILITY CATALOG — WHAT THIS DEVICE OFFERS, PER SETTING, AS A LIST (SCAL1).
//
// WHY A LIST AND NOT A CHECK. Until SCAL1 every selector asked the device at the moment of choice — the
// Scalability panel reads RenderConfig::MaxMSAASamples to trim its MSAA combo, SceneRenderer clamps the sample
// count again when it builds its framebuffers, the swapchain walks IMMEDIATE -> MAILBOX -> FIFO on its own, the
// GPU profiler switches itself off. Four places, four answers to one question, and two of them silent. UE has
// the same shape scattered across RHI feature flags and per-CVar clamps. The catalog is the better shape: built
// ONCE when the device is created, it is the only source of "what this device can do", every selector (editor
// UI, game menu, the console, the scalability resolver) OFFERS its list, and a saved value the list does not
// contain is resolved ONCE by Scalability::Resolve with one logged fallback (the MachineSettings::ResolveAA
// pattern, generalised).
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
    // ---- The two axes of image quality (owner, 2026-10-05) ----------------------------------------------
    //
    // AXIS 1 — the ANTI-ALIASING METHOD: one choice, mutually exclusive (UE r.AntiAliasingMethod).
    // AXIS 2 — the RENDER SCALE and the UPSCALER that brings it back to output size (UE r.ScreenPercentage +
    //          r.TemporalAA.Upscaler / the upscaler plugin). Below 100 % an upscaler is mandatory, at 100 % the
    //          frame is native, above 100 % it is supersampled (SSAA) and downsampled with a fixed filter.
    //
    // A MENU PRESET ("DLSS Quality", "Native + TAA", "Performance") is not a third axis: it decomposes into one
    // value of each (AntiAliasingPreset, Scalability.hpp). Rejected alternative: one flat "AA mode" enum holding
    // DLSS-Quality, FSR-Balanced, MSAA4x... — it multiplies (upscaler × scale × method) into a list nobody can
    // validate against the device, and it cannot say "DLSS at 77 %".
    enum class AntiAliasingMethod : int
    {
        None = 0,
        FXAA,
        SMAA,
        // Multisampling; the count is the separate parameter AntiAliasingSamples, offered from
        // CapabilityCatalog::MSAACounts. Forward path only (EffectiveAntiAliasing table in MachineSettings.hpp).
        MSAA,
        // The engine's own temporal AA at 100 % (and the temporal half of TAAU below 100 %). Always offered: it
        // is ours and needs nothing the required feature set lacks.
        TAA,
        // FSR's temporal pass run at 100 % scale ("FSR Native AA"). Offered iff Upscaler::FSR is.
        FSRNative,
        // DLSS's network run at 100 % scale. Offered iff Upscaler::DLSS is.
        DLAA,
    };

    // The upscaler of axis 2. `None` is the only legal value at render scale >= 100 % (native / SSAA), and an
    // illegal one below 100 %.
    enum class Upscaler : int
    {
        None = 0,
        TAAU,    // the engine's temporal upscaler — always offered (no third-party runtime)
        FSR,     // AMD FidelityFX Super Resolution 2+/3 — vendor-agnostic; offered once its SDK is integrated
        DLSS,    // NVIDIA DLSS via NGX/Streamline — NVIDIA RTX + the runtime DLL loaded; never on Mac
        XeSS,    // Intel XeSS — offered once its SDK is integrated
        MetalFX, // Apple MetalFX temporal scaler — Mac only, reached through the MoltenVK/Metal interop
    };

    // How rays may be traced on this device. Reader: the ray-traced shadows / reflections / GI parameters
    // (Shadows, Reflections, GlobalIllumination groups) — a level that asks for RT on a device that lists only
    // None resolves to the raster technique with one logged fallback.
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
        int MinPercent = 50;  // below it every upscaler's quality collapses (DLSS Ultra Performance is 33 %; we
                              // do not offer it until something renders it acceptably)
        int MaxPercent = 200; // SSAA ceiling: 4x the pixels; bounded further by MaxTexture2DSize at output size

        bool operator==( const RenderScaleRange& ) const = default;
    };

    // THE CATALOG. Filled once per device (VulkanCapabilityCatalog::Build), then immutable for the device's
    // life; a device loss rebuilds it with the new device. Every list is ordered BEST FIRST where an order
    // exists, so "the best this device offers" is front() and a fallback walks the list.
    struct CapabilityCatalog
    {
        // Reader: AntiAliasing group (AntiAliasingMethod parameter), every AA selector. Always contains None,
        // FXAA, SMAA, TAA. MSAA iff MSAACounts has a count > 1. FSRNative/DLAA iff the matching upscaler is
        // offered. Mac: None/FXAA/SMAA/MSAA/TAA (+FSRNative once FSR is integrated); never DLAA.
        std::vector<AntiAliasingMethod> AntiAliasingMethods;

        // Reader: AntiAliasing group (AntiAliasingSamples). Colour AND depth sample counts both supported,
        // ascending, always starting with 1. Mac: Apple GPUs report 1/2/4 (8 on some), MoltenVK passes them on.
        std::vector<int> MSAACounts;

        // Reader: ResolutionScale group (Upscaler parameter). Always contains None and TAAU. Mac: None, TAAU,
        // MetalFX (macOS 13+ on Apple Silicon), FSR once integrated; never DLSS / XeSS.
        std::vector<Upscaler> Upscalers;

        // Reader: ResolutionScale group (RenderScalePercent).
        RenderScaleRange RenderScale;

        // Reader: Shadows / Reflections / GlobalIllumination ray-traced parameters. Always contains None.
        // Mac: None, plus RayQuery where MoltenVK exposes VK_KHR_ray_query (Apple7+ GPU, MoltenVK >= 1.2.x);
        // RayTracingPipeline never.
        std::vector<RayTracingMode> RayTracingModes;

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
    // "aa=None,FXAA,SMAA,MSAA,TAA msaa=1,2,4,8 upscale=None,TAAU rt=None,RayQuery aniso=1..16 out=SDR present=..."
    [[nodiscard]] std::string FormatCatalog( const CapabilityCatalog& catalog );
} // namespace Common::Scalability
