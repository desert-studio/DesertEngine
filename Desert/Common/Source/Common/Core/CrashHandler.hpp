#pragma once

// Ported from UE 5.8 Runtime/Core/Private/GenericPlatform/GenericPlatformCrashContext.cpp and
// Runtime/Core/Private/Windows/WindowsPlatformCrashContext.cpp (the structure: a context assembled
// BEFORE the fault, a single unhandled-exception filter that writes a minidump plus a textual
// context file into one per-crash directory, then hands that directory to an out-of-process
// reporter and terminates). Adapted: no UObject/FString/FPlatformMisc, no CrashReportClient XML —
// the context is a flat key=value text file (see the format block at the top of CrashHandler.cpp),
// the buffers are fixed-size C arrays owned by this module, and the POSIX arm writes through
// `write(2)` only so the same writer is legal inside a signal handler.

#include <Common/Core/Core.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Common::Crash
{
    // WHERE A REPORT GOES, DECIDED AT INSTALL (moved only by MoveReportRoot) AND NEVER FROM THE WORKING
    // DIRECTORY. A crash report written
    // relative to cwd lands in the source tree when the editor is started from the checkout and in
    // C:\Windows\System32 when it is started from a shortcut — two places nobody looks, and one of
    // them is under version control. `projectRoot` non-empty puts reports in `<projectRoot>/Saved/
    // Crashes`; empty means "this host has no project open" and reports go to the per-user location
    // (`%LOCALAPPDATA%/DesertEngine/Crashes`, `$HOME/.desertengine/Crashes` elsewhere).
    struct InstallOptions
    {
        // Names the process in the report ("Editor", "Runtime", "GamePackager", a suite's name).
        std::string hostName;

        // Project root, or empty for the per-user location. See above.
        std::filesystem::path projectRoot;

        // Overrides the report root outright, both branches above. Only tests pass this: a suite must
        // never write into the developer's real crash folder, and it must be able to assert on what it
        // wrote. Empty in every shipping host.
        std::filesystem::path reportRootOverride;

        // The CR2 reporter executable. Empty means "look for DesertCrashReporter[.exe] next to this
        // binary"; a report whose reporter is absent is still written and its path is printed to
        // stderr (see LaunchReporter's comment in the .cpp — this is the one permitted not-yet path
        // and it is an explicit logged branch, not a fallback).
        std::filesystem::path reporterExecutable;
    };

    // Installs the platform fault handlers and freezes the report location. Fails — rather than
    // installing half of itself — when the report root cannot be created, because a handler that
    // cannot write is worse than no handler: it turns a crash with a core dump into a crash with
    // nothing. Call exactly once per process, at startup, before anything that can fault.
    NO_DISCARD BoolResultStr Install( const InstallOptions& inOptions );

    // True between a successful Install() and process exit. Exposed so a host can refuse to arm the
    // "Debug > Crash (test)" command when the handler is not installed.
    bool IsInstalled();

    // MOVES THE REPORT ROOT after Install(), for a host that learns where its reports belong only after
    // work that can itself crash. The Runtime (PKG1c, UE's order: the handler before the project loads)
    // installs with no project — reports under the engine's per-user root — BEFORE it mounts the
    // archive and opens the .deproj, then moves to GameUserDirectory(<Name>)/Crashes, because a player's
    // install folder is read-only and every file a run writes belongs beside machine.json. The per-crash
    // directory keeps its name (start stamp + pid). Safe against a fault on another thread: the handler
    // reads a fully built path set published by one atomic store (see ReportPaths in the .cpp). Fails,
    // leaving the old root in force, when not installed or when `inNewRoot` cannot be created.
    NO_DISCARD BoolResultStr MoveReportRoot( const std::filesystem::path& inNewRoot );

    // The directory reports are written under, valid after Install(). Empty before it. Follows
    // MoveReportRoot.
    const std::filesystem::path& ReportRootDirectory();

    // THE CONTEXT IS FORMATTED HERE, NOT IN THE HANDLER. Both copy into a fixed buffer owned by this
    // module; the handler only memcpy's bytes out of it. Anything that formats — std::format, an
    // allocation, a lock — is unusable from a signal handler and unreliable from an unhandled
    // exception filter whose thread may have smashed its own stack. Values longer than the buffer are
    // truncated rather than dropped, since a truncated scene path still identifies the crash.
    void SetScenePath( std::string_view inScenePath );

    // THE GPU THE PROCESS ACTUALLY RENDERS ON, as the graphics API reported it for the device it created —
    // not the first DXGI adapter, which on a laptop is the integrated one. The OS half of the context is
    // filled by Install(); the GPU cannot be, because Common knows nothing of Vulkan: the host copies the
    // numbers out of VkPhysicalDeviceProperties right after the device exists. Until then every gpu* key
    // reads "unknown", which is therefore what a crash before device creation says. The packed values are
    // decoded here (DescribeDriverVersion / DescribeApiVersion) so the report carries readable versions.
    struct GpuIdentity
    {
        std::string_view name;              // deviceName
        std::uint32_t    vendorId      = 0; // PCI vendor id (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel)
        std::uint32_t    deviceId      = 0;
        std::uint32_t    driverVersion = 0; // packed, vendor-specific (see DescribeDriverVersion)
        std::uint32_t    apiVersion    = 0; // packed as VK_MAKE_API_VERSION
    };
    void SetGpu( const GpuIdentity& inGpu );

    // A driverVersion as the vendor prints it. NVIDIA packs 10/8/8/6 bits ("591.86"; the last two parts
    // are appended only when non-zero), Intel on Windows 18/14 ("101.5186"); every other vendor, and
    // Intel elsewhere, uses the API's own major.minor.patch packing.
    std::string DescribeDriverVersion( std::uint32_t inVendorId, std::uint32_t inPacked );

    // A VK_MAKE_API_VERSION value as "major.minor.patch".
    std::string DescribeApiVersion( std::uint32_t inPacked );

    // THE GAME THIS PROCESS RUNS, once the host has READ it (the Runtime: the packaged .deproj's Name).
    // Until then the report says game=unread — which is how a crash while the descriptor is still being
    // mounted and parsed is told apart from one inside a running game.
    void SetGameName( std::string_view inGameName );

    // THE OS THREAD ID the report's `tid=` carries: GetCurrentThreadId on Windows, pthread_threadid_np
    // on macOS, gettid on Linux — the number a debugger and the system's own crash log show, not
    // std::thread::id (opaque) and not the pid. Safe to call from the fault handler.
    std::uint64_t CurrentThreadId();

    // EVERY THREAD THE ENGINE STARTS HOLDS ONE OF THESE FOR ITS WHOLE LIFE (UE's Unix pattern: each
    // FRunnableThreadPThread gives itself a crash-handling stack before it runs, and frees it when it
    // ends). On POSIX the fault handler runs on the ALTERNATE SIGNAL STACK, and that stack is per
    // thread: a thread without one that overflows its stack takes SIGSEGV with no stack to run the
    // handler on, the kernel kills the process and no report is written. The constructor maps a stack
    // (with a guard page below it) and installs it for the calling thread; the destructor uninstalls
    // it and unmaps it. A thread that already has an alternate stack (the main thread, given one by
    // Install()) keeps it and the scope does nothing. On Windows the report is written by the report
    // thread, not on the faulting stack (CR1b), so the scope has nothing to do there.
    // Use Common::StartEngineThread (EngineThread.hpp) rather than holding one by hand; the JobSystem
    // workers hold one directly.
    class ThreadCrashStackScope
    {
    public:
        ThreadCrashStackScope();
        ~ThreadCrashStackScope();

        ThreadCrashStackScope( const ThreadCrashStackScope& )            = delete;
        ThreadCrashStackScope& operator=( const ThreadCrashStackScope& ) = delete;
        ThreadCrashStackScope( ThreadCrashStackScope&& )                 = delete;
        ThreadCrashStackScope& operator=( ThreadCrashStackScope&& )      = delete;

    private:
        void*       m_Mapping     = nullptr;
        std::size_t m_MappingSize = 0;
    };

    // A DELIBERATE CRASH, FOR PROVING THE HANDLER RATHER THAN HOPING. Reached from the editor's
    // "Debug > Crash (test)" command and from `--crash-test <kind>` in every host (kinds: kKnownTestKinds).
    enum class TestKind : std::uint8_t
    {
        Segv,          // null dereference -> EXCEPTION_ACCESS_VIOLATION / SIGSEGV
        Abort,         // std::abort()     -> SIGABRT
        PureCall,      // a pure virtual called from a base constructor -> purecall handler / SIGABRT
        StackOverflow, // unbounded recursion -> EXCEPTION_STACK_OVERFLOW / SIGSEGV on the alternate stack
        // The same recursion on a thread started by StartEngineThread: not the thread Install() ran on,
        // so the report depends on that thread's own ThreadCrashStackScope (POSIX) / the report
        // thread (Windows), and its `tid=` must name the worker.
        StackOverflowWorker,
        // The same recursion inside a JobSystem job: proves the pool's workers are prepared too.
        StackOverflowJob,
    };

    // Every spelling ParseTestKind accepts, for the hosts' "it knows: ..." error messages.
    inline constexpr const char* kKnownTestKinds =
         "segv, abort, purecall, stackoverflow, stackoverflow-worker, stackoverflow-job";

    // Parses the `--crash-test` argument. `nullopt` for an unknown word, so the caller can report the
    // word it did not understand instead of silently picking a default.
    std::optional<TestKind> ParseTestKind( std::string_view inWord );

    // The spelling accepted on the command line, for error messages and the editor's menu.
    const char* TestKindName( TestKind inKind );

    // Crashes the calling thread in the requested way. The named functions below appear in crash.txt's
    // `function=` field, which is what the CR1 suite asserts on.
    [[noreturn]] void TriggerTestCrash( TestKind inKind );
} // namespace Common::Crash
