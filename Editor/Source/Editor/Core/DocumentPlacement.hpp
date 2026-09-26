#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

// Where an asset document (a Material Editor, a mesh or texture viewer, ...) is put the moment it OPENS.
//
// Unreal opens an asset editor as a large tab in the main work area, and that is the rule here: the document
// joins the dock node the level viewport lives in, as a tab beside it, and gets the whole of that area. The
// documents used to be docked into a column split off the centre for them (layout option B.1), which left a
// Material Editor about 400 px wide on a 1600 px window — its preview pane a sliver — and a document whose
// imgui.ini entry predated that column opened as a small floating window over the level.
//
// Only the FIRST frame of an opening is placed (the caller applies this with ImGuiCond_Appearing): whatever
// the person does with the tab afterwards — tears it out, docks it beside the Details — stands for as long
// as the document stays open.
//
// Kept apart from ImGui so the rule is asserted (Desert/Tests/Editor/DocumentOwnership): EditorLayer.cpp is
// compiled by no suite, and a placement that is wrong is only ever seen as "the window opened small".
namespace Desert::Editor::DocumentPlacement
{
    // A document that has no dock node to join floats over the editor at this share of its work area,
    // centred — large enough to work in, small enough that the editor behind it still reads as the editor.
    inline constexpr float kFloatingShare = 0.7f;

    struct Placement
    {
        // Non-zero: dock into this node, as a tab. Zero: float at Pos, Size (work-area pixels).
        uint32_t  DockId = 0;
        glm::vec2 Pos{ 0.0f };
        glm::vec2 Size{ 0.0f };

        [[nodiscard]] bool Docked() const noexcept
        {
            return DockId != 0;
        }
    };

    /**
     * @brief Where a document opens, given the dock node of the level viewport and the editor's work area.
     *
     * @p mainDockId is the node the level viewport is docked in (0 when it floats), and @p nodeLive whether
     * that node still exists in the dock tree — a window keeps the id of a node the layout has since
     * dropped, and docking into a dead id makes ImGui create a free-floating node of its own, which is the
     * small window this rule exists to prevent. Either failing, the document floats centred at
     * kFloatingShare of the work area.
     */
    [[nodiscard]] inline Placement Place( const uint32_t mainDockId, const bool nodeLive, const glm::vec2 workPos,
                                          const glm::vec2 workSize ) noexcept
    {
        if ( mainDockId != 0 && nodeLive )
            return { mainDockId, glm::vec2( 0.0f ), glm::vec2( 0.0f ) };

        // A work area nobody can measure (a minimised window, a NaN before the first frame) gives a zero
        // size rather than a negative or infinite one; ImGui then sizes the window to its content.
        const auto      usable = []( const float v ) { return std::isfinite( v ) && v > 0.0f ? v : 0.0f; };
        const glm::vec2 area( usable( workSize.x ), usable( workSize.y ) );
        const glm::vec2 size = area * kFloatingShare;
        const glm::vec2 origin( std::isfinite( workPos.x ) ? workPos.x : 0.0f,
                                std::isfinite( workPos.y ) ? workPos.y : 0.0f );
        return { 0u, origin + ( area - size ) * 0.5f, size };
    }
} // namespace Desert::Editor::DocumentPlacement
