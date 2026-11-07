#pragma once

#include <Common/Core/CrashHandler.hpp>
#include <Common/Core/JobSystem.hpp>
#include <Engine/Assets/MeshDistanceField.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <memory>
#include <typeinfo>

// `pthread.h` is POSIX and does not exist under MSVC. `_WIN32` rather than the project's own
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
#include <intrin.h>
#endif

#ifndef _WIN32
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
inline void DesertTerminateBacktrace() noexcept
{
#ifndef _WIN32
    std::fprintf( stderr, "\n=== DESERT TERMINATE (thread %p) ===\n", (void*)pthread_self() );
#else
    std::fprintf( stderr, "\n=== DESERT TERMINATE (thread %lu) ===\n", ::GetCurrentThreadId() );
#endif

    // No rethrow-and-catch to read what(): engine code does not catch, and a job or a noexcept frame
    // that a std or ThirdParty throw reaches lands here with the throwing frames still live, so the stack
    // below is what names it.
    if ( std::current_exception() )
        std::fprintf( stderr, "an exception reached a noexcept frame (engine code does not throw)\n" );
    std::fflush( stderr );

    // Windows: a StackWalk64 walk named from the PDBs (UE: FWindowsPlatformStackWalk); POSIX: backtrace().
    Common::Crash::PrintCallingThreadStack();

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

#if defined( _WIN32 ) && defined( __AVX2__ )
// The engine is compiled with /arch:AVX2 (DESERT_AVX2). On a CPU without AVX2 (or an OS that does not save the
// YMM state) the first AVX2 instruction is an illegal-instruction crash with no message, so main says it first.
static bool DesertCpuHasAvx2() noexcept
{
    int info[4] = {};
    __cpuid( info, 0 );
    if ( info[0] < 7 )
        return false;
    __cpuid( info, 1 );
    const bool osxsave = ( info[2] & ( 1 << 27 ) ) != 0;
    const bool avx = ( info[2] & ( 1 << 28 ) ) != 0;
    const bool fma = ( info[2] & ( 1 << 12 ) ) != 0;
    if ( !osxsave || !avx || !fma )
        return false;
    if ( ( _xgetbv( 0 ) & 0x6 ) != 0x6 )
        return false;
    __cpuidex( info, 7, 0 );
    return ( info[1] & ( 1 << 5 ) ) != 0;
}
#endif

int main( int argc, char** argv )
{
#if defined( _WIN32 ) && defined( __AVX2__ )
    if ( !DesertCpuHasAvx2() )
    {
        (void)MessageBoxA( nullptr, "This build needs a CPU with AVX2 (Intel Haswell / AMD Zen or newer).",
                           "DesertEngine", MB_OK | MB_ICONERROR );
        return 1;
    }
#endif
#ifdef _WIN32
    // PER-MONITOR DPI AWARENESS, FIRST THING. It can only be declared before the process creates its first
    // window, and the native splash now does that before GLFW starts -- GLFW's own attempt then failed
    // silently, and Windows bitmap-stretched the whole editor to the display scale (owner, 2026-09-24:
    // "everything is too big"). UE declares the same, per monitor v2, in its application manifest.
    if ( SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ) == 0 )
        (void)SetProcessDPIAware();
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
        if ( app->StartupRefusal().empty() )
        {
            app->OnCreate();
            app->Run();
            app->OnDestroy();
        }

        // Read before the application is destroyed at the closing brace.
        exitCode = app->ExitCode();

        // The distance-field builds first (UE FDistanceFieldAsyncQueue::Shutdown at engine exit): the pool join
        // below waits for every running job, and a mesh build in a Debug editor runs for minutes - cancelled, it
        // stops at its next brick and the join is immediate.
        Desert::Assets::CancelMeshDistanceFields();
        Common::JobSystem::Get().Shutdown();
        if ( const auto device = Desert::EngineContext::GetInstance().GetDevice() )
            device->WaitIdle();
    }

    return exitCode;
}