#include "CommandPalette.hpp"

#include "FuzzyMatch.hpp"

#include <ImGui/imgui.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    Common::BoolResultStr CommandPalette::Draw()
    {
        if ( !m_Open )
            return PaletteCommandDone();

        constexpr const char* kPopupId = "##CommandPalette";
        if ( m_JustOpened )
            ImGui::OpenPopup( kPopupId );

        // Centered near the top of the main viewport, Sublime/VSCode style.
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos( ImVec2( center.x, center.y * 0.5f ), ImGuiCond_Always,
                                 ImVec2( 0.5f, 0.5f ) );
        ImGui::SetNextWindowSize( ImVec2( 560.0f, 0.0f ), ImGuiCond_Always );

        if ( !ImGui::BeginPopup( kPopupId ) )
        {
            m_Open = false; // popup dismissed (click outside)
            return PaletteCommandDone();
        }

        // WHAT THE CHOSEN ENTRY ANSWERED. Success until something runs and refuses; a frame in which
        // nobody chose anything is a success with nothing to say, which is what the caller wants.
        Common::BoolResultStr chosen = PaletteCommandDone();

        // THE RANKING AND THE SELECTION ARITHMETIC ARE NOT HERE ANY MORE (A6-2 point 3). They were
        // decisions buried in a drawing routine no suite compiles; they are free functions in the header
        // now, and this reads as what it is — a frame applying them.
        const std::vector<PaletteHit> hits = RankPaletteCommands( m_Commands, m_Query );

        m_Selected = ClampPaletteSelection( m_Selected, hits.size() );

        // Keyboard navigation (read before the InputText eats the frame's key state).
        if ( ImGui::IsKeyPressed( ImGuiKey_DownArrow, true ) )
            ++m_Selected;
        if ( ImGui::IsKeyPressed( ImGuiKey_UpArrow, true ) )
            --m_Selected;
        m_Selected = WrapPaletteSelection( m_Selected, hits.size() );

        if ( m_JustOpened )
        {
            ImGui::SetKeyboardFocusHere();
            m_JustOpened = false;
        }
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::InputTextWithHint( "##paletteQuery", "Go to panel, entity, action...", m_Query,
                                  sizeof( m_Query ) );

        const bool enter  = ImGui::IsKeyPressed( ImGuiKey_Enter, false ) ||
                           ImGui::IsKeyPressed( ImGuiKey_KeypadEnter, false );
        const bool escape = ImGui::IsKeyPressed( ImGuiKey_Escape, false );

        ImGui::Separator();
        ImGui::BeginChild( "##paletteResults", ImVec2( 0.0f, 320.0f ) );
        for ( int i = 0; i < static_cast<int>( hits.size() ); ++i )
        {
            const PaletteCommand& c        = *hits[i].Command;
            const bool            selected = ( i == m_Selected );
            if ( ImGui::Selectable( std::format( "{}##{}", c.Label, i ).c_str(), selected ) )
            {
                // The outcome is CAPTURED rather than returned from here: the popup still has to be
                // closed and EndChild/EndPopup still have to be called, and an early return would leave
                // ImGui's stack unbalanced. Returned once, at the bottom, after the frame is well-formed.
                chosen = c.Run();
                ImGui::CloseCurrentPopup();
                m_Open = false;
            }
            if ( selected )
                ImGui::SetScrollHereY( 0.5f );
            ImGui::SameLine();
            ImGui::TextDisabled( "  %s", c.Group.c_str() );
        }
        ImGui::EndChild();

        if ( enter && !hits.empty() )
        {
            chosen = hits[m_Selected].Command->Run();
            ImGui::CloseCurrentPopup();
            m_Open = false;
        }
        if ( escape )
        {
            ImGui::CloseCurrentPopup();
            m_Open = false;
        }

        ImGui::EndPopup();
        return chosen;
    }
} // namespace Desert::Editor
