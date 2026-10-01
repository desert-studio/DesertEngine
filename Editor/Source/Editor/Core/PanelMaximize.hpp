#pragma once

#include <Editor/Core/CommandPalette.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    // "Maximize panel" / "Restore panel" (CTL2): one docked panel at a time is lifted out of its dock and
    // stretched over the main viewport's work area; restore puts the WHOLE dock layout back as it stood.
    //
    // UE's maximize is a snapshot of the tab layout and a return to it, not a re-dock of one tab: a panel
    // that was alone in its node empties that node when it is lifted out, ImGui deletes the empty node, and
    // a re-dock into the remembered node id names a dead node — ImGui then makes a new FLOATING node, and the
    // layout file keeps the panel floating across restarts (the Scene viewport was lost this way). So the
    // layout is captured (as ImGui ini text) at the moment of undocking and handed back whole on restore.
    //
    // No ImGui here: the layer asks Before() right ahead of the panel's Begin and turns the answer into
    // SetNextWindow* calls, and asks TakeLayoutToRestore() at the top of the frame, before the dockspace is
    // submitted; the state machine is testable without a context. Dock node ids are ImGuiID, carried as
    // std::uint32_t.
    class PanelMaximize
    {
    public:
        enum class Step
        {
            None,   // nothing to do for this panel this frame
            Undock, // take it out of its dock, fill the work area, bring it forward
            Restore // pending only: the layout snapshot goes back at the top of the next frame
        };

        [[nodiscard]] Common::BoolResultStr RequestMaximize( std::string_view panel )
        {
            if ( !m_Panel.empty() && m_Panel != panel )
                return Common::MakeError( "Maximize panel: '" + m_Panel + "' is maximized; restore it before '" +
                                          std::string( panel ) + "'" );
            if ( m_Maximized )
                return Common::MakeError( "Maximize panel: '" + m_Panel + "' is already maximized" );
            m_Panel   = panel;
            m_Pending = Step::Undock;
            return Common::MakeSuccess( true );
        }

        [[nodiscard]] Common::BoolResultStr RequestRestore()
        {
            if ( !m_Maximized )
                return Common::MakeError( "Restore panel: no panel is maximized" );
            m_Pending = Step::Restore;
            return Common::MakeSuccess( true );
        }

        // Right before the panel's Begin. `currentDockId` is the window's dock node now (0 = floating);
        // `layoutNow` captures the dock layout as it stands, and is called only on the frame the panel is
        // lifted out — before it is. A maximized panel the user docked back by hand counts as restored: putting
        // the old layout back would undo what the user just did.
        [[nodiscard]] Step Before( std::string_view panel, std::uint32_t currentDockId,
                                   const std::function<std::string()>& layoutNow )
        {
            if ( panel != m_Panel )
                return Step::None;

            if ( m_Pending == Step::Undock )
            {
                m_Pending = Step::None;
                if ( currentDockId == 0 )
                {
                    // A floating panel has no dock to return to; filling the screen would strand it there.
                    Clear();
                    return Step::None;
                }
                m_Layout    = layoutNow();
                m_Maximized = true;
                return Step::Undock;
            }
            if ( m_Maximized && m_Pending == Step::None && currentDockId != 0 )
                Clear();
            return Step::None;
        }

        // At the top of the frame, before the dockspace is submitted: the layout captured when the panel was
        // lifted out, once, after "Restore panel" — the layer loads it back whole. Empty otherwise.
        [[nodiscard]] std::optional<std::string> TakeLayoutToRestore()
        {
            if ( m_Pending != Step::Restore )
                return std::nullopt;
            std::optional<std::string> layout = std::move( m_Layout );
            Clear();
            return layout;
        }

        [[nodiscard]] const std::string& MaximizedPanel() const
        {
            return m_Maximized ? m_Panel : kNone;
        }

    private:
        void Clear()
        {
            m_Panel.clear();
            m_Layout.clear();
            m_Maximized = false;
            m_Pending   = Step::None;
        }

        static inline const std::string kNone;

        std::string m_Panel;
        std::string m_Layout; // ImGui ini text: every window's dock binding and every dock node
        bool        m_Maximized = false;
        Step        m_Pending   = Step::None;
    };

    // The palette entries: "Maximize panel: <name>" for every panel shown in a dock while none is maximized,
    // and "Restore panel" while one is.
    [[nodiscard]] inline std::vector<PaletteCommand>
    PanelMaximizePaletteCommands( PanelMaximize& state, const std::vector<std::string>& docked )
    {
        std::vector<PaletteCommand> commands;
        if ( !state.MaximizedPanel().empty() )
        {
            commands.push_back( { "Panel", "Restore panel", [&state] { return state.RequestRestore(); } } );
            return commands;
        }
        commands.reserve( docked.size() );
        for ( const std::string& panel : docked )
            commands.push_back( { "Panel", "Maximize panel: " + panel,
                                  // clang-tidy 18 reports the closure's implicit move constructor, which only
                                  // moves a std::string (noexcept); nothing on this path can throw.
                                  // NOLINTNEXTLINE(bugprone-exception-escape)
                                  [&state, panel] { return state.RequestMaximize( panel ); } } );
        return commands;
    }
} // namespace Desert::Editor
