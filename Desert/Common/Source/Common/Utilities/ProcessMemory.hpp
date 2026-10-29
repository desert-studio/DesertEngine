#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Common::Utils
{
    /**
     * @brief HOW MUCH MEMORY THIS PROCESS HOLDS, asked of the operating system.
     *
     * WHY THIS EXISTS, AND WHY IT IS NOT THE GPU LEDGER'S JOB. `Engine/Graphic/ResourceLedger.hpp` says
     * in its own words that "the only instrument for it was the process's RSS, which mixes device memory,
     * the asset layer's CPU copies and the allocator's own slack" — and then counts only what it can see,
     * which is device objects. Measured on this tree: loading a 50 179-entity scene moved the ledger's
     * byte total by 677 829 bytes (0.15 %) while the process grew by 315 MB. The ledger is not wrong; it
     * is answering a different question, and the difference between the two numbers is the answer nobody
     * had. So this is the SECOND of three numbers the memory readout prints, not a replacement for any.
     *
     * WHAT EACH FIELD IS, because "memory used" is four different numbers on every platform:
     *   - `Resident` is what is in physical RAM now (macOS `resident_size`, Windows `WorkingSetSize`,
     *     Linux `VmRSS`). It goes DOWN under pressure without anything being freed.
     *   - `Peak` is the high-water mark of the same quantity over the process's life. It never goes down,
     *     which is what makes it the number a "does it grow with the map" claim can rest on.
     *
     * A ZERO IS NOT A ZERO — it is "the platform did not answer", and `Known` says which. A readout that
     * printed 0 bytes resident would be read as a spectacularly small process rather than as a failed
     * syscall, which is the silent-substitution shape the delivery contract forbids.
     */
    struct ProcessFootprint
    {
        /// False when the platform query failed or is not implemented here. Then the bytes below are 0
        /// and mean nothing; a report must say "unknown" rather than print them.
        bool     Known    = false;
        uint64_t Resident = 0; ///< bytes in physical RAM right now
        uint64_t Peak     = 0; ///< high-water mark of the same quantity since process start
        /// What the OS charges the process for (macOS `phys_footprint`: dirty + compressed memory INCLUDING
        /// the GPU allocations of unified memory, the number jetsam and run_capped kill on; Windows
        /// `PrivateUsage`; Linux = `Resident`). `Resident` misses Metal device memory entirely, so a census
        /// keyed on it never fires at a GPU-memory peak (BISTRO-MEM-2: resident 1.7 GB at a 6.4 GB kill).
        uint64_t Physical = 0;

        /// One line for the log. Says "unknown" rather than "0" when `Known` is false.
        [[nodiscard]] std::string Describe() const;
    };

    /**
     * @brief Asks the OS, every call. Never cached.
     *
     * Not cached for the same reason `VK_EXT_memory_budget` must not be: the value is a fact about an
     * instant, and a cached one is a fact about whenever the cache was filled — which is exactly the
     * measurement mistake a memory detector exists to prevent. Costs one syscall (measured at ~2 us on
     * this machine), so a per-frame caller is affordable and a caching one would be saving nothing.
     */
    [[nodiscard]] ProcessFootprint ReadProcessFootprint();

    /**
     * @brief WHERE THE PROCESS'S CHARGED MEMORY LIVES, by the kind of virtual memory region holding it.
     *
     * The device census accounts for every byte the GPU allocator holds, yet the footprint it is keyed on was
     * 1.7 GB larger (PKG-RT-RES: footprint 5183 MB, driver usage 3412 MB) with nothing naming the rest. This walks
     * the address space the way `footprint(1)` / `vmmap -summary` do and charges each region's dirty and
     * compressed pages to its kind (the kernel's VM user tag): the malloc heaps, GPU driver mappings, stacks,
     * the writable data of loaded images, and so on. What the walk cannot see (GPU memory of private storage,
     * which is charged to the process but mapped nowhere in it) is the footprint minus the sum, printed as such.
     */
    struct FootprintKind
    {
        std::string Name;
        uint64_t    Bytes   = 0; ///< dirty + compressed bytes of the regions of this kind
        uint32_t    Regions = 0;
    };

    struct FootprintBreakdown
    {
        /// False where the platform has no region walk here (only macOS has one). Then Kinds is empty and a
        /// report must say "unknown".
        bool                       Known = false;
        std::vector<FootprintKind> Kinds;      ///< largest first
        uint64_t                   Walked = 0; ///< sum of Kinds
        /// Bytes in use / reserved by every malloc zone (malloc_zone_statistics), the heap's own view of the
        /// "malloc" kinds above: their difference is allocator slack.
        uint64_t HeapInUse     = 0;
        uint64_t HeapAllocated = 0;

        /// Lines for the log, @p footprint the Physical footprint read at the same moment.
        [[nodiscard]] std::string Describe( uint64_t footprint ) const;
    };

    /// Walks the whole address space: tens of milliseconds, for a census, never per frame.
    [[nodiscard]] FootprintBreakdown ReadFootprintBreakdown();

} // namespace Common::Utils
