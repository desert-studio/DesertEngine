#pragma once

#include <Editor/Core/CommandPalette.hpp>
#include <Editor/Core/Control/ControlPipeline.hpp>
#include <Editor/LiveCoding/ILiveCodingBackend.hpp>
#include <Editor/LiveCoding/LiveCodingBuildIndex.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Editor::LiveCoding
{
    /// THE OWNER of Live Coding in the editor process (a member of EditorLayer). One patch at a time.
    class LiveCodingSession
    {
    public:
        LiveCodingSession();
        ~LiveCodingSession() noexcept; // cancels a running patch and waits for its job
        LiveCodingSession( const LiveCodingSession& )            = delete;
        LiveCodingSession& operator=( const LiveCodingSession& ) = delete;

        /// Palette "Live Coding: Patch changed sources", Ctrl+Alt+F11 and the control channel. Answers at once:
        /// "started", or the refusal (a patch runs; no backend on this platform; LiveCoding.json missing).
        [[nodiscard]] Common::BoolResultStr RequestPatch();

        /// MAIN THREAD, first thing in EditorLayer::OnUpdate: applies a prepared patch, logs the report.
        void ApplyPending();

        /// MAIN THREAD, each frame. For Control::BackgroundWork::LiveCoding: Running while
        /// scanning/compiling/linking/ready; Progress is kPatchStageNames; Outcome is the last report's Summary() or
        /// the last refusal, kept until the next request. The outcome is copied only when it changed.
        [[nodiscard]] const Control::BackgroundReport& Report();

        /// The palette's "Live Coding" group.
        void AppendCommands( std::vector<PaletteCommand>& out );

    private:
        Common::ResultStr<std::unique_ptr<ILiveCodingBackend>> m_Backend; // CreateLiveCodingBackend()
        std::optional<BuildIndex>                              m_Index;   // loaded on the first request
        std::atomic<PatchStage>                                m_Stage{ PatchStage::Idle };
        std::atomic<bool>                                      m_Cancel{ false };
        std::atomic<bool>                                      m_JobDone{ true };
        std::mutex                                             m_Mutex;   // guards the four below
        std::unique_ptr<IPreparedPatch>                        m_Prepared;
        std::vector<std::size_t>                               m_PreparedSources;
        std::vector<std::filesystem::file_time_type>           m_PreparedAt; // their mtimes when compiled
        std::string                                            m_Outcome;
        std::atomic<std::uint64_t> m_OutcomeVersion{ 0 }; // raised under m_Mutex with every m_Outcome write
        std::uint64_t              m_ReportedVersion = 0; // main thread: the version m_Report holds
        Control::BackgroundReport  m_Report;              // main thread: what Report() returns
    };
} // namespace Desert::Editor::LiveCoding
