#pragma once

// Ported from UE 5.8 Runtime/Core/Private/HAL/PThreadRunnableThread.h (FRunnableThreadPThread::_ThreadProc
// sets up the thread's crash-handling stack before Run() and releases it after), adapted: no FRunnable —
// a callable passed to std::thread, and the per-thread setup is Common::Crash::ThreadCrashStackScope.

#include <Common/Core/CrashHandler.hpp>

#include <functional>
#include <thread>
#include <type_traits>
#include <utility>

namespace Common
{
    // THE ONE WAY THE ENGINE STARTS A THREAD. A bare std::thread has no alternate signal stack on POSIX,
    // so a stack overflow on it kills the process without a crash report (CR1d); this wrapper runs the
    // callable inside a ThreadCrashStackScope. Same contract as the std::thread constructor otherwise:
    // arguments are decay-copied into the thread, the result is joinable.
    template <typename Function, typename... Arguments>
    NO_DISCARD std::thread StartEngineThread( Function&& inFunction, Arguments&&... inArguments )
    {
        return std::thread(
             []( std::decay_t<Function> inBody, std::decay_t<Arguments>... inArgs )
             {
                 const Crash::ThreadCrashStackScope crashStack;
                 std::invoke( std::move( inBody ), std::move( inArgs )... );
             },
             std::forward<Function>( inFunction ), std::forward<Arguments>( inArguments )... );
    }
} // namespace Common
