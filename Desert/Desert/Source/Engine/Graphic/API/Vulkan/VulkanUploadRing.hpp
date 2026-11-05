#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <variant>

namespace Desert::Graphic::API::Vulkan
{
    /**
     * THE STREAM-IN STAGING RING (CPU-STREAM-UPLOAD-RING; UE FRHIResourceUpdateBatcher and the async reallocation
     * of FTexture2DStreamIn; NVIDIA "Vulkan Dos and Don'ts": staging in system memory, few submits; VMA
     * "Recommended usage patterns": AUTO_PREFER_HOST + HOST_ACCESS_SEQUENTIAL_WRITE, so the bytes live in system
     * RAM and never in the small BAR heap).
     *
     * ONE persistently mapped host buffer of kSlices x kSliceBytes, made once, on the first reservation. Every
     * texture upload used to allocate a staging buffer of its own (a dedicated vkAllocateMemory past 16 MB), map
     * it, copy into it and free it after the GPU: 2.1-2.5 ms of main thread for a 5.6 MB texture. A slice of the
     * ring costs a mutex and a few compares; the cook worker writes the levels into it, so the main thread never
     * touches the bytes.
     *
     * A SLICE'S LIFE. Reserve hands out [Offset, Offset + Size) in reservation order (wrapping to 0 when the tail
     * of the buffer is too short, the skipped bytes count as in flight until the slice before them retires). The
     * slice is retired by the last holder of its lease: the image whose levels it holds (until its last copy is
     * recorded) and every GPU batch that copies out of it (GpuBatch::Retain, dropped only once the batch's
     * submission has reached its value on the upload timeline that the submit thread signals,
     * CommandBufferAllocator::IsComplete). Slices retire out of order; the ring's free space grows when its OLDEST
     * live slice retires, as in any ring.
     *
     * FULL IS A VALUE, NOT A WAIT. A reservation that does not fit answers Full (and counts it); nothing here
     * blocks on the GPU. NeverFits says the bytes are larger than the whole ring, so retrying cannot help: the
     * caller splits the upload by levels (VulkanImage2D::RecordUploadStep).
     */
    class VulkanUploadRing
    {
    public:
        static constexpr uint64_t kSliceBytes = 64ull * 1024ull * 1024ull;
        static constexpr uint32_t kSlices     = 3;
        static constexpr uint64_t kCapacity   = kSliceBytes * kSlices;
        /// Every slice starts on this boundary: a multiple of every texel block size (16), of 4 (the copy rule
        /// for any format) and of the optimalBufferCopyOffsetAlignment drivers report (<= 256).
        static constexpr uint64_t kAlignment = 256;

        struct Slice
        {
            VkBuffer   Buffer = VK_NULL_HANDLE;
            uint64_t   Offset = 0;
            uint64_t   Size   = 0;
            std::byte* Mapped = nullptr; // Buffer's mapping at Offset: the first byte the caller writes
            /// Retires the slice when the last copy of it is dropped. Hand a copy to each batch that reads it.
            std::shared_ptr<const void> Lease;
        };
        struct Full
        {
            uint64_t Asked     = 0;
            uint64_t InFlight  = 0;
            uint64_t Capacity  = 0;
            bool     NeverFits = false; // larger than the ring, or the ring could not be made
        };
        using Reservation = std::variant<Slice, Full>;

        /// The process's ring. Made on the first Reserve; destroyed by VulkanContext::Shutdown (Destroy).
        static VulkanUploadRing& Get() noexcept;

        /// A slice of @p size bytes, or why not. Any thread; never waits for the GPU.
        Reservation Reserve( uint64_t size ) noexcept;

        struct Stats
        {
            uint64_t BytesInFlight     = 0;
            uint64_t PeakBytesInFlight = 0;
            uint64_t Reservations      = 0;
            uint64_t FullCount         = 0;
        };
        [[nodiscard]] Stats GetStats() const noexcept;

        /// Frees the buffer. The device is idle by then (CommandBufferAllocator::Destroy waited every upload);
        /// a lease dropped later retires into nothing.
        void Destroy() noexcept;

    private:
        struct Live
        {
            uint64_t Id      = 0;
            uint64_t Offset  = 0;
            uint64_t Size    = 0; // with the bytes a wrap skipped before it
            bool     Retired = false;
        };

        bool CreateLocked() noexcept;
        void RetireLocked( uint64_t id ) noexcept;
        void Retire( uint64_t id ) noexcept;
        void LogLocked() noexcept;

        mutable std::mutex m_Mutex;
        VkBuffer           m_Buffer     = VK_NULL_HANDLE;
        VmaAllocation      m_Allocation = nullptr;
        std::byte*         m_Mapped     = nullptr;
        bool               m_Refused    = false; // the buffer could not be made: every Reserve is NeverFits
        uint64_t           m_Head       = 0;
        std::deque<Live>   m_Live;
        uint64_t           m_NextId = 0;
        Stats              m_Stats;
        Stats              m_Logged;
        std::chrono::steady_clock::time_point m_LastLog{};
    };
} // namespace Desert::Graphic::API::Vulkan
