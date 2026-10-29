#include "IPanel.hpp"

#include <ImGui/imgui.h>

namespace Desert::Editor
{
    bool IPanel::HoldsKeyboardFocus() const
    {
        return m_InteractionFrame == ::ImGui::GetFrameCount() && m_KeyboardFocus;
    }

    bool IPanel::IsUnderPointer() const
    {
        return m_InteractionFrame == ::ImGui::GetFrameCount() && m_UnderPointer;
    }

    void IPanel::TrackWindowInteraction()
    {
        m_InteractionFrame = ::ImGui::GetFrameCount();
        m_KeyboardFocus    = ::ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows );
        m_UnderPointer     = ::ImGui::IsWindowHovered( ImGuiHoveredFlags_RootAndChildWindows );
    }
} // namespace Desert::Editor
