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
    // critical path and none of their waits spins on a thread below it (the census in OS-SCHED-HYGIENE) - and
    // every other engine thread (workers, the submit thread, IO, decoders, bakes) is Normal.
    enum class ThreadPriority : uint8_t
    {
        Normal,
        AboveNormal,
    };

    // NAMES THE CALLING THREAD AND SETS ITS PRIORITY (UE FRunnableThreadWin::CreateInternal: SetThreadDescription
    // and SetThreadPriority on the new thread). The name is what a debugger, a profiler capture, a crash dump and
    // the '[Threads]' census show (Windows SetThreadDescription, POSIX pthread_setname_np); threads of one role
    // share a name up to " #" ("Job Worker #3"), and the census counts them as one row. The thread is listed in
    // the census until it ends. Windows: THREAD_PRIORITY_ABOVE_NORMAL; macOS: QOS_CLASS_USER_INTERACTIVE; Linux
    // without the privilege keeps its priority, and the census says the priority was not applied.
    void NameCurrentThread( std::string_view name, ThreadPriority priority ) noexcept;

    // THE '[Threads]' CENSUS: every engine thread alive now as "name priority xcount" rows, in the order they were
    // named, and on Windows how many threads of the process are not the engine's (the driver's, the OS's, a
    // third-party pool's - a second job pool shows here as a jump of that count).
    [[nodiscard]] std::string DescribeEngineThreads();

    // EcoQoS OPT-OUT (UE: FWindowsPlatformProcess's PROCESS_POWER_THROTTLING_EXECUTION_SPEED off at startup).
    // Windows 11 runs a process whose window is not in the foreground on efficiency cores at a lower clock; a game
    // or an editor running a scenario without focus would then measure a different machine. Windows only; on other
    // platforms there is no such throttling to opt out of and this returns true.
    [[nodiscard]] BoolResultStr OptOutOfPowerThrottling() noexcept;

    // THE ONE WAY THE ENGINE STARTS A THREAD. A bare std::thread has no alternate signal stack on POSIX,
    // so a stack overflow on it kills the process without a crash report (CR1d); this wrapper runs the
    // callable inside a ThreadCrashStackScope, after naming the thread and setting its priority
    // (NameCurrentThread). Same contract as the std::thread constructor otherwise: arguments are decay-copied
    // into the thread, the result is joinable.
    template <typename Function, typename... Arguments>
    NO_DISCARD std::thread StartEngineThread( std::string name, const ThreadPriority priority,
                                              Function&& inFunction, Arguments&&... inArguments )
    {
        return std::thread(
             []( std::string inName, const ThreadPriority inPriority, std::decay_t<Function> inBody,
                 std::decay_t<Arguments>... inArgs )
             {
                 NameCurrentThread( inName, inPriority );
                 const Crash::ThreadCrashStackScope crashStack;
                 std::invoke( std::move( inBody ), std::move( inArgs )... );
             },
             std::move( name ), priority, std::forward<Function>( inFunction ),
             std::forward<Arguments>( inArguments )... );
    }
} // namespace Common
