#pragma once

#include "../IPanel.hpp"

#include <Editor/Core/LogView.hpp> // LogRepeatRun — the cached row type below

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace Desert::Editor
{
    class LogsPanel final : public IPanel
    {
    public:
        LogsPanel();
        void OnUIRender() override;
        // Tailing the log file happens here, NOT in OnUIRender: OnPreUpdate runs for every panel including
        // hidden ones, and the counters below are read by the status bar, which is on screen whether or not
        // this panel is. Refreshing only while visible would let the status bar report yesterday's warning
        // count with total confidence.
        void OnPreUpdate() override;

        // How many warnings / errors the session has logged so far. Static because the STATUS BAR needs
        // them and is not a panel: this is the one place that has parsed the log, and a second parser in
        // the status bar would be a second answer to "are there errors" — the exact question the chips
        // exist to settle.
        [[nodiscard]] static std::size_t WarningCount();
        [[nodiscard]] static std::size_t ErrorCount();
        [[nodiscard]] static std::size_t InfoCount();

        // The last @p maxLines written, oldest first — what the control channel hands a client that asks
        // for the log. Static for the same reason the counters are: this class is the one thing that has
        // parsed the log file, and a second reader would be a second answer to "what happened", differing
        // by whatever the two read at different moments.
        //
        // Capped by kTailCapacity however much is asked for: a client is reading this to find out what
        // went wrong, and a whole session's log down a socket is not that.
        static constexpr std::size_t                  kTailCapacity = 200;
        [[nodiscard]] static std::vector<std::string> Tail( std::size_t maxLines );

    private:
        struct LogEntry
        {
            std::string Text;
            int         Level; // 0=trace/debug/info, 1=warning, 2=error/critical
        };

        void Refresh();
        void DrawToolbar();
        // One row: severity edge + tint, fixed-width timestamp column, category chip, message.
        static void DrawRow( const LogRepeatRun& row );

    private:
        std::vector<LogEntry>                    m_Entries;
        std::filesystem::file_time_type          m_LastWriteTime{};
        bool                                     m_ScrollToBottom = true;
        bool                                     m_ShowInfo       = true;
        bool                                     m_ShowWarnings   = true;
        bool                                     m_ShowErrors     = true;
        bool                                     m_Collapse       = true; // fold consecutive duplicates
        char                                     m_Filter[128]    = {};   // case-insensitive search

        // Per-severity totals of the WHOLE log, not of the filtered view: the chips carry them, and a
        // count that fell when you unticked its own chip would answer a different question than the one
        // "are there errors" is asking.
        std::size_t m_CountInfo = 0, m_CountWarning = 0, m_CountError = 0;

        // CACHED VIEW. The row list only changes when the log or the filters change, but it used to be
        // rebuilt from scratch every frame — two std::string copies per entry, so ~10k allocations per
        // frame on a 5k-line log. The ImGuiListClipper below made the DRAWING O(visible), but all of that
        // work happened before the clipper ever ran, and it measured 9.7 ms of a 16 ms frame — more than
        // the entire 3D scene. Rebuilt only when one of the inputs below actually differs.
        std::vector<LogRepeatRun> m_Rows;
        size_t              m_RowsBuiltFromCount = static_cast<size_t>( -1 );
        bool                m_RowsInfo           = true;
        bool                m_RowsWarnings       = true;
        bool                m_RowsErrors         = true;
        bool                m_RowsCollapse       = true;
        char                m_RowsFilter[128]    = {};
    };
} // namespace Desert::Editor
