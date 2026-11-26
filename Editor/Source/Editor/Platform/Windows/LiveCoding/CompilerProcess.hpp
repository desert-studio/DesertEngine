#pragma once

#include <Common/Core/ResultStr.hpp>

#include <atomic>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Desert::Editor::LiveCoding
{
    struct CompileJob
    {
        std::string           Command;   // BuildIndex::CommandFor
        std::filesystem::path Directory; // compile_commands.json "directory"
        std::filesystem::path Object;    // the /Fo it writes
    };

    struct CompileResult
    {
        int         ExitCode = 0;
        std::string Output; // stdout + stderr, for the refusal
        double      Seconds = 0.0;
    };

    /// Runs the jobs as child processes (CreateProcessW, no window, BELOW_NORMAL_PRIORITY_CLASS), at most
    /// @p parallel at once, and waits for all of them (WaitForMultipleObjects). @p cancel terminates the
    /// children it started (by handle; never by image name).
    [[nodiscard]] Common::ResultStr<std::vector<CompileResult>>
    RunCompilers( std::span<const CompileJob> jobs, unsigned parallel, const std::atomic<bool>& cancel ) noexcept;
} // namespace Desert::Editor::LiveCoding
