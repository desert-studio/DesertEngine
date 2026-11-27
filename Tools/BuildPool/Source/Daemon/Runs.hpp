// The counters of one build ("a build" = buildRoot + runLabel) and its line of <home>/runs.tsv (format in
// BuildPool/Daemon.hpp). Written once, when the build has had no client for kRunIdleMs or the daemon stops.
#pragma once

#include "BuildPool/Platform.hpp"
#include "BuildPool/Result.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace buildpool::daemon
{
    inline constexpr uint64_t kRunIdleMs = 10'000; // Daemon.hpp: "has had no client for 10 s"

    struct RunRecord
    {
        uint64_t    startUnix = 0;
        uint64_t    endUnix   = 0;
        std::string buildRoot;
        std::string runLabel;
        std::string className;
        uint64_t    actions       = 0; // finished (not preempted) leases
        uint64_t    cacheReruns   = 0; // Verdict{runCacheAgain = true}
        uint64_t    waitedMsTotal = 0; // Acquire -> Grant or Retry, summed over the build's actions
        uint64_t    preemptions   = 0;
        uint32_t    maxRunning    = 0; // machine-wide leases, highest while the build was active
        // Machine-wide over the build's span: the lowest min(physicalAvailable, commitFree) sampled, and the
        // limit L it is measured against (Daemon.hpp runs.tsv): MemoryState::commitLimit when the samples carry
        // one, else physicalTotal. The highest L sampled is kept, so a page file that grows raises it.
        uint64_t lowestFreeBytes = std::numeric_limits<uint64_t>::max();
        uint64_t limitBytes      = 0;

        // One sample tick (or one Grant) while the build is active.
        void ObserveMachine( uint32_t runningLeases, const MemoryState* memory ) noexcept;
    };

    // min(physicalAvailable, commitFree) — the memory the pool plans against.
    uint64_t FreeBytes( const MemoryState& memory ) noexcept;

    // The runs.tsv line, '\n' included.
    std::string FormatRunLine( const RunRecord& run );

    // Appends the line; the file is rewritten atomically (readers never see half a line).
    Status AppendRun( const std::filesystem::path& file, const RunRecord& run ) noexcept;
} // namespace buildpool::daemon
