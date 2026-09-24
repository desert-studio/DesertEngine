#pragma once

#include <Common/Core/JobSystem.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <memory>
#include <typeinfo>

// `execinfo.h` is POSIX and does not exist under MSVC. `_WIN32` rather than the project's own
// DESERT_PLATFORM_WINDOWS: this is a question about the toolchain's headers, and the compiler is the
// authority on that whether or not the build system remembered to say so.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef _WIN32
#include <execinfo.h>
#include <pthread.h>
#endif

// Returns the application BY VALUE-OWNERSHIP on purpose. It used to hand back a raw pointer into
// Common::Singleton<T>, a namespace-scope static std::unique_ptr, and that single fact was the whole of
// the exit-time crash: a namespace-scope static registers its destructor before main, so it is destroyed
// LAST — after spdlog, after the job system, and after every library the Vulkan loader dlopen'ed during
// main. The application, which owns the window, the device and every GPU resource, was therefore torn
// down in a process where the things it tears down through no longer existed. Owned here, it dies inside
// main, while they are all still alive.
extern std::unique_ptr<Desert::Engine::Application> CreateApplication( int argc, char** argv );

/**
 * @brief Prints the exception and the THROWING stack when an exception escapes to std::terminate.
 *
 * WHY THIS EXISTS. An uncaught exception in this engine used to produce exactly one line —
 * `libc++abi: terminating due to uncaught exception of type ...` — and nothing else: macOS writes no
 * crash report for an abort whose signature it has already seen, and a defect that appears in one run
 * in fifty does not survive being run under a debugger. A rare abort in the volumetric cloud phase cost
 * a whole task for precisely that reason and was handed on unsolved (Docs/Clouds/CALIBRATION.md §A0).
 * Thirty lines here would have named it in one run.
 *
 * WHY THE STACK IS STILL THERE. For an UNCAUGHT exception libc++abi calls std::terminate from the
 * point of the throw, without unwinding first — the standard permits either and libc++ chooses this
 * one — so the frames that threw are live and `backtrace()` walks them.
 *
 * `_Exit` rather than `abort`: the frames have been printed, and abort would run the same teardown that
 * is already known to fault on the way out, burying the message that was the point.
 */
inline void DesertTerminateBacktrace()
{
#ifndef _WIN32
    void*     frames[128];
    const int count = backtrace( frames, 128 );
    std::fprintf( stderr, "\n=== DESERT TERMINATE (thread %p) ===\n", (void*)pthread_self() );
#else
    std::fprintf( stderr, "\n=== DESERT TERMINATE ===\n" );
#endif

    // The exception itself is worth printing on EVERY platform even where the stack is not available:
    // `what()` is what says `vector` rather than merely `std::length_error`, and that one word is what
    // pointed at a torn container rather than at a bad argument.
    if ( auto ptr = std::current_exception() )
    {
        try
        {
            std::rethrow_exception( ptr );
        }
        catch ( const std::exception& e )
        {
            std::fprintf( stderr, "uncaught %s: %s\n", typeid( e ).name(), e.what() );
        }
        catch ( ... )
        {
            std::fprintf( stderr, "uncaught non-std exception\n" );
        }
    }
    std::fflush( stderr );

#ifndef _WIN32
    backtrace_symbols_fd( frames, count, 2 );
#endif

    std::fprintf( stderr, "=== END DESERT TERMINATE ===\n" );
    std::fflush( stderr );
    std::_Exit( 134 );
}

#ifdef _WIN32
// THE EDITOR IS A WINDOWS-SUBSYSTEM PROGRAM, SO NO CONSOLE WINDOW OPENS WITH IT (owner, 2026-09-24). A
// console is asked for with `--console`. Output that is redirected (a pipe, a file, CI) needs no console:
// the CRT picks the inherited handles up at start either way.
static void OpenConsoleIfAsked( const int argc, char** argv )
{
    for ( int i = 1; i < argc; ++i )
    {
        if ( std::strcmp( argv[i], "--console" ) != 0 )
            continue;
        if ( AllocConsole() == 0 )
            return;
        FILE* ignored = nullptr;
        (void)freopen_s( &ignored, "CONOUT$", "w", stdout );
        (void)freopen_s( &ignored, "CONOUT$", "w", stderr );
        (void)freopen_s( &ignored, "CONIN$", "r", stdin );
        return;
    }
}
#endif

int main( int argc, char** argv )
{
#ifdef _WIN32
    OpenConsoleIfAsked( argc, argv );
#endif
    // Before anything can throw, and before the logger exists: the handler writes to stderr directly so
    // that it still works when the failure is the logger's own.
    std::set_terminate( &DesertTerminateBacktrace );

    Common::Logger::LogInit();

    // Ordered teardown BEFORE static destructors run (their cross-TU order is undefined):
    // 1) join the worker pool while the logger/engine objects its jobs touch are still alive;
    // 2) drain the GPU so descriptor pools/buffers destroyed during teardown are no longer referenced by
    //    in-flight command buffers (the exit-time VUID-vkDestroyDescriptorPool-00303 followed by worker
    //    threads aborting on destroyed mutexes);
    // 3) destroy the application itself. This step is the point of the block and used to be missing: the
    //    application was left to a namespace-scope static, i.e. to __cxa_finalize, and the two faults
    //    every headless capture ended in both came from being there.
    int exitCode = 0;
    {
        auto app = CreateApplication( argc, argv );
        app->OnCreate();
        app->Run();
        app->OnDestroy();

        // Read before the application is destroyed at the closing brace.
        exitCode = app->ExitCode();

        Common::JobSystem::Get().Shutdown();
        if ( const auto device = Desert::EngineContext::GetInstance().GetDevice() )
            device->WaitIdle();
    }

    return exitCode;
}