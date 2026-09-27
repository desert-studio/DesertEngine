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

    // The GPU half of the OS/GPU line. The OS half is filled by Install(); the GPU cannot be, because
    // Common knows nothing of Vulkan — the host passes the adapter string once the device exists.
    void SetGpuDescription( std::string_view inGpuDescription );

    // A DELIBERATE CRASH, FOR PROVING THE HANDLER RATHER THAN HOPING. Reached from the editor's
    // "Debug > Crash (test)" command and from `--crash-test <segv|abort|purecall>` in every host.
    enum class TestKind : std::uint8_t
    {
        Segv,     // null dereference -> EXCEPTION_ACCESS_VIOLATION / SIGSEGV
        Abort,    // std::abort()     -> SIGABRT
        PureCall, // a pure virtual called from a base destructor -> purecall handler / SIGABRT
    };

    // Parses the `--crash-test` argument. `nullopt` for an unknown word, so the caller can report the
    // word it did not understand instead of silently picking a default.
    std::optional<TestKind> ParseTestKind( std::string_view inWord );

    // The spelling accepted on the command line, for error messages and the editor's menu.
    const char* TestKindName( TestKind inKind );

    // Crashes the calling thread in the requested way. The named functions below appear in crash.txt's
    // `function=` field, which is what the CR1 suite asserts on.
    [[noreturn]] void TriggerTestCrash( TestKind inKind );
} // namespace Common::Crash
