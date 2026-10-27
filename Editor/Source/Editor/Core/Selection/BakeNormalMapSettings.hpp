#pragma once

#include <Engine/Geometry/MeshNormalMapBake.hpp>

namespace Desert::Editor::Core
{
    // What Bake Normal Map (Panels/Modeling/MeshBakeNormalTool.hpp) bakes with. Core's and not the tool's: the
    // modeling property table (ModelingStateProperties.hpp) edits these settings, and the tool itself imports
    // textures, which only the layers above Core may do.
    struct BakeNormalMapArgs
    {
        Geometry::MeshMapBakeSettings Settings;
    };

    // The Bake palette's settings, shared by the panel and the palette command.
    [[nodiscard]] BakeNormalMapArgs& BakeNormalMapSettings();
} // namespace Desert::Editor::Core
