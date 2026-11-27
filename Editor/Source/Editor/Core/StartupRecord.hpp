#pragma once

// THE EDITOR START'S TWO FACTS FROM OUTSIDE THE PROCESS (STARTUP-PROGRESS): when the process began, from the
// kernel's own record (the one clock that includes the loader and the static initialisers, which no timer
// started in main() sees), and where the previous start's per-phase seconds are kept. The host reads both
// before the splash (Sandbox.hpp, CreateApplication); EditorStartup writes the history at the reveal.

#include <Common/Core/ResultStr.hpp>

#include <chrono>
#include <filesystem>

namespace Desert::Editor
{
    // The process's start on the steady clock: now minus the kernel's age of this process. A refusal says why
    // the kernel did not answer.
    [[nodiscard]] Common::ResultStr<std::chrono::steady_clock::time_point> ProcessStartTime();

    // <ProjectContext::ConfigDirectory()>/startup_timings.json, beside machine.json. ONE name: the host reads
    // it, EditorStartup writes it.
    [[nodiscard]] std::filesystem::path StartupHistoryFile();
} // namespace Desert::Editor
