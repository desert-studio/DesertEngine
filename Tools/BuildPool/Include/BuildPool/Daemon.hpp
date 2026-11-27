// BuildPool contract: the daemon. One per pool home; it owns every slot of every build that names that home.
// Implemented by package A in Source/Daemon/. The client (C) calls RunDaemon for `buildpool daemon`; the
// integration (D) reads the files documented at the end of this header.
// CONTRACT HEADER: written by the architect, edited by no package.
#pragma once

#include "BuildPool/Platform.hpp"
#include "BuildPool/Result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace buildpool
{
    // A priority class of builds. Classes are data of <home>/pool.ini; no class name appears in the code.
    struct ClassPolicy
    {
        std::string name;
        int priority = 0;             // lower runs first
        uint32_t guaranteedSlots = 1; // slots a build of this class gets even while higher classes wait
        OsPriority osPriority = OsPriority::Normal;
    };

    // <home>/pool.ini. A missing file is written with the defaults below and the daemon says so in its log;
    // a file that does not parse, names an unknown key or has no [class.*] section is an Error (the daemon
    // does not start and every client fails with the reason).
    //
    //   [pool]
    //   reserve_mb      = <max(2048, 10 % of physical memory)>   memory the pool never plans to use
    //   cpu_reserve     = 1          logical CPUs left to everything that is not a build
    //   window_factor   = 2          `buildpool window` = (logical CPUs - cpu_reserve) * window_factor
    //   estimate_margin = 1.15       an action is planned at (largest of its last 3 peaks) * this
    //   sample_ms       = 500        the Guard tick and, through Grant::sampleMs, every lease's Heartbeat period
    //   default_class   = normal
    //   [class.normal]
    //   priority = 5
    //   guaranteed_slots = 1
    //   os_priority = normal         normal | below_normal | idle
    struct PoolConfig
    {
        uint64_t reserveBytes = 0;
        uint32_t cpuReserve = 1;
        uint32_t windowFactor = 2;
        double estimateMargin = 1.15;
        uint32_t sampleMs = 500;
        std::string defaultClass;
        std::vector<ClassPolicy> classes;
    };
    Result<PoolConfig> LoadConfig(const std::filesystem::path& home) noexcept;

    // Runs until a Query::Stop is served or the endpoint is lost. Returns the process exit code: 0 after a stop,
    // 2 when another daemon already holds the endpoint ("already running": the caller treats it as success),
    // 1 with the reason in <home>/daemon.log otherwise. Never throws.
    int RunDaemon(const std::filesystem::path& home) noexcept;

    // ---- the rules RunDaemon implements (the design's «Архитектура», normative) --------------------------------------
    // Slots     S = LogicalCpuCount() - cpuReserve, machine-wide; a lease of any kind holds one.
    // Estimate  est(costKey) = (largest of its last 3 recorded peaks) * estimateMargin. A key never seen is planned
    //           at the largest estimate of its class ("<tool>|<action>"). A class never seen runs ONE action at a
    //           time, planned at the largest estimate in the ledger (0 when the ledger is empty), until its first
    //           peak is recorded (slow start; no constant stands in for a measurement).
    // Headroom  H = min(physicalAvailable, commitFree if present) - reserveBytes
    //               - sum over running leases of max(0, est - last Heartbeat::currentBytes).
    //           An action is admitted only when a slot is free and est <= H. ONE exception: when no lease is
    //           running machine-wide and the Guard does not hold admission, the first action in Order is admitted
    //           whatever its estimate, and daemon.log names it with est and H. Without it an estimate above
    //           what the machine can ever free (a key raised x 1.5 by a Preempt) would wait forever. Nothing is
    //           admitted beside it until est <= H holds again (H counts it from its Grant).
    // Order     waiting actions are served by (class priority, then the build with the fewest running leases,
    //           then arrival). A build holding fewer leases than its class's guaranteedSlots is served first
    //           whatever its priority (at most S/2 slots are given this way).
    //           Dedup never inverts the order: a Queued action that leads an inflightKey is served at
    //           min(its own class priority, the best class priority among that key's Awaiting waiters). Only the
    //           priority term is lent; its build's lease count, its arrival and guaranteedSlots stay its own, and
    //           its Grant carries its own class's osPriority.
    // Backfill  when the first action in that order does not fit H, its start time is planned from the ledger's
    //           wall times of the running leases (EASY backfilling); a later action is admitted before it only
    //           if it fits now and either ends before that time or leaves the first action's estimate free at it.
    // Dedup     the first lease granted under an inflightKey is its leader; a later Acquire with the same key is
    //           answered Await and holds no slot. When the leader's SESSION ends (its Front sent CacheExited or
    //           closed): leader released with exit code 0 -> every waiter gets Retry; otherwise the oldest waiter
    //           is granted a lease as the new leader.
    // Guard     every sampleMs: if min(physicalAvailable, commitFree) < reserveBytes or osPressure, nothing is
    //           admitted; if it is below reserveBytes / 2, ONE restartable lease is sent Preempt: lowest class
    //           first, then the least cpuMs spent (the lease's last Heartbeat::cpuMs; 0 before its first
    //           Heartbeat). Its key's estimate becomes max(estimate, observed * 1.5) and
    //           the action returns to the head of its build's queue. An action preempted twice is never preempted
    //           again.
    // Loss      a client whose connection closes has released everything it held, at once.

    // ---- files under <home> (written atomically by the daemon; read by scripts and the acceptance scenario) -------
    //   pool.ini      PoolConfig
    //   daemon.log    start, stop, refusals, every Preempt, every admission over Headroom, every config value
    //                 in effect
    //   ledger.tsv    one line per costKey, rewritten every 30 s and on stop:
    //                 cost_key  peak_mb_1  peak_mb_2  peak_mb_3  wall_ms  cpu_ms  samples
    //   runs.tsv      one line appended when a build (buildRoot + runLabel) has had no client for 10 s:
    //                 start_unix  end_unix  build_root  run_label  class  actions  cache_reruns
    //                 waited_ms_total  preemptions  max_running  peak_commit_used_mb  min_headroom_mb
    //                 (max_running, peak_commit_used_mb, min_headroom_mb are machine-wide over the run's time span;
    //                  peak_commit_used_mb = L minus the lowest min(physicalAvailable, commitFree) sampled, where
    //                  L = MemoryState::commitLimit when the samples carry one (Windows: always) and
    //                  physicalTotal only where the kernel has no commit limit; min_headroom_mb is that lowest
    //                  sample itself)
} // namespace buildpool
