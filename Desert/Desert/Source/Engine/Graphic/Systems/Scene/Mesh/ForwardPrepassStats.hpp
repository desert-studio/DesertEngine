#pragma once

#include <cstdint>

namespace Desert::Graphic::System
{
    // THE LAST FRAME'S FORWARD DEPTH PREPASS, as the base pass of that frame admitted it (MeshRenderer::
    // GetForwardPrepassStats, DesertCtl state render.forwardPrepass). All zero on a frame that drew its meshes into
    // the G-buffer (no forward prepass ran).
    struct ForwardPrepassStats
    {
        uint32_t Draws       = 0;     // commands of "Forward: Depth Prepass" (meshes, grass sections, generics)
        uint32_t Cells       = 0;     // (material x path) cell draws admitted for the base pass (static/ISM/skinned)
        uint32_t GpuSections = 0;     // GPU-instanced sections (the landscape grass) admitted for the base pass
        bool     DepthWriteInBase = false; // a base-pass draw writes depth (only the Wireframe view's twins may)
    };
} // namespace Desert::Graphic::System
