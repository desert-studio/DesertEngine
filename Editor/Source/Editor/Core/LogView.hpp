#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    // Pure helpers behind the Logs panel's search + duplicate-collapse. Kept std-only so the filtering
    // and collapsing logic is unit-tested directly, away from ImGui and the log file.

    // --- Log line structure ---------------------------------------------------------------------
    // Every line the engine writes has the shape the logger's own pattern gives it (Common/Core/Logger.hpp:
    // `[%T.%e][%l][Desert]: %v`), so a row is not one blob of text — it is a timestamp, a severity, an
    // optional subsystem tag the message opens with, and the message itself:
    //
    //     [16:55:20.047][info][Desert]: [Vulkan] GPU: Apple M1 Pro (...)
    //      \__ Time __/  \Level/         \Cat_/  \____________ Message ____________/
    //
    // Splitting it is what lets the panel put the timestamps in a fixed column, chip the category, and
    // colour the row — and, more importantly, what lets the SEVERITY come from the line's own level field.
    // The panel used to decide severity by searching the WHOLE line for "[warning]" / "[error]", which
    // means any message quoting those words was drawn as if it were one. A log that miscolours its own
    // rows is worse than a plain one, because the colour is the thing people trust.

    enum class LogSeverity
    {
        Info = 0, // trace / debug / info — everything that is not a complaint
        Warning,  // warning
        Error     // error / critical
    };

    // Views INTO the line passed in; they are valid only as long as it is. Empty fields mean "the line did
    // not carry one" — a continuation line, or a message with no subsystem tag — never a guessed default.
    struct LogLineParts
    {
        std::string_view Time;     // "16:55:20.047", empty if the line has no standard prefix
        std::string_view Level;    // "info", "warning", ... as the logger spelled it
        std::string_view Category; // "Vulkan" — the [Tag] the message opens with, if any
        std::string_view Message;  // the message with that tag removed; the whole line when unparsed
        LogSeverity      Severity = LogSeverity::Info;
        bool             Parsed   = false; // did the line match the logger's pattern at all?
    };

    // Splits one written log line. Total: never throws, never allocates, and on anything that does not
    // match the pattern returns Parsed=false with Message set to the whole line, so an unrecognised line
    // is still shown in full rather than dropped.
    [[nodiscard]] LogLineParts ParseLogLine( std::string_view line );

    // The severity a level word denotes. Exposed because the panel's counters and its row colours must
    // agree with each other, and with what ParseLogLine reports.
    [[nodiscard]] LogSeverity SeverityOfLevel( std::string_view level );

    // Case-insensitive substring test. An empty query matches everything.
    bool LogMatches( const std::string& line, const std::string& query );

    // A run of consecutive identical log lines collapsed into one entry.
    struct LogRun
    {
        std::string Text;
        int         Level = 0;
        int         Count = 1;
    };

    // Collapse runs of CONSECUTIVE identical (text, level) lines into LogRuns with a count. Order is
    // preserved; non-adjacent duplicates are NOT merged (they are separate events in the timeline).
    std::vector<LogRun> CollapseConsecutive( const std::vector<std::pair<std::string, int>>& lines );

    // RDG-FAULT1. The collapse the panel needs. CollapseConsecutive compares the WHOLE line, and every line the
    // logger writes opens with its own timestamp, so a message repeated every frame never collapsed: the
    // 2026-10-05 flood was ~60 distinct rows a second with Collapse on. A repeat is the same Severity, Category
    // and Message of ParseLogLine - the time is not part of the event's identity. The run keeps the first line's
    // Text and the first and last time, so the panel shows "x<Count>  <FirstTime> .. <LastTime>". An unparsed line
    // has no time and compares whole. Order is preserved; non-adjacent repeats stay separate. Replaces
    // CollapseConsecutive (deleted with its call in LogsPanel::Refresh by the implementation step).
    struct LogRepeatRun
    {
        std::string Text; // the first line of the run, whole
        int         Level = 0;
        int         Count = 1;
        std::string FirstTime; // "16:55:20.047"; empty for an unparsed line
        std::string LastTime;
    };
    std::vector<LogRepeatRun> CollapseRepeats( const std::vector<std::pair<std::string, int>>& lines );
} // namespace Desert::Editor
