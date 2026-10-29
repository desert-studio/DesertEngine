#pragma once

#include <Engine/Graphic/Denoise/ImageDenoiser.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Graphic::Denoise
{
    // What the OIDN denoiser did in this process, for the control channel's state (device section) and the log.
    struct OidnStatus
    {
        bool        Created = false; // a device was made (Device / Reason are its)
        std::string Device;          // "CUDA: <name>" / "HIP: <name>" / "SYCL: <name>" / "Metal: <name>"
        std::string Reason;          // why that device
        std::string Error;           // the last refusal (library missing, device error, filter error); empty: none
        uint64_t    Runs             = 0; // EnqueueShared calls that succeeded
        uint32_t    LastPaths        = 0; // paths per pixel of the image the last run filtered (the caller's)
        uint32_t    LastWidth        = 0;
        uint32_t    LastHeight       = 0;
        double      LastMilliseconds = 0.0; // the host time of the last EnqueueShared (the GPU work is not waited)
    };

    // INTEL OPEN IMAGE DENOISE 2.x behind IImageDenoiser (UE: the OpenImageDenoise plugin). The library is LOADED
    // at run time from beside the executable (Windows OpenImageDenoise.dll; macOS lib/libOpenImageDenoise.2.dylib)
    // - Desert/Dependencies.lua DesertCopyOidnRuntime puts it there, ThirdParty/OIDN/manifest.*.json pins it - and
    // nothing links it, so an executable without it starts and PathTracing.Denoiser OIDN reports the library
    // missing by name.
    //
    // THE DEVICE: OIDN's physical device whose UUID is the Vulkan device's (DeviceCapabilities::DeviceUuid; a
    // CUDA, HIP, SYCL or Metal module of OIDN offers it), so the filter runs on the GPU that rendered the image,
    // on that GPU's memory. When no OIDN GPU module offers that GPU (no module for the vendor, an unsupported
    // architecture, a driver too old), or the device imports neither the catalog's memory handle type nor its
    // timeline semaphore type (oidnGetDeviceInt "externalMemoryTypes" / "externalSemaphoreTypes"), Acquire
    // REFUSES by name with what OIDN offered: there is no CPU device and no host copy - a CPU filter would need
    // the image read back, which is the stall this path removes.
    //
    // THE MEMORY (BindShared): the renderer's exported buffers imported with oidnNewSharedBufferFromWin32Handle /
    // FromFD / FromMetal, and its exported timeline with oidnNewSharedSemaphoreFromWin32Handle / FromFD. A run
    // (EnqueueShared) is oidnWaitSemaphoresAsync, the three filters' oidnExecuteFilterAsync and
    // oidnSignalSemaphoresAsync: enqueued on OIDN's stream, never waited by the host. The graph's ExternalQueue
    // pass (RDG-EXTERNAL) places it between two submissions of the frame.
    //
    // THE FILTER (OIDN's recommendation for a noisy beauty with noisy guides): "RT", hdr, the albedo and the
    // normal first denoised by their own RT filter each, then the beauty with cleanAux - the guides of a few paths
    // carry their own sub-pixel noise, which would otherwise be read as detail. Quality High for the converged
    // frame.
    //
    // One instance per Vulkan device, made on first use (Acquire): loading the library and its device modules is
    // not paid by a session that never selects OIDN. Render thread only, but Status() may be read from any thread.
    class OidnDenoiser final : public IImageDenoiser
    {
    public:
        ~OidnDenoiser() override;
        OidnDenoiser( const OidnDenoiser& )            = delete;
        OidnDenoiser& operator=( const OidnDenoiser& ) = delete;

        // The process's denoiser for the Vulkan device with this UUID, importing @p sharing's handle types: made
        // on the first call (and again after a device change), else the one made before. An error names what
        // refused (GpuSharing None, the library, no OIDN GPU device for this GPU, the handle types); it is also
        // kept in Status().Error.
        [[nodiscard]] static Common::ResultStr<OidnDenoiser*> Acquire( const std::array<uint8_t, 16>&  vulkanUuid,
                                                                       Common::Scalability::GpuSharing sharing );

        [[nodiscard]] static OidnStatus Status();
        // The caller's facts about the image a successful run filtered (OidnStatus::LastPaths), kept with the run.
        static void NoteRun( uint32_t paths, uint32_t width, uint32_t height, double milliseconds );
        // A refusal the caller met with this denoiser's image (a bind or enqueue error, a graph that did not run
        // the pass), kept in Status().Error until the next successful run.
        static void NoteError( const std::string& error );

        [[nodiscard]] const std::string& GetDeviceName() const override
        {
            return m_DeviceName;
        }
        [[nodiscard]] const std::string& GetDeviceReason() const override
        {
            return m_DeviceReason;
        }

        [[nodiscard]] Common::BoolResultStr BindShared( const SharedDenoiseImages& images,
                                                        ImageDenoiseQuality        quality ) override;
        [[nodiscard]] Common::BoolResultStr EnqueueShared( uint64_t waitValue, uint64_t signalValue ) override;
        void                                ReleaseShared() override;

    private:
        struct Library;
        OidnDenoiser() = default;

        std::unique_ptr<Library>        m_Library;
        void*                           m_Device  = nullptr; // OIDNDevice
        Common::Scalability::GpuSharing m_Sharing = Common::Scalability::GpuSharing::None;
        // BindShared's imports: OIDNBuffer per SharedDenoiseImages::Buffers, OIDNFilter (albedo prefilter,
        // normal prefilter, beauty), OIDNSemaphore (null on Metal).
        std::vector<void*>       m_Buffers;
        std::vector<void*>       m_Filters;
        void*                    m_Semaphore = nullptr;
        std::array<uint8_t, 16>  m_VulkanUuid{};
        std::string              m_DeviceName;
        std::string              m_DeviceReason;
    };
} // namespace Desert::Graphic::Denoise
