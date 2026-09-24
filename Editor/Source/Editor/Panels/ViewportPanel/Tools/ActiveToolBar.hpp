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

    // Bottom-centre overlay of the viewport rectangle. Accept/Cancel of a cancellable tool raise its
    // one-shot requests (the tool resolves them on its next Update, active or not) and end the tool;
    // Complete ends it. Every button ends the tool, as UE's do.
    void DrawActiveToolBar( const glm::vec2& viewportPos, const glm::vec2& viewportSize );
} // namespace Desert::Editor::Tools
