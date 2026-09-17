#pragma once

#include <cstdint>
#include <string>

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

} // namespace Common::Utils
