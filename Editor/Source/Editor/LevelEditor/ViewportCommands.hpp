#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <vector>

namespace Desert::Editor
{
    // THE SNAP STEPS A PERSON ACTUALLY USES, named once for the same reason the menu bar's menus are. Read by
    // DrawSnapControl, which draws them as the magnet popup's list, and by AppendViewportCommands, which
    // offers exactly these as commands — so the palette cannot offer a step the toolbar does not, which
    // is the shape a hand-copied second list always ends up in.
    //
    // Translation in CENTIMETRES because 1 world unit IS 1 cm here, so the label and the value are the
    // same number and nothing has to be converted in anyone's head.
    inline constexpr float kGridSteps[]  = { 1.0f, 5.0f, 10.0f, 25.0f, 50.0f, 100.0f, 500.0f };
    inline constexpr float kAngleSteps[] = { 1.0f, 5.0f, 10.0f, 15.0f, 30.0f, 45.0f, 90.0f };

    // The level viewport's groups of the command palette: Snap (the steps above), Transform (the gizmo tools
    // and their space), the Perf HUD action, the pilot's Eject, and View (grid, authoring modes). Stateless:
    // every entry goes through the same GizmoState / EditorPreferences / ViewportPanel door the toolbar uses.
    void AppendViewportCommands( std::vector<PaletteCommand>& commands );
} // namespace Desert::Editor
