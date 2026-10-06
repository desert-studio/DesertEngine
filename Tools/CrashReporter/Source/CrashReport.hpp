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
        std::string config; // "Debug" / "Release" / "Shipping"; empty when the report was cut off before it
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

    // WHO THE WINDOW IS FOR (CR2b). Chosen by the report's own `config` key, never by how the reporter
    // was built: the writer already left the developer-only fields out of a Shipping report, and the
    // player view is the reporter not asking for them. Anything but "Shipping" — including a report
    // cut off before [build] — is the developer view, which can only show what the file holds.
    enum class Audience
    {
        Developer,
        Player
    };

    // One row of the Summary card, in the order drawn.
    struct SummaryLine
    {
        std::string label;
        std::string value;
        bool        mono = false; // drawn in the monospace font
    };

    // Everything the window draws from the report, decided here so the census of rows is testable
    // without a window. Main.cpp draws this and nothing else from the report's fields.
    struct ReportView
    {
        Audience                 audience = Audience::Developer;
        std::vector<SummaryLine> summary;
        bool                     showLog  = true; // the "Log tail" tab
        bool                     showPath = true; // the report's directory in the footer
    };

    Audience   AudienceOf( const Report& inReport );
    ReportView ComposeView( const Report& inReport );

    // Parses the text of a crash.txt. `inSourcePath` is only carried into the result for messages.
    Report ParseCrashText( const std::string& inText, const std::string& inSourcePath );

    // Reads <inDirectory>/crash.txt and parses it. Every failure comes back as an invalid Report
    // whose `error` names the full path — there is no silent empty result.
    Report LoadReport( const std::filesystem::path& inDirectory );

    // The human-readable crash time, derived from crash_epoch. Returns the raw field when it is not
    // a number, so a garbled value is visible rather than replaced by a plausible date.
    std::string FormatCrashTime( const Report& inReport );
} // namespace CrashReporter
