#pragma once

#include <glm/glm.hpp>

namespace Desert::Editor::Core
{
    // Shared state between the docked ModelingPanel (the UE5-style tool palette + properties) and the
    // viewport tool that actually edits geometry (CubeGridTool). The panel selects the active tool + edits
    // its properties + posts Accept/Cancel/Clear requests; the tool reads them and reports back its stats.
    // A tiny global singleton mirroring ViewportMode.
    class ModelingState
    {
    public:
        enum class Tool
        {
            None = 0,
            CubeGrid,
            PolyEdit
        };

        static ModelingState& Get()
        {
            static ModelingState s;
            return s;
        }

        // Smallest CubeGrid block, in world units (= 1 cm). Both the panel and the tool clamp to it.
        static constexpr float MinCellSize = 1.0f;
        // Grid Power 0 is a one-metre block; each step halves it (Power 2 = 25 cm), like UE's Grid Power.
        static constexpr float BaseBlockSize = 100.0f;
        static constexpr int   MaxGridPower  = 6;

        // Panel -> tool
        // No Output Type here on purpose: Accept has exactly one output today (a StaticMeshComponent carrying
        // the live mesh inline), so a Static/Dynamic choice would be a knob nothing reads. It comes back
        // with the reader that makes it mean something, defaulting to Static Mesh (owner's decision).
        Tool ActiveTool = Tool::None;
        // CubeGrid block size (Resize Grid) in world units — one unit is one centimetre, so the default
        // 100 is a one-metre block, the same default as UE.
        float CellSize = 100.0f;
        int   BlocksPerStep = 1; // cells extruded/removed per Push/Pull (UE "Blocks Per Step")

        // --- Grid frame (UE "Grid Reinitialization" / "Options") ---
        // World position of grid cell (0,0,0). Moving it re-aligns the lattice to an object's corner so
        // any block size stays flush with it, instead of tiling from the world origin.
        glm::vec3 GridOrigin = glm::vec3( 0.0f );
        // Targeting also considers OTHER scene meshes (build on top of an imported prop, snap the plane
        // onto it), not just the blockout being edited. UE calls this "Hit Unrelated Geometry".
        bool HitUnrelated = true;
        bool ShowGizmo    = false; // draw the grid frame axes at the origin

        // Corner Mode (Z): moves the selection's corner posts along the grid's up axis to build ramps,
        // roofs and wedges. Snap Size = the fraction of a block one press moves them.
        int  CornerSnapDiv = 2;     // 2 = half a block, 4 = quarter, 10 = a tenth
        bool ReqCornerMode = false; // one-shot: toggle Corner Mode

        // Accept also gives the committed piece a BOX collider + a static body, so a blockout is
        // walkable immediately. Box, not triangle mesh: the physics layer has no trimesh shape yet.
        bool GenerateCollision = true;

        bool ReqAccept         = false; // one-shot: commit the blockout, start a fresh one
        bool ReqCancel         = false; // one-shot: delete the in-progress blockout
        bool ReqClear          = false; // one-shot: clear the cells (keep editing)
        bool ReqResetFromActor = false; // one-shot: put the grid origin on the selected entity

        // Tool -> panel (read-only stats for the properties panel)
        int  Cubes      = 0;
        bool CornerMode = false; // the tool is currently in Corner Mode
    };
} // namespace Desert::Editor::Core
