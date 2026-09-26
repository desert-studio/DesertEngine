#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

// Where an asset document (a Material Editor, a mesh or texture viewer, ...) is put the moment it OPENS.
//
// Unreal opens an asset editor as a large tab in the main work area, and that is the rule here: the document
// joins the dock node the level viewport lives in, as a tab beside it, and gets the whole of that area. The
// documents used to be docked into a column split off the centre for them (layout option B.1), which left a
// Material Editor about 400 px wide on a 1600 px window — its preview pane a sliver — and a document whose
// imgui.ini entry predated that column opened as a small floating window over the level.
//
// Only the FIRST frame of an opening is placed (EditorLayer::DrawDocuments, ImGuiCond_Always once): whatever
// the person does with the tab afterwards stands, and is remembered per document kind (below).
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

    // ── REMEMBERED PLACEMENT, PER DOCUMENT KIND (the Unreal rule) ─────────────────────────────────────────
    //
    // The FIRST opening of a kind (a Material Editor, a mesh viewer, ...) goes beside the level viewport.
    // Wherever the person then puts it — another dock node, or floating — is remembered for that kind, and
    // the next opening goes back there. Persisted in imgui.ini beside the dock layout, so it survives a
    // restart. A remembered node that no longer exists (the layout was reset or rebuilt) falls back to the
    // level viewport's node and SAYS SO: the Report names the kind and the dead node.
    struct Remembered
    {
        enum class Where : uint8_t
        {
            NextToScene, // in the level viewport's node, whatever that node's id is today
            DockNode,    // in a named dock node
            Floating,    // an undocked window at Pos, Size
        };
        Where     At     = Where::NextToScene;
        uint32_t  DockId = 0;
        glm::vec2 Pos{ 0.0f };
        glm::vec2 Size{ 0.0f };

        bool operator==( const Remembered& ) const = default;
    };

    // What the open window's placement says to remember. The scene's node is remembered as NextToScene rather
    // than by id, so a layout rebuilt under a new id still reads as "beside the level", with no fallback.
    [[nodiscard]] inline Remembered Observe( const uint32_t windowDockId, const uint32_t sceneDockId,
                                             const glm::vec2 pos, const glm::vec2 size ) noexcept
    {
        if ( windowDockId != 0 && windowDockId == sceneDockId )
            return {};
        if ( windowDockId != 0 )
            return { Remembered::Where::DockNode, windowDockId, glm::vec2( 0.0f ), glm::vec2( 0.0f ) };
        return { Remembered::Where::Floating, 0u, pos, size };
    }

    struct Resolution
    {
        Placement   Place;
        std::string Report; // non-empty: the remembered placement was refused, and why
    };

    /**
     * @brief Where a document of @p kind opens, given what was remembered for it (nullptr: never opened).
     * @p rememberedLive whether the remembered DockNode still exists in the dock tree.
     */
    [[nodiscard]] inline Resolution Resolve( const std::string& kind, const Remembered* remembered,
                                             const uint32_t sceneDockId, const bool sceneLive,
                                             const bool rememberedLive, const glm::vec2 workPos,
                                             const glm::vec2 workSize )
    {
        const Placement beside = Place( sceneDockId, sceneLive, workPos, workSize );
        if ( remembered == nullptr || remembered->At == Remembered::Where::NextToScene )
            return { beside, {} };
        if ( remembered->At == Remembered::Where::DockNode )
        {
            if ( remembered->DockId != 0 && rememberedLive )
                return { { remembered->DockId, glm::vec2( 0.0f ), glm::vec2( 0.0f ) }, {} };
            return { beside,
                     "the " + kind + " was last docked in node " + std::to_string( remembered->DockId ) +
                          ", which the current layout no longer has; opening it beside the level viewport" };
        }
        const bool usable = std::isfinite( remembered->Size.x ) && std::isfinite( remembered->Size.y ) &&
                            remembered->Size.x > 0.0f && remembered->Size.y > 0.0f &&
                            std::isfinite( remembered->Pos.x ) && std::isfinite( remembered->Pos.y );
        if ( usable )
            return { { 0u, remembered->Pos, remembered->Size }, {} };
        return { beside, "the " + kind +
                              " was last floating with an unusable size; opening it beside the level "
                              "viewport" };
    }

    // One imgui.ini value: "next" | "dock <id>" | "float <x> <y> <w> <h>".
    [[nodiscard]] inline std::string Format( const Remembered& r )
    {
        char buf[96];
        switch ( r.At )
        {
            case Remembered::Where::NextToScene:
                return "next";
            case Remembered::Where::DockNode:
                std::snprintf( buf, sizeof( buf ), "dock %u", r.DockId );
                return buf;
            case Remembered::Where::Floating:
                std::snprintf( buf, sizeof( buf ), "float %.0f %.0f %.0f %.0f", r.Pos.x, r.Pos.y, r.Size.x,
                               r.Size.y );
                return buf;
        }
        return "next";
    }

    [[nodiscard]] inline std::optional<Remembered> Parse( const std::string& text )
    {
        unsigned id = 0;
        float    x  = 0;
        float    y  = 0;
        float    w  = 0;
        float    h  = 0;
        if ( text == "next" )
            return Remembered{};
        if ( std::sscanf( text.c_str(), "dock %u", &id ) == 1 && id != 0 )
            return Remembered{ Remembered::Where::DockNode, id, glm::vec2( 0.0f ), glm::vec2( 0.0f ) };
        if ( std::sscanf( text.c_str(), "float %f %f %f %f", &x, &y, &w, &h ) == 4 )
            return Remembered{ Remembered::Where::Floating, 0u, glm::vec2( x, y ), glm::vec2( w, h ) };
        return std::nullopt;
    }
} // namespace Desert::Editor::DocumentPlacement
