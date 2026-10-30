#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/CapabilityCatalog.hpp>
#include <Engine/Graphic/RDG/RDGCompileResult.hpp>

#include <cstdint>

namespace Desert::Graphic
{
    namespace RDG
    {
        struct ExternalBuffer;
    } // namespace RDG

    // GPU MEMORY AND GPU ORDER SHARED WITH ANOTHER API ON THE SAME GPU (CUDA / HIP / SYCL through Intel OIDN; the
    // catalog's GpuSharing says which handle type, CapabilityCatalog::Sharing). The engine allocates and owns the
    // memory and the semaphore; the other API imports the handle and reads / writes the same bytes, so nothing
    // goes through the host. Made by Renderer::CreateSharedBuffer / CreateSharedTimeline, which refuse by name on
    // a device whose catalog says GpuSharing::None. API-neutral: the Vulkan backend's are
    // API/Vulkan/VulkanSharedGpuMemory.

    // One exported handle, as the importing API takes it. Win32: an NT handle the exporter keeps and closes (the
    // importer does not take it: OIDN's FromWin32Handle). Fd: a fresh descriptor the importer takes on success
    // (OIDN's FromFD) and the caller closes on failure. Metal: the MTLBuffer (MoltenVK, VK_EXT_metal_objects).
    struct SharedHandle
    {
        Common::Scalability::GpuSharing Kind  = Common::Scalability::GpuSharing::None;
        void*                           Win32 = nullptr;
        int                             Fd    = -1;
        void*                           Metal = nullptr;
        // The size of the memory object behind the handle (a buffer: its dedicated allocation, which may be larger
        // than Bytes(); a semaphore: 0). CUDA's import names the whole allocation.
        uint64_t AllocationBytes = 0;
        // The memory object is a dedicated allocation of one buffer (OIDN_EXTERNAL_MEMORY_TYPE_FLAG_DEDICATED).
        bool Dedicated = false;
    };

    // A device-local storage buffer, exportable. The graph sees it as an imported buffer (Import), its access
    // state carried across graphs by the buffer itself, like Renderer::ImportBuffer's.
    class SharedGpuBuffer
    {
    public:
        virtual ~SharedGpuBuffer() = default;

        // The bytes the graph addresses (the requested size).
        [[nodiscard]] virtual uint64_t Bytes() const = 0;

        // The handle for the other API. Win32: the same handle on every call, owned by this buffer. Fd: a fresh
        // descriptor per call (the importer takes ownership).
        [[nodiscard]] virtual Common::ResultStr<SharedHandle> ExportHandle() = 0;

        // This buffer in @p into for one graph: its size, the graph's non-owning handle, the state the last graph
        // left it in, and the hook through which Execute records the final state back.
        [[nodiscard]] virtual Common::BoolResultStr Import( RDG::ExternalBuffer& into ) = 0;
    };

    // A timeline semaphore, exportable: the order between the graph's queue and the other API
    // (RDG::ExternalQueueSync). The values only grow.
    class SharedGpuTimeline
    {
    public:
        virtual ~SharedGpuTimeline() = default;

        // The backend handle RDG::ExternalTimelinePoint::Semaphore names (Vulkan: the VkSemaphore as uint64).
        [[nodiscard]] virtual uint64_t Semaphore() const = 0;

        // The handle for the other API (same ownership rule as SharedGpuBuffer::ExportHandle).
        [[nodiscard]] virtual Common::ResultStr<SharedHandle> ExportHandle() = 0;

        // The next pair of points for one ExternalQueue pass: the graph signals 2n+1, the other API waits it and
        // signals 2n+2, the graph waits that. A pair whose frame was abandoned is never reused: the next pair is
        // above it, so nothing waits on a value nobody will signal.
        [[nodiscard]] RDG::ExternalQueueSync NextPair()
        {
            const uint64_t signal = 2u * m_Pairs + 1u;
            ++m_Pairs;
            return RDG::ExternalQueueSync{ .Signal = { .Semaphore = Semaphore(), .Value = signal },
                                           .Wait   = { .Semaphore = Semaphore(), .Value = signal + 1u } };
        }

    private:
        uint64_t m_Pairs = 0;
    };
} // namespace Desert::Graphic
