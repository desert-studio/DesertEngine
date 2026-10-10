#pragma once

#include <glm/glm.hpp>

namespace Desert::Editor::Tools
{
    // What the cursor over the viewport says about the active tool's next click. UE asks the running tool
    // (UInteractiveTool / FEditorViewportClient::GetCursor) and the viewport sets the shape; here every
    // Modeling tool answers Cursor() from the state its last Update left, and the viewport applies the answer
    // through ApplyToolCursor - the one place a tool's cursor becomes an OS cursor.
    enum class ToolCursor
    {
        None,        // the tool has no opinion (inactive): the viewport leaves the cursor alone
        Default,     // the arrow: a click does the tool's plain thing (e.g. clears a selection)
        Place,       // a crosshair: a click puts something at the point under it
        Select,      // the hand: a click picks the element (face / edge / vertex / cell) under it
        Drag,        // four arrows: the tool is moving what it picked
        Unavailable, // the crossed circle: a click here does nothing (no surface, no mesh to edit)
    };

    // Sets the cursor for this frame while the mouse is inside the viewport rectangle and the viewport window
    // is hovered (an overlay window on top keeps its own cursor). Place hides the OS cursor and draws a
    // crosshair at the mouse, since the platform layer has no crosshair shape.
    void ApplyToolCursor( ToolCursor cursor, const glm::vec2& viewportPos, const glm::vec2& viewportSize,
                          bool viewportHovered );
} // namespace Desert::Editor::Tools
