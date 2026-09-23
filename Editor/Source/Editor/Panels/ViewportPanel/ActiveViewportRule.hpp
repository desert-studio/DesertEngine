#pragma once

// ── "WHICH VIEWPORT DID YOU MEAN", AND WHY IT MUST NOT BE REMEMBERED ───────────────────────────────
//
// A command that aims a camera — the toolbar's angle menu, the palette's "Viewport Camera: Top", the
// control channel's — has to pick one of the live viewports. Several are live at once: that is a
// shipped feature here (Scenes -> New Scene View, and the four-up grid opens three more).
//
// THE ANSWER IS FOCUS, AND THE EDITOR ALREADY HAS IT TWICE. Multi-scene activation follows the focused
// viewport (`ViewportPanel::SetOnActivate` -> `EditorLayer::SetActiveScene`, which rebinds m_MainScene
// and every panel's scene), and bone authoring follows it too (`TakeAuthoringContextIfFocused`). What
// `ActiveViewport()` did was ask NEITHER: it looked for the viewport holding the AUTHORING context and
// otherwise took the first live one. Those are different questions and they give different answers —
// the authoring context can be held by a Sequencer document or by the Details bone tree, neither of
// which is a viewport, and in that state the answer collapsed to "the first one in the list" with no
// reference to where the user was at all.
//
// SO THIS IS NOT A THIRD COPY. It is a READING of the one authority that already knows: ImGui's own
// focus order (`ImGuiContext::WindowsFocusOrder`, root windows, least-recently-focused first). Nothing
// is stored, nothing is published, nothing can go stale, and no call site has to remember to update it
// — which is exactly what went wrong with the four process-wide statics the authoring context replaced.
//
// WHY THE ORDER AND NOT `IsWindowFocused()`. Two reasons, both load-bearing:
//
//   * `IsWindowFocused` answers about the window currently between Begin()/End(), and this question is
//     asked from the palette and the control channel, which are not inside any viewport's window.
//   * While the command palette is open, IT has the focus and no viewport does. "Nothing is focused"
//     must not mean "fall back to the first one" — the user opened the palette from somewhere, and the
//     order still remembers which viewport that was. This is the exact case that made
//     "Viewport Camera: Top" land on the wrong pane.
//
// The rule below is stated over plain strings so that it is assertable without ImGui, a window or a
// device: `Editor/Panels/ViewportPanel/ViewportPanel.cpp` is compiled by no test suite
// (scripts/CI/UnreachedSources.sh), and a decision that lives only inside it is a decision nothing can
// check.

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace Desert::Editor
{
    // DOES @p windowName NAME THE PANEL @p panelName.
    //
    // Not equality, because the two are not equal. Every panel is drawn with
    // `ImGui::Begin( PanelDisplayTitle( GetName() ) )`, which builds "<icon>  <label>###<GetName()>" —
    // the decorated half is what the user sees and the half after the LAST "###" is the identity ImGui
    // keys the window on (IPanel.hpp says why that separation exists at all). So the match is on the
    // identity: the window's name ends with "###" followed by the panel's whole name.
    [[nodiscard]] inline bool WindowNamesPanel( std::string_view windowName, std::string_view panelName )
    {
        if ( panelName.empty() )
            return false;
        constexpr std::string_view kIdSeparator = "###";
        const size_t               suffix       = kIdSeparator.size() + panelName.size();
        if ( windowName.size() < suffix )
            return false;
        const std::string_view tail = windowName.substr( windowName.size() - suffix );
        return tail.substr( 0, kIdSeparator.size() ) == kIdSeparator &&
               tail.substr( kIdSeparator.size() ) == panelName;
    }

    // WHICH OF @p livePanelNames THE USER IS WORKING IN.
    //
    // @p focusOrder is ImGui's window focus order, LEAST recently focused first — pass
    // `ImGuiContext::WindowsFocusOrder` window names in their own order and nothing else.
    //
    // Returns the index into @p livePanelNames of the most recently focused one. Falls back to 0 (the
    // first live viewport) when none of them appears in the order at all, which is the editor's cold
    // start: the layout has been built, the windows exist, and the user has not yet clicked in one.
    // `nullopt` only when there are no viewports.
    //
    // WINDOWS THAT ARE NOT VIEWPORTS ARE SKIPPED, NOT TREATED AS "NOTHING". The palette, the Outliner
    // and the Details panel are all more recently focused than the viewport the user came from, every
    // single time a command is issued — an implementation that looked only at the most recent entry
    // would therefore answer "no viewport" for every command the editor has.
    [[nodiscard]] inline std::optional<size_t>
    ActiveViewportIndex( const std::vector<std::string_view>& livePanelNames,
                         const std::vector<std::string_view>& focusOrder )
    {
        if ( livePanelNames.empty() )
            return std::nullopt;

        for ( size_t i = focusOrder.size(); i-- > 0; )
        {
            for ( size_t panel = 0; panel < livePanelNames.size(); ++panel )
            {
                if ( WindowNamesPanel( focusOrder[i], livePanelNames[panel] ) )
                    return panel;
            }
        }
        return size_t{ 0 };
    }
} // namespace Desert::Editor
