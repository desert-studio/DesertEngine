#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/SharedGpuMemory.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Graphic::Denoise
{
    // ONE IMAGE DENOISED ON THE GPU THAT RENDERED IT, in memory the renderer shares with the denoiser's API (UE
    // IPathTracingDenoiser, the interface its OpenImageDenoise plugin implements - which reads the image back to
    // the host and blocks; this one does neither). Consumers: System::PathTracer (PathTracing.Denoiser OIDN) and,
    // later, the Lightmass bake's Denoise stage (StaticLighting/LightmassBake.hpp) - one interface for both, so
    // the library is integrated once. No Vulkan type here: the memory and the order come as SharedHandle
    // (SharedGpuMemory.hpp), exported by the renderer (Renderer::CreateSharedBuffer / CreateSharedTimeline).
    //
    // Every image is Width x Height tightly packed float3 (12 bytes a pixel), row-major, top row first, at
    // ByteOffset into one of the shared buffers.
    struct SharedDenoiseBuffer
    {
        SharedHandle Handle;
        uint64_t     Bytes = 0; // the bytes the images address (Handle.AllocationBytes may be larger)
    };
    struct SharedDenoiseImage
    {
        uint32_t Buffer     = 0; // index into SharedDenoiseImages::Buffers
        uint64_t ByteOffset = 0;
    };
    struct SharedDenoiseImages
    {
        uint32_t                         Width  = 0;
        uint32_t                         Height = 0;
        std::vector<SharedDenoiseBuffer> Buffers;
        SharedDenoiseImage               Color;  // linear HDR radiance (any scale; OIDN's hdr mode)
        SharedDenoiseImage               Albedo; // reflectance in [0, 1]; denoised in place by its own prefilter
        SharedDenoiseImage               Normal; // [-1, 1], unnormalised averages allowed; denoised in place
        SharedDenoiseImage               Output;
        // The order between the renderer's queue and the denoiser's (a timeline semaphore). Kind None: Metal,
        // where the denoiser runs on the renderer's own MTLCommandQueue and the queue order is the order.
        SharedHandle Timeline;
    };

    enum class ImageDenoiseQuality : uint8_t
    {
        Balanced, // interactive previews
        High,     // the final frame (UE's plugin: the converged image)
    };

    class IImageDenoiser
    {
    public:
        virtual ~IImageDenoiser() = default;

        // The device the denoiser runs on, as the library named it (e.g. "CUDA: NVIDIA GeForce RTX 3070 Ti"), and
        // why it is that one (e.g. "the Vulkan device's UUID matched").
        [[nodiscard]] virtual const std::string& GetDeviceName() const   = 0;
        [[nodiscard]] virtual const std::string& GetDeviceReason() const = 0;

        // Imports @p images' buffers and timeline into the denoiser's API and prepares the filters for that size
        // and @p quality, once (again after a resize: the caller binds the new buffers). Replaces what was bound.
        // An error names what refused; nothing is bound then.
        [[nodiscard]] virtual Common::BoolResultStr BindShared( const SharedDenoiseImages& images,
                                                                ImageDenoiseQuality        quality ) = 0;

        // Enqueues one denoise of the bound images on the denoiser's queue, NOT waited: the queue waits
        // @p waitValue on the timeline (the renderer's work that wrote the inputs), filters, and signals
        // @p signalValue (the renderer's work that reads the output waits it). An error names what refused.
        [[nodiscard]] virtual Common::BoolResultStr EnqueueShared( uint64_t waitValue, uint64_t signalValue ) = 0;

        // Waits for the enqueued work and drops every import (the renderer frees the memory after this).
        virtual void ReleaseShared() = 0;
    };
} // namespace Desert::Graphic::Denoise
