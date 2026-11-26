#pragma once

#include <chrono>

namespace Common::Platform
{
    /// THE CPU TIME OF THE CALLING THREAD, as a std::chrono clock (C++20's TrivialClock requirements).
    ///
    /// A budget that guards against RUNAWAY work (a script watchdog) must count the work, not the wall: a thread
    /// that is preempted by cold shader compiles, waits for a page-in on a slow disk or blocks on the GPU spends
    /// no CPU, and a wall-clock budget kills a call that did nothing (GAMEPLAY-PROOF b_wf8: OnStart of a two-line
    /// function stopped at its entry after 250 ms of a 7150 ms frame). Time points of this clock are comparable
    /// only on the thread that took them.
    ///
    /// Windows: GetThreadTimes (kernel + user, 100 ns units, updated at the scheduler tick); POSIX:
    /// clock_gettime(CLOCK_THREAD_CPUTIME_ID).
    struct ThreadCpuClock
    {
        using duration                  = std::chrono::nanoseconds;
        using rep                       = duration::rep;
        using period                    = duration::period;
        using time_point                = std::chrono::time_point<ThreadCpuClock>;
        static constexpr bool is_steady = true;

        [[nodiscard]] static time_point now() noexcept;
    };
} // namespace Common::Platform
