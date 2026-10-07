#pragma once

// IS A DEBUGGER ATTACHED TO THIS PROCESS — the question UE asks with FPlatformMisc::IsDebuggerPresent()
// before UE_DEBUG_BREAK. A breakpoint instruction with nobody attached is not a "break": on macOS
// __builtin_debugtrap() raises SIGTRAP and the process dies as "trace trap" before std::abort() is
// reached, so a failed DESERT_VERIFY left no crash report (DEV-CRASH1: the owner's Editor launched
// without MoltenVK's variables died on `Verify failed: glfwVulkanSupported()` with nothing written).
// DESERT_DEBUG_BREAK therefore traps only when this answers true; otherwise the failure goes straight
// down the crash path (std::abort -> the handler -> the report).
//
// HEADER-ONLY ON PURPOSE: Core.hpp's DESERT_VERIFY calls it, and Core.hpp is compiled into test suites
// and tools that do not all link Common; an out-of-line definition would turn every one of them into a
// link error. The platform is asked of the COMPILER (_WIN32 / __APPLE__ / __linux__), not of the
// per-project DESERT_PLATFORM_* define that suites forget (Core.hpp §DESERT_DEBUG_BREAK records why).
// It is called only on the failure path, so its cost (one sysctl / one /proc read) is irrelevant.

#if defined( _WIN32 )
// kernel32's own declaration, repeated so that every TU including Core.hpp does not pull <windows.h>.
// Identical to <debugapi.h>'s (BOOL is int, WINAPI is __stdcall), so a TU that includes both is fine.
extern "C" __declspec( dllimport ) int __stdcall IsDebuggerPresent( void );
#elif defined( __APPLE__ )
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined( __linux__ )
#include <cstdio>
#include <cstring>
#endif

namespace Common::Platform
{
    [[nodiscard]] inline bool IsDebuggerAttached()
    {
#if defined( _WIN32 )
        return ::IsDebuggerPresent() != 0;
#elif defined( __APPLE__ )
        // Apple's documented recipe (Technical Q&A QA1361): P_TRACED is set while a debugger is attached.
        int               mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>( ::getpid() ) };
        struct kinfo_proc info   = {};
        size_t            size   = sizeof( info );
        if ( ::sysctl( mib, 4, &info, &size, nullptr, 0 ) != 0 )
        {
            return false;
        }
        return ( info.kp_proc.p_flag & P_TRACED ) != 0;
#elif defined( __linux__ )
        // `TracerPid:\t<pid>` in /proc/self/status: non-zero while a ptrace-based debugger is attached.
        std::FILE* status = std::fopen( "/proc/self/status", "r" );
        if ( status == nullptr )
        {
            return false;
        }
        char line[256];
        bool traced = false;
        while ( std::fgets( line, sizeof( line ), status ) != nullptr )
        {
            if ( std::strncmp( line, "TracerPid:", 10 ) == 0 )
            {
                const char* digit = line + 10;
                while ( *digit == ' ' || *digit == '\t' )
                {
                    ++digit;
                }
                traced = *digit != '\0' && *digit != '0' && *digit != '\n';
                break;
            }
        }
        std::fclose( status );
        return traced;
#else
        // No way to ask: report "not attached", so a failed check takes the crash path and leaves a report
        // instead of executing a trap nobody catches.
        return false;
#endif
    }
} // namespace Common::Platform
