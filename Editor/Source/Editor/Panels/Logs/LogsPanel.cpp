#include "LogsPanel.hpp"
#include <Common/Core/Logger.hpp>

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/LogView.hpp>
#include <Editor/Core/ThemeManager.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;


    // The counters the STATUS BAR reads. File-static rather than per-panel because there is exactly one
    // log file and the status bar is not a panel; see LogsPanel::WarningCount in the header for why a
    // second parser would be a second answer.
    static std::size_t s_SessionWarnings = 0;
    static std::size_t s_SessionErrors   = 0;
    static std::size_t s_SessionInfo     = 0;
    // The tail the control channel serves, refreshed by the same pass that counts. Newest at the back,
    // capped at kTailCapacity so a long session does not turn this into a second copy of the log file.
    static std::vector<std::string> s_SessionTail;

    std::size_t LogsPanel::WarningCount()
    {
        return s_SessionWarnings;
    }

    std::size_t LogsPanel::ErrorCount()
    {
        return s_SessionErrors;
    }

    std::size_t LogsPanel::InfoCount()
    {
        return s_SessionInfo;
    }

    std::vector<std::string> LogsPanel::Tail( std::size_t maxLines )
    {
        const std::size_t take = std::min( maxLines, s_SessionTail.size() );
        return std::vector<std::string>( s_SessionTail.end() - static_cast<std::ptrdiff_t>( take ),
                                         s_SessionTail.end() );
    }

    LogsPanel::LogsPanel()
         : IPanel( "Logs" )
    {
        Refresh();
    }

    void LogsPanel::OnPreUpdate()
    {
        Refresh();
    }

    void LogsPanel::Refresh()
    {
        if ( !std::filesystem::exists( Common::Logger::CurrentLogFile() ) )
            return;

        auto writeTime = std::filesystem::last_write_time( Common::Logger::CurrentLogFile() );
        if ( writeTime == m_LastWriteTime )
            return;

        m_LastWriteTime = writeTime;
        m_Entries.clear();
        m_CountInfo = m_CountWarning = m_CountError = 0;

        std::ifstream file( Common::Logger::CurrentLogFile() );
        std::string   line;
        while ( std::getline( file, line ) )
        {
            if ( line.empty() )
                continue;

            // The severity comes from the line's OWN level field, via the shared parser. It used to come
            // from searching the whole line for "[warning]" / "[error]", which painted any message that
            // QUOTED those words — a shader diagnostic, a validation-layer string, this very comment
            // echoed back — in the colour of a fault it did not have. A log that miscolours its own rows
            // costs more than a plain one, because the colour is the part people trust at a glance.
            const LogLineParts parts = ParseLogLine( line );
            const int          level = static_cast<int>( parts.Severity );

            switch ( parts.Severity )
            {
                case LogSeverity::Warning:
                    ++m_CountWarning;
                    break;
                case LogSeverity::Error:
                    ++m_CountError;
                    break;
                default:
                    ++m_CountInfo;
                    break;
            }

            m_Entries.push_back( { std::move( line ), level } );
        }

        s_SessionWarnings = m_CountWarning;
        s_SessionErrors   = m_CountError;
        s_SessionInfo     = m_CountInfo;

        // Rebuilt from the entries just parsed rather than appended to as lines arrive: Refresh re-reads
        // the WHOLE file whenever it changes, so an appended tail would accumulate every line several
        // times over and the channel would report a log the editor never wrote.
        s_SessionTail.clear();
        const std::size_t tailStart =
             m_Entries.size() > kTailCapacity ? m_Entries.size() - kTailCapacity : std::size_t{ 0 };
        for ( std::size_t i = tailStart; i < m_Entries.size(); ++i )
            s_SessionTail.push_back( m_Entries[i].Text );

        m_ScrollToBottom = true;
    }

    void LogsPanel::DrawToolbar()
    {
        // SEVERITY CHIPS, each carrying its own count.
        //
        // The three buttons used to be plain "Info / Warn / Error" toggles, which means the question the
        // panel is opened for — "did anything go wrong?" — could only be answered by turning the other two
        // OFF and looking at what was left. The count belongs ON the control: a chip reading "0" answers it
        // without a click, and a chip reading "3" says how much reading is ahead.
        //
        // The count is of the WHOLE log, not of the filtered view (see m_CountInfo).
        const auto chip =
             [&]( const char* icon, std::size_t count, bool& flag, const ImVec4& colour, const char* tip )
        {
            char label[64];
            std::snprintf( label, sizeof( label ), "%s %zu", icon, count );

            // On: the severity's own colour at low alpha, so the chip reads as lit. Off: barely there —
            // a disabled filter must not look like a zero count.
            ImVec4 bg = colour;
            bg.w      = flag ? 0.20f : 0.06f;
            ImVec4 fg = colour;
            if ( !flag )
                fg.w = 0.45f;

            ImGui::PushStyleColor( ImGuiCol_Button, bg );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( bg.x, bg.y, bg.z, bg.w + 0.12f ) );
            ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( bg.x, bg.y, bg.z, bg.w + 0.20f ) );
            ImGui::PushStyleColor( ImGuiCol_Text, fg );
            ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 3.0f );
            if ( ImGui::Button( label ) )
                flag = !flag;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor( 4 );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s", tip );
            ImGui::SameLine( 0.0f, 4.0f );
        };

        chip( ICON_MDI_INFORMATION_OUTLINE, m_CountInfo, m_ShowInfo,
              ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ), "Show information lines" );
        chip( ICON_MDI_ALERT_OUTLINE, m_CountWarning, m_ShowWarnings, ThemeManager::GetWarningColor(),
              "Show warnings" );
        chip( ICON_MDI_CLOSE_CIRCLE_OUTLINE, m_CountError, m_ShowErrors, ThemeManager::GetErrorColor(),
              "Show errors" );

        // Search box.
        ImGui::SetNextItemWidth( 220.0f );
        ImGui::InputTextWithHint( "##logFilter", "Filter...", m_Filter, sizeof( m_Filter ) );
        if ( m_Filter[0] != '\0' )
        {
            ImGui::SameLine();
            if ( ImGui::Button( ICON_MDI_CLOSE "##clearFilter" ) )
                m_Filter[0] = '\0';
        }
        ImGui::SameLine();
        ImGui::Checkbox( "Collapse", &m_Collapse );

        ImGui::SameLine( ImGui::GetContentRegionMax().x - 200.0f );
        ImGui::Checkbox( "Auto-scroll", &m_ScrollToBottom );
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_DELETE_SWEEP_OUTLINE ) )
        {
            m_Entries.clear();
            m_CountInfo = m_CountWarning = m_CountError = 0;
            s_SessionWarnings = s_SessionErrors = 0;
            std::ofstream( Common::Logger::CurrentLogFile(), std::ios::trunc ).close();
            m_LastWriteTime = {};
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Clear the log" );
    }

    void LogsPanel::DrawRow( const LogRun& row )
    {
        const LogLineParts parts = ParseLogLine( row.Text );

        const ImVec4 severityColour = ( row.Level == 2 )   ? ThemeManager::GetErrorColor()
                                      : ( row.Level == 1 ) ? ThemeManager::GetWarningColor()
                                                           : ImGui::GetStyleColorVec4( ImGuiCol_Text );

        ImDrawList*  dl     = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float  rowH   = ImGui::GetTextLineHeight() + 4.0f;
        const float  left   = ImGui::GetWindowPos().x;
        const float  right  = left + ImGui::GetWindowSize().x;

        // A tint across the whole row, plus a solid edge at its left margin. The tint is what makes a
        // warning findable while SCROLLING (the colour of the text alone is not, at this size), and the
        // edge is what survives a row whose message is short enough to leave the band mostly empty.
        if ( row.Level != 0 )
        {
            ImVec4 wash = severityColour;
            wash.w      = 0.09f;
            dl->AddRectFilled( ImVec2( left, origin.y - 1.0f ), ImVec2( right, origin.y + rowH - 1.0f ),
                               ImGui::GetColorU32( wash ) );
        }
        {
            ImVec4 edge = severityColour;
            if ( row.Level == 0 )
            {
                edge   = ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled );
                edge.w = 0.25f;
            }
            dl->AddRectFilled( ImVec2( left, origin.y - 1.0f ), ImVec2( left + 2.0f, origin.y + rowH - 1.0f ),
                               ImGui::GetColorU32( edge ) );
        }

        // The timestamp in a FIXED column. Every stamp is the same twelve characters, so reserving the
        // width of those twelve characters makes every message start on one x — which is the whole reason
        // a log reads as a column of events rather than as ragged prose. (The engine ships no monospaced
        // face; a fixed column holding a fixed-length field gets the same result without one.)
        const float startX     = ImGui::GetCursorPosX();
        const float stampWidth = ImGui::CalcTextSize( "00:00:00.000" ).x + 10.0f;
        ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
        // An unparsed line (a continuation, a crash dump pasted in) has no stamp; it still gets the column,
        // so its text lines up with everything else instead of jutting out into the gutter.
        ImGui::TextUnformatted( parts.Parsed ? parts.Time.data() : " ",
                                parts.Parsed ? parts.Time.data() + parts.Time.size() : nullptr );
        ImGui::PopStyleColor();
        ImGui::SameLine( 0.0f, 0.0f );
        ImGui::SetCursorPosX( startX + stampWidth );

        // The category as a chip: scanning for everything the Shader subsystem said needs no filter typed,
        // and a bracketed tag inside a wall of text does not read as a heading.
        if ( !parts.Category.empty() )
        {
            const ImVec2 chipPos = ImGui::GetCursorScreenPos();
            const ImVec2 textSz =
                 ImGui::CalcTextSize( parts.Category.data(), parts.Category.data() + parts.Category.size() );
            ImVec4 chipBg = ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled );
            chipBg.w      = 0.22f;
            dl->AddRectFilled( ImVec2( chipPos.x, chipPos.y + 1.0f ),
                               ImVec2( chipPos.x + textSz.x + 10.0f, chipPos.y + rowH - 3.0f ),
                               ImGui::GetColorU32( chipBg ), 3.0f );
            ImGui::SetCursorScreenPos( ImVec2( chipPos.x + 5.0f, chipPos.y ) );
            ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );
            ImGui::TextUnformatted( parts.Category.data(), parts.Category.data() + parts.Category.size() );
            ImGui::PopStyleColor();
            ImGui::SameLine( 0.0f, 10.0f );
        }

        ImGui::PushStyleColor( ImGuiCol_Text, severityColour );
        if ( row.Count > 1 )
        {
            // The collapse count stays attached to the message, not to the row: it counts repeats of the
            // TEXT, and putting it at the right edge would let it drift away from what it counts.
            const std::string message( parts.Message );
            ImGui::Text( "%s  (x%d)", message.c_str(), row.Count );
        }
        else
        {
            ImGui::TextUnformatted( parts.Message.data(), parts.Message.data() + parts.Message.size() );
        }
        ImGui::PopStyleColor();
    }

    void LogsPanel::OnUIRender()
    {
        Refresh();

        DrawToolbar();
        ImGui::Separator();

        ImGui::BeginChild( "##logscroll", ImVec2( 0.0f, 0.0f ), false,
                           ImGuiWindowFlags_HorizontalScrollbar );

        // Build the filtered subset (level toggles + case-insensitive search), optionally collapse runs of
        // consecutive duplicates, then render only the on-screen rows via a clipper. The clipper keeps this
        // O(visible) even for a huge log; all rows are single-line, so it is exact.
        // Rebuild the row list ONLY when something it depends on changed — see the note on m_Rows. Doing
        // this per frame cost more than rendering the scene.
        const bool viewDirty = m_RowsBuiltFromCount != m_Entries.size() || m_RowsInfo != m_ShowInfo ||
                               m_RowsWarnings != m_ShowWarnings || m_RowsErrors != m_ShowErrors ||
                               m_RowsCollapse != m_Collapse ||
                               std::strncmp( m_RowsFilter, m_Filter, sizeof( m_RowsFilter ) ) != 0;
        if ( viewDirty )
        {
            std::vector<std::pair<std::string, int>> filtered;
            filtered.reserve( m_Entries.size() );
            for ( const auto& entry : m_Entries )
            {
                if ( entry.Level == 0 && !m_ShowInfo )     continue;
                if ( entry.Level == 1 && !m_ShowWarnings ) continue;
                if ( entry.Level == 2 && !m_ShowErrors )   continue;
                if ( !LogMatches( entry.Text, m_Filter ) ) continue;
                filtered.emplace_back( entry.Text, entry.Level );
            }

            if ( m_Collapse )
            {
                m_Rows = CollapseConsecutive( filtered );
            }
            else
            {
                m_Rows.clear();
                m_Rows.reserve( filtered.size() );
                for ( auto& [text, level] : filtered )
                    m_Rows.push_back( { std::move( text ), level, 1 } );
            }

            m_RowsBuiltFromCount = m_Entries.size();
            m_RowsInfo           = m_ShowInfo;
            m_RowsWarnings       = m_ShowWarnings;
            m_RowsErrors         = m_ShowErrors;
            m_RowsCollapse       = m_Collapse;
            std::memcpy( m_RowsFilter, m_Filter, sizeof( m_RowsFilter ) );
        }

        const std::vector<LogRun>& rows = m_Rows;

        ImGuiListClipper clipper;
        clipper.Begin( static_cast<int>( rows.size() ) );
        while ( clipper.Step() )
        {
            for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row )
                DrawRow( rows[row] );
        }
        clipper.End();

        // Right-click the log area to copy the currently visible lines.
        if ( ImGui::BeginPopupContextWindow() )
        {
            if ( ImGui::MenuItem( "Copy visible" ) )
            {
                std::string all;
                for ( const auto& r : rows )
                {
                    all += r.Text;
                    if ( r.Count > 1 )
                        all += "  (x" + std::to_string( r.Count ) + ")";
                    all += '\n';
                }
                ImGui::SetClipboardText( all.c_str() );
            }
            ImGui::EndPopup();
        }

        if ( m_ScrollToBottom )
        {
            ImGui::SetScrollHereY( 1.0f );
            m_ScrollToBottom = false;
        }

        ImGui::EndChild();
    }

} // namespace Desert::Editor
