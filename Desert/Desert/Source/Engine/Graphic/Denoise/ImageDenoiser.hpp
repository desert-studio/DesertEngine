#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <span>
#include <string>

namespace Desert::Graphic::Denoise
{
    // ONE IMAGE DENOISED OFFLINE, host memory in and out (UE IPathTracingDenoiser, the interface its
    // OpenImageDenoise plugin implements). Consumers: System::PathTracer (PathTracing.Denoiser OIDN, the converged
    // still frame) and, later, the Lightmass bake's Denoise stage (StaticLighting/LightmassBake.hpp) - one
    // interface for both, so the library is integrated once. No Vulkan type here: step 1 hands the images over in
    // host memory (the path tracer's read-back); a GPU-shared path (Vulkan external memory) is an implementation
    // detail behind the same call.
    //
    // Every image is Width x Height tightly packed float3 (12 bytes a pixel), row-major, top row first.
    struct ImageDenoiseInputs
    {
        uint32_t               Width  = 0;
        uint32_t               Height = 0;
        std::span<const float> Color;  // linear HDR radiance (any scale; OIDN's hdr mode)
        std::span<const float> Albedo; // reflectance in [0, 1]; empty: none
        std::span<const float>
             Normal; // world or view normal in [-1, 1], unnormalised averages allowed; empty: none
    };

    enum class ImageDenoiseQuality : uint8_t
    {
        Balanced, // interactive previews
        High,     // the final frame (UE's plugin: the converged image)
    };

    // What one Denoise call did (logged by the caller, shown by the control channel's state).
    struct ImageDenoiseReport
    {
        double Milliseconds = 0.0; // upload, filter and read-back, wall clock
    };

    class IImageDenoiser
    {
    public:
        virtual ~IImageDenoiser() = default;

        // The device the denoiser runs on, as the library named it (e.g. "CUDA: NVIDIA GeForce RTX 3070 Ti",
        // "CPU"), and why it is that one (e.g. "the Vulkan device's UUID matched", "no OIDN GPU module found this
        // GPU").
        [[nodiscard]] virtual const std::string& GetDeviceName() const   = 0;
        [[nodiscard]] virtual const std::string& GetDeviceReason() const = 0;

        // Denoises @p inputs into @p output (Width x Height float3). Blocking. An error names what refused; the
        // output is then untouched.
        [[nodiscard]] virtual Common::ResultStr<ImageDenoiseReport>
        Denoise( const ImageDenoiseInputs& inputs, ImageDenoiseQuality quality, std::span<float> output ) = 0;
    };
} // namespace Desert::Graphic::Denoise
