#pragma once

#include <Editor/Core/CommandPalette.hpp>

#include <memory>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    // The Clouds group of the command palette: one entry per stage of the sky, each opening the Clouds window
    // ON that stage (CloudsPanel::OpenAt, the Details panel's own door), and "Capture current sky" on the
    // selected layer (CaptureCurrentSky, the Details button's own function).
    void AppendCloudCommands( std::vector<PaletteCommand>&                  commands,
                              const std::shared_ptr<::Desert::Core::Scene>& scene );
} // namespace Desert::Editor
