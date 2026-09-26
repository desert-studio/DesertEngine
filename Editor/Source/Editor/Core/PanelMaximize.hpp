#pragma once

#include <Editor/Core/CommandPalette.hpp>
#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    // "Maximize panel" / "Restore panel" (CTL2): one docked panel at a time is lifted out of its dock and
    // stretched over the main viewport's work area, and put back into the SAME dock node on restore.
    //
    // No ImGui here: the layer asks Before() right ahead of the panel's Begin and turns the answer into
    // SetNextWindow* calls, so the state machine is testable without a context. Dock node ids are ImGuiID,
    // a 32-bit unsigned, carried as std::uint32_t.
    class PanelMaximize
    {
    public:
        enum class Step
        {
            None,   // nothing to do for this panel this frame
            Undock, // take it out of its dock, fill the work area, bring it forward
            Redock  // back into Directive::DockId
        };

        struct Directive
        {
            Step          Kind   = Step::None;
            std::uint32_t DockId = 0;
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
            m_Pending = Step::Redock;
            return Common::MakeSuccess( true );
        }

        // Right before the panel's Begin. `currentDockId` is the window's dock node now (0 = floating). A
        // maximized panel the user docked back by hand counts as restored: the saved node would only
        // move it a second time.
        [[nodiscard]] Directive Before( std::string_view panel, std::uint32_t currentDockId )
        {
            if ( panel != m_Panel )
                return {};

            const Step step = std::exchange( m_Pending, Step::None );
            switch ( step )
            {
                case Step::Undock:
                    if ( currentDockId == 0 )
                    {
                        // A floating panel has no dock to return to; filling the screen would strand it there.
                        m_Panel.clear();
                        return {};
                    }
                    m_SavedDockId = currentDockId;
                    m_Maximized   = true;
                    return { Step::Undock, 0 };
                case Step::Redock:
                {
                    const Directive redock{ Step::Redock, m_SavedDockId };
                    Clear();
                    return redock;
                }
                case Step::None:
                    break;
            }
            if ( m_Maximized && currentDockId != 0 )
                Clear();
            return {};
        }

        [[nodiscard]] const std::string& MaximizedPanel() const
        {
            return m_Maximized ? m_Panel : kNone;
        }

    private:
        void Clear()
        {
            m_Panel.clear();
            m_Maximized   = false;
            m_SavedDockId = 0;
            m_Pending     = Step::None;
        }

        static inline const std::string kNone;

        std::string   m_Panel;
        bool          m_Maximized   = false;
        std::uint32_t m_SavedDockId = 0;
        Step          m_Pending     = Step::None;
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
                                  [&state, panel] { return state.RequestMaximize( panel ); } } );
        return commands;
    }
} // namespace Desert::Editor
