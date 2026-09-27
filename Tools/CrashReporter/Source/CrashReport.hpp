// The DESERTCRASH report model and its parser. NO UI AND NO WINDOW SYSTEM IN THIS HEADER, on
// purpose: the format contract lives in Desert/Common/Source/Common/Core/CrashHandler.cpp:41-75,
// this file is the only reader of it, and the reader has to stay testable from a console. When the
// UI moves to our own framework, Main.cpp is what gets rewritten and this file is what survives.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace CrashReporter
{
    // One [stack] line. Five fields, always — the writer emits empty ones rather than dropping them.
    struct StackFrame
    {
        std::string index;
        std::string address;
        std::string module;
        std::string function;
        std::string source; // "file:line", or empty when no line info resolved
    };

    struct Report
    {
        // False means: do not show a report, show `error`. `error` always names the path it tried.
        bool        valid = false;
        std::string error;

        // The path the text came from, filled even when parsing failed, so the window can say it.
        std::string sourcePath;
        // The whole file as read. This is what "Copy report" puts on the clipboard: the bytes the
        // owner would attach to a bug, not a re-rendering of them.
        std::string rawText;

        int formatVersion = 0;

        // [report]
        std::string host;
        std::string pid;
        std::string tid;
        std::string started;
        std::string crashEpoch;

        // [exception]
        std::string kind;
        std::string code;
        std::string codename;
        std::string address;
        std::string synthesized;
        std::string module;
        std::string moduleOffset;
        std::string function;
        std::string faultFrame;

        // [build]
        std::string version;
        std::string sha;
        std::string branch;
        std::string dirty;

        // [context]
        std::string machine;
        std::string scene;
        std::string os;
        std::string gpu;
        std::string gpuVendor; // CR1c: "0x10DE"; empty for a report written before the key existed
        std::string gpuDevice;
        std::string gpuDriver; // the vendor's own spelling, "591.86"
        std::string gpuApi;
        std::string game; // the game's Name, or "unread" when the process died before reading it

        std::vector<StackFrame>  frames;
        std::vector<std::string> log;

        // [end] written=complete. Absent ⇒ the process died mid-write; the report is still shown,
        // and the window says it is truncated rather than pretending it is whole.
        bool complete = false;
    };

    // Parses the text of a crash.txt. `inSourcePath` is only carried into the result for messages.
    Report ParseCrashText( const std::string& inText, const std::string& inSourcePath );

    // Reads <inDirectory>/crash.txt and parses it. Every failure comes back as an invalid Report
    // whose `error` names the full path — there is no silent empty result.
    Report LoadReport( const std::filesystem::path& inDirectory );

    // The human-readable crash time, derived from crash_epoch. Returns the raw field when it is not
    // a number, so a garbled value is visible rather than replaced by a plausible date.
    std::string FormatCrashTime( const Report& inReport );
} // namespace CrashReporter
