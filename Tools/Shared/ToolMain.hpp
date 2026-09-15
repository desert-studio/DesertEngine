#pragma once

// A TOOL'S main IS THE LAST PLACE AN EXCEPTION CAN BE TURNED INTO A SENTENCE.
//
// `main` is not `noexcept`, but nothing encloses it: an exception that leaves it calls `std::terminate`,
// and the default handler prints `libc++abi: terminating due to uncaught exception of type X` and aborts
// with SIGABRT. The message names the TYPE and never the reason, the exit code is 134 rather than the
// tool's own, and a script that reads the tool's output sees neither.
//
// WHY THE ENGINE IS NOT IN THIS HEADER, and why the difference is deliberate. `Engine/EntryPoint.hpp`
// installs `std::set_terminate( &DesertTerminateBacktrace )`, which prints a backtrace THROUGH the throw
// site and exits 134 — strictly more than a catch here could say, because a catch has already unwound
// everything the backtrace would have named. A process that has that handler is better off without this
// one. The thirteen tools have no handler at all, which is what this closes.
//
// The tool keeps its own exit codes: only the exception path invents one, and 70 is EX_SOFTWARE from
// sysexits.h — "an internal software error", which is exactly what an exception nobody caught is.
//
// Header-only and dependency-free on purpose: six of the thirteen tools deliberately link nothing but a
// vendored stb, so that they still build on a machine that cannot build an engine.

#include <cstdio>
#include <exception>

namespace Desert::Tools
{
    /// Exit code for an exception that reached `main`. EX_SOFTWARE (sysexits.h).
    inline constexpr int kUncaughtExceptionExitCode = 70;

    /// Runs @p body as the whole of a tool's `main`, reporting anything it throws on stderr with the
    /// tool's name. @p body returns the tool's own exit code.
    template <typename Body>
    int RunMain( const char* toolName, int argc, char** argv, Body body ) noexcept
    {
        try
        {
            return body( argc, argv );
        }
        catch ( const std::exception& failure )
        {
            std::fprintf( stderr, "%s: unhandled exception: %s\n", toolName, failure.what() );
        }
        catch ( ... )
        {
            std::fprintf( stderr, "%s: unhandled exception of an unknown type\n", toolName );
        }
        return kUncaughtExceptionExitCode;
    }
} // namespace Desert::Tools
