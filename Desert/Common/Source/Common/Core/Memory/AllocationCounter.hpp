#pragma once

#include <cstdint>

// THE ALLOCATION COUNTER (ARCH-AUDIT C; UE LLM / Insights memory trace, reduced to the three numbers a frame
// needs). The process' global operator new / delete (AllocationCounter.cpp, the one replacement in the process)
// count every C++ heap allocation into the calling thread's own slot: a relaxed load + store on a cache line no
// other thread writes, so the cost is a TLS read and two plain stores. The profiler reads the sum once per frame
// (BeginFrame) and publishes the averaged per-frame figures beside the frame time ("[Profiler] heap" in the dump).
// Counted: operator new / new[] in every form (plain, nothrow, aligned) and the deletes of non-null pointers.
// Not counted: direct malloc/free calls (C libraries, mimalloc's own) and other modules' heaps (vendor DLLs).
namespace Common::Memory
{
    struct AllocationCounts
    {
        uint64_t Allocations = 0; // operator new calls
        uint64_t Bytes       = 0; // bytes those calls asked for
        uint64_t Frees       = 0; // operator delete calls with a non-null pointer
    };

    // Every thread's counts since the process started, summed. Monotonic; a frame's figure is the difference of
    // two reads. Lock-free, allocation-free.
    [[nodiscard]] AllocationCounts ReadAllocationCounts() noexcept;
} // namespace Common::Memory
