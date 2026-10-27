#include "LogsPanel.hpp"
#include <Common/Core/Profiler.hpp>

#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Core/LogView.hpp>
#include <Engine/Core/TextTemplate.hpp>
#include <Editor/Core/ThemeManager.hpp>

#include <Common/Core/Logger.hpp>

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

    void LogsPanel::ResetParsed()
    {
        m_Entries.clear();
        m_PendingLine.clear();
        m_ReadOffset = 0;
        m_CountInfo = m_CountWarning = m_CountError = 0;
        s_SessionTail.clear();
    }

    void LogsPanel::Refresh()
    {
        DESERT_PROFILE_SCOPE( "Logs: Refresh" );
        // The file the logger placed (Common::Logger::LogFilePath) — never a bare name, which would be read
        // from whatever directory the editor was started in.
        const std::filesystem::path logFile = Common::Logger::LogFilePath();
        if ( logFile.empty() )
            return;

        // TAILED, not re-read: only the bytes written since the last call are parsed. Re-reading the whole
        // file on every change made this panel O(log size) per frame - the logger writes every frame, so a
        // 28k-line Bistro session spent 110 ms of a 233 ms frame here. The file stays OPEN between frames:
        // opening it (plus exists/file_size) twice a frame cost 8.9 ms of a 28 ms Bistro frame on Windows,
        // where every open is scanned. A file shorter than what was already read was truncated (Clear, or a
        // new session reusing the name): start over from its first byte.
        if ( !m_File.is_open() || m_FilePath != logFile )
        {
            m_File.close();
            m_File.open( logFile, std::ios::binary );
            m_FilePath = logFile;
            ResetParsed();
            if ( !m_File.is_open() )
                return;
        }
        m_File.clear();
        m_File.seekg( 0, std::ios::end );
        const std::streamoff end = m_File.tellg();
        if ( end < 0 || static_cast<std::uintmax_t>( end ) == m_ReadOffset )
            return;
        const auto size = static_cast<std::uintmax_t>( end );
        if ( size < m_ReadOffset )
            ResetParsed();

        m_File.seekg( static_cast<std::streamoff>( m_ReadOffset ) );
        std::string chunk( static_cast<std::size_t>( size - m_ReadOffset ), '\0' );
        m_File.read( chunk.data(), static_cast<std::streamsize>( chunk.size() ) );
        chunk.resize( static_cast<std::size_t>( m_File.gcount() ) );
        m_ReadOffset += chunk.size();

        // A line the logger is still writing has no '\n' yet; it waits in m_PendingLine for the rest.
        const std::size_t firstNew = m_Entries.size();
        std::size_t       begin    = 0;
        for ( std::size_t end = chunk.find( '\n' ); end != std::string::npos; end = chunk.find( '\n', begin ) )
        {
            std::string line = std::move( m_PendingLine );
            m_PendingLine.clear();
            line.append( chunk, begin, end - begin );
            begin = end + 1;
            if ( !line.empty() && line.back() == '\r' )
                line.pop_back();
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
        m_PendingLine.append( chunk, begin, std::string::npos );
        if ( m_Entries.size() == firstNew )
            return;

        s_SessionWarnings = m_CountWarning;
        s_SessionErrors   = m_CountError;
        s_SessionInfo     = m_CountInfo;

        // Extended by the lines just parsed - each line is read once now, so appending cannot repeat one -
        // and trimmed back to the newest kTailCapacity.
        for ( std::size_t i = firstNew; i < m_Entries.size(); ++i )
            s_SessionTail.push_back( m_Entries[i].Text );
        if ( s_SessionTail.size() > kTailCapacity )
            s_SessionTail.erase( s_SessionTail.begin(),
                                 s_SessionTail.end() - static_cast<std::ptrdiff_t>( kTailCapacity ) );

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
            m_PendingLine.clear();
            m_CountInfo = m_CountWarning = m_CountError = 0;
            s_SessionWarnings = s_SessionErrors = 0;
            s_SessionTail.clear();
            if ( const std::filesystem::path logFile = Common::Logger::LogFilePath(); !logFile.empty() )
                std::ofstream( logFile, std::ios::trunc ).close();
            m_ReadOffset = 0;
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Clear the log" );
    }

    void LogsPanel::DrawRow( const LogRepeatRun& row )
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
            // EVENT (CollapseRepeats: same level, category and message, any time), and putting it at the right
            // edge would let it drift away from what it counts. The time span says how long it repeated.
            const std::string message( parts.Message );
            if ( row.FirstTime.empty() )
                ImGui::Text( "%s  (x%d)", message.c_str(), row.Count );
            else
                ImGui::Text( "%s  (x%d  %s .. %s)", message.c_str(), row.Count, row.FirstTime.c_str(),
                             row.LastTime.c_str() );
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
        // A filter change rebuilds the rows; new log lines only extend them (the collapse folds a new line into
        // the last run exactly as a rebuild would), so the cost per frame is the lines written that frame.
        const bool filtersChanged = m_RowsInfo != m_ShowInfo || m_RowsWarnings != m_ShowWarnings ||
                                    m_RowsErrors != m_ShowErrors || m_RowsCollapse != m_Collapse ||
                                    std::strncmp( m_RowsFilter, m_Filter, sizeof( m_RowsFilter ) ) != 0;
        if ( filtersChanged || m_RowsBuiltFromCount > m_Entries.size() )
        {
            m_Rows.clear();
            m_RowsBuiltFromCount = 0;
        }
        for ( std::size_t i = m_RowsBuiltFromCount; i < m_Entries.size(); ++i )
        {
            const LogEntry& entry = m_Entries[i];
            if ( entry.Level == 0 && !m_ShowInfo )     continue;
            if ( entry.Level == 1 && !m_ShowWarnings ) continue;
            if ( entry.Level == 2 && !m_ShowErrors )   continue;
            if ( !LogMatches( entry.Text, m_Filter ) ) continue;
            if ( m_Collapse )
            {
                AppendCollapsed( m_Rows, entry.Text, entry.Level );
            }
            else
            {
                const std::string time( ParseLogLine( entry.Text ).Time );
                m_Rows.push_back( { entry.Text, entry.Level, 1, time, time } );
            }
        }
        m_RowsBuiltFromCount = m_Entries.size();
        m_RowsInfo           = m_ShowInfo;
        m_RowsWarnings       = m_ShowWarnings;
        m_RowsErrors         = m_ShowErrors;
        m_RowsCollapse       = m_Collapse;
        std::memcpy( m_RowsFilter, m_Filter, sizeof( m_RowsFilter ) );

        const std::vector<LogRepeatRun>& rows = m_Rows;

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
                static const ::Desert::Core::TextTemplate kVisible(
                     "LogsCopyVisible",
                     "{% for row in rows %}{{ row.text }}{% if row.count > 1 %}  (x{{ row.count }}){% endif %}\n"
                     "{% endfor %}" );
                Common::Json::Value::Array lines;
                lines.reserve( rows.size() );
                for ( const auto& r : rows )
                    lines.emplace_back(
                         Common::Json::ObjectBuilder().Set( "text", r.Text ).Set( "count", r.Count ).Build() );
                const std::string all = kVisible.RenderOrLog(
                     Common::Json::Value( Common::Json::ObjectBuilder()
                                               .Set( "rows", Common::Json::Value( std::move( lines ) ) )
                                               .Build() ) );
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
