#pragma once

#include <Editor/Core/Selection/ModelingState.hpp>

#include <glm/glm.hpp>

namespace Desert::Editor::Tools
{
    // What the viewport's tool bar shows for the active tool. UE draws ONE overlay for whatever tool is
    // running (ModelingToolsEditorModeToolkit.cpp:765-824: the tool's icon and name, then Accept + Cancel
    // when the tool can be cancelled, a single Complete when it cannot); this is that decision per tool.
    struct ActiveToolLabel
    {
        const char* Icon      = nullptr;
        const char* Name      = nullptr;
        bool        HasCancel = false; // Accept + Cancel; false = one Complete button
    };

    // nullptr Name for Tool::None: no bar.
    [[nodiscard]] ActiveToolLabel LabelOf( Core::ModelingState::Tool tool );

    // What a press of the bar does. Cancel and Complete end the tool at once, as UE's do. Accept of a
    // cancellable tool only raises its request: the tool ends ITSELF once the commit succeeded, so a refused
    // Accept leaves it open with its work instead of ending it with the work orphaned (CG1).
    inline void PressToolBar( Core::ModelingState& ms, bool hasCancel, bool finish, bool cancel )
    {
        if ( finish && hasCancel )
        {
            ms.ReqAccept         = true;
            ms.ReqAcceptEndsTool = true;
        }
        if ( cancel )
            ms.ReqCancel = true;
        if ( cancel || ( finish && !hasCancel ) )
            ms.ActiveTool = Core::ModelingState::Tool::None;
    }

    // A tool's one-shot request raised by name - the command palette and the command channel. Cancel is the
    // bar's Cancel for every tool (UE: Cancel is a tool shutdown), so it ends the tool as well as raising
    // ReqCancel; the tool's Update resolves ReqCancel whether it is active or not, which is what still discards
    // the un-accepted piece after the tool has ended (CG2). Every other request only raises its flag, as the
    // panel button does - "Accept and Start New" keeps the tool running by design.
    inline void RaiseToolRequest( Core::ModelingState& ms, bool Core::ModelingState::*request )
    {
        if ( request == &Core::ModelingState::ReqCancel )
            PressToolBar( ms, /*hasCancel*/ true, /*finish*/ false, /*cancel*/ true );
        else
            ms.*request = true;
    }

    // Bottom-centre overlay of the viewport rectangle; the buttons go through PressToolBar.
    void DrawActiveToolBar( const glm::vec2& viewportPos, const glm::vec2& viewportSize );
} // namespace Desert::Editor::Tools
