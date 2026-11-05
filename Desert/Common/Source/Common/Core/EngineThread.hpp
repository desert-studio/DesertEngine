#pragma once

// Ported from UE 5.8 Runtime/Core/Private/HAL/PThreadRunnableThread.h (FRunnableThreadPThread::_ThreadProc
// sets up the thread's crash-handling stack before Run() and releases it after), adapted: no FRunnable —
// a callable passed to std::thread, and the per-thread setup is Common::Crash::ThreadCrashStackScope.

#include <Common/Core/CrashHandler.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace Common
{
    // THE OS PRIORITY OF AN ENGINE THREAD BY ITS ROLE (UE EThreadPriority, set by FRunnableThread::Create). Two
    // values because two are used: the game and render threads are AboveNormal - each owns a core of the frame's
    // critical path, and the job workers are cores - 2 and keep the rest - and every other engine thread (workers,
    // the submit thread, IO, decoders, bakes) is Normal. Where the game thread yield-spins on a worker
    // (AsyncAssetLoader's FlushOne/AwaitOne) or on the clock (FramePacer's last 0.5 ms), the yield is
    // SwitchToThread on Windows, which hands the core to a ready thread of any priority: no inversion.
    enum class ThreadPriority : uint8_t
    {
        Normal,
        AboveNormal,
    };

    // NAMES THE CALLING THREAD AND SETS ITS PRIORITY (UE FRunnableThreadWin::CreateInternal: SetThreadDescription
    // and SetThreadPriority on the new thread). The name is what a debugger, a profiler capture, a crash dump and
    // the '[Threads]' census show (Windows SetThreadDescription, resolved at run time and skipped on a Windows
    // older than 10 1607; POSIX pthread_setname_np, cut to the OS's limit); threads of one role share a name up to
    // " #" ("Job Worker #3"), and the census counts them as one row. The thread is listed in the census until it
    // ends. AboveNormal is THREAD_PRIORITY_ABOVE_NORMAL on Windows and QOS_CLASS_USER_INTERACTIVE on macOS; Linux
    // does not raise it (that needs CAP_SYS_NICE), and the census says the priority was not applied.
    void NameCurrentThread( std::string_view name, ThreadPriority priority ) noexcept;

    // THE '[Threads]' CENSUS: every engine thread alive now as "name priority xcount" rows, in the order they were
    // named, and on Windows how many threads of the process are not the engine's (the driver's, the OS's, a
    // third-party pool's - a second job pool shows here as a jump of that count).
    [[nodiscard]] std::string DescribeEngineThreads();

    // EcoQoS OPT-OUT: PROCESS_POWER_THROTTLING_EXECUTION_SPEED controlled and off for the process, the
    // documented opt-out (SetProcessInformation(ProcessPowerThrottling)). Windows 11 runs a process whose window
    // is not in the foreground on efficiency cores at a lower clock; a game or an editor running a scenario
    // without focus would then measure a different machine. The value: true = opted out (or, off Windows, there
    // is no such throttling); false = skipped, the running Windows exports no SetProcessInformation (resolved at
    // run time; older than 8, which has no EcoQoS either); an error = the call failed. The process's one caller
    // (Application) says a false or an error in the log, once.
    [[nodiscard]] BoolResultStr OptOutOfPowerThrottling() noexcept;

    // THE ONE WAY THE ENGINE STARTS A THREAD. A bare std::thread has no alternate signal stack on POSIX,
    // so a stack overflow on it kills the process without a crash report (CR1d); this wrapper runs the
    // callable inside a ThreadCrashStackScope, after naming the thread and setting its priority
    // (NameCurrentThread). The entry is noexcept, asserted: a thread returns its errors as values, and a throw
    // out of it would end the process in std::thread's own terminate with no word of where it came from. Same
    // contract as the std::thread constructor otherwise: arguments are decay-copied into the thread, the result
    // is joinable.
    template <typename Function, typename... Arguments>
    NO_DISCARD std::thread StartEngineThread( std::string name, const ThreadPriority priority,
                                              Function&& inFunction, Arguments&&... inArguments )
    {
        static_assert( std::is_nothrow_invocable_v<std::decay_t<Function>, std::decay_t<Arguments>...>,
                       "StartEngineThread: the thread's entry must be noexcept (a lambda: '() noexcept')" );
        return std::thread(
             []( std::string inName, const ThreadPriority inPriority, std::decay_t<Function> inBody,
                 std::decay_t<Arguments>... inArgs ) noexcept
             {
                 NameCurrentThread( inName, inPriority );
                 const Crash::ThreadCrashStackScope crashStack;
                 std::invoke( std::move( inBody ), std::move( inArgs )... );
             },
             std::move( name ), priority, std::forward<Function>( inFunction ),
             std::forward<Arguments>( inArguments )... );
    }
} // namespace Common
