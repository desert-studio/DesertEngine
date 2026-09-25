#pragma once

#include <string>

#include <Common/Core/UUID.hpp>
#include <Engine/Geometry/EditMeshBridge.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

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
            PolyEdit,
            ElementSelect, // mesh element selection (MeshElementSelection)
            CreateShape,   // place a parametric shape (CreateShapeTool, UE's Add Primitive tools)
        };

        // The shapes the Create tool places. Plane is not here: it is a card for the scene's Add menu, not
        // a solid anyone models from.
        enum class Shape
        {
            Box,
            Sphere,
            Cylinder,
            Cone,
            Capsule,
            Pyramid,
            Stairs,
        };
        static constexpr Shape kShapes[] = { Shape::Box,     Shape::Sphere,  Shape::Cylinder, Shape::Cone,
                                             Shape::Capsule, Shape::Pyramid, Shape::Stairs };

        static constexpr const char* ShapeName( Shape shape )
        {
            switch ( shape )
            {
                case Shape::Box:
                    return "Box";
                case Shape::Sphere:
                    return "Sphere";
                case Shape::Cylinder:
                    return "Cylinder";
                case Shape::Cone:
                    return "Cone";
                case Shape::Capsule:
                    return "Capsule";
                case Shape::Pyramid:
                    return "Pyramid";
                case Shape::Stairs:
                    return "Stairs";
            }
            return "Box";
        }

        // Where a click puts the shape: on the ground plane (Y = 0), or on whatever scene surface is under
        // the cursor, falling back to the ground where there is none (UE's placement Ground / On Scene).
        // What a creating tool's Accept leaves in the scene (UE: UCreateMeshObjectTypeProperties::OutputType).
        // StaticMesh (the default, owner decision B4) writes a new .stmesh and the entity draws the asset;
        // Dynamic keeps the EditMesh on the entity, editable in place and saved inside the scene.
        enum class OutputType
        {
            StaticMesh,
            Dynamic,
        };

        struct OutputSettings
        {
            OutputType  Type   = OutputType::StaticMesh;
            std::string Folder = "Modeling"; // under the cooked mesh folder (StaticMeshOutput.hpp says why)
            std::string Name;                // empty: the entity's name
        };

        enum class Placement
        {
            Ground,
            OnScene,
        };

        // Every field moves the shape it is shown for (ModelingPanel shows only those). Centimetres.
        struct ShapeSettings
        {
            Shape                        Kind         = Shape::Box;
            float                        Width        = 100.0f; // X extent; the diameter of a round shape
            float                        Depth        = 100.0f; // Z extent (Box, Pyramid)
            float                        Height       = 100.0f; // Y extent (all but Sphere and Stairs)
            int                          Subdivisions = 1;      // Box: quads along each edge
            int                          Slices       = 24;     // round shapes: segments around the axis
            int                          Stacks       = 16;     // Sphere, Capsule: segments pole to pole
            int                          Steps        = 8;      // Stairs
            float                        StepDepth    = 30.0f;  // Stairs
            float                        StepHeight   = 20.0f;  // Stairs
            Geometry::ShapePolygroupMode Groups       = Geometry::ShapePolygroupMode::PerFace;
            Geometry::ShapePivot         Pivot        = Geometry::ShapePivot::Base;
            Placement                    Place        = Placement::OnScene;

            bool operator==( const ShapeSettings& ) const = default;
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
        static constexpr float MaxCellSize   = 100000.0f;

        // Grid Power, the step the block size is nearest to: the slider shows it, and a free Current Block
        // Size still wins because only a write through SetGridPower snaps the size.
        [[nodiscard]] int GridPower() const
        {
            int power = 0;
            for ( float sz = BaseBlockSize; power < MaxGridPower && sz > CellSize + 0.01f; ++power )
                sz *= 0.5f;
            return power;
        }
        void SetGridPower( int power )
        {
            power    = power < 0 ? 0 : ( power > MaxGridPower ? MaxGridPower : power );
            CellSize = BaseBlockSize / static_cast<float>( 1 << power );
            CellSize = CellSize < MinCellSize ? MinCellSize : CellSize;
        }
        // The panel's /2 and x2, the Ctrl+Q / Ctrl+E shortcuts and the palette entries are these two.
        void HalveBlockSize()
        {
            CellSize = CellSize * 0.5f < MinCellSize ? MinCellSize : CellSize * 0.5f;
        }
        void DoubleBlockSize()
        {
            CellSize = CellSize * 2.0f > MaxCellSize ? MaxCellSize : CellSize * 2.0f;
        }

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

        // --- The mouse's part of CubeGrid, for the command palette and the control channel (which have no
        //     cursor). Each lands in the same code the mouse and E/Q reach inside CubeGridTool::Update. ---
        // Aim the tool from the viewport centre instead of the cursor: the hover grid, the selection a
        // request makes and Push/Pull all follow the centre while it is set.
        bool CubeGridAimCentre       = false;
        int  ReqCubeGridSelectBlocks = 0; // one-shot: select an N x N block square starting at the aim
        int  ReqCubeGridStep         = 0; // one-shot: +1 = E, -1 = Q (Push/Pull, or the corner posts)
        // One-shot: which of the selection's four corner posts Corner Mode picks, one bit per post in the
        // order of CubeGridTool's kPosts (bit k = post k); -1 = no request.
        int ReqCornerPosts = -1;

        // Select Elements: the distance Extrude / Push-Pull / Offset / Inset / Outset / Bevel use, in centimetres
        // (the panel's field, the Alt hotkeys and the palette entries all read this one value). Push/Pull and
        // Offset take its sign; the others refuse a negative one.
        float ElementOpDistance = 20.0f;
        // Insert Edge Loop: where along each ring edge the loop goes, in (0, 1).
        float ElementLoopPosition = 0.5f;
        // Clean: vertices closer than this (cm) are welded.
        float ElementWeldTolerance = 0.01f;
        // Subdivide: how many times the whole mesh is split, and whether it is smoothed (Loop) or only
        // re-tessellated (Uniform).
        int                       ElementSubdivideLevels = 1;
        Geometry::SubdivideScheme ElementSubdivideScheme = Geometry::SubdivideScheme::Loop;
        // Mirror: the plane is perpendicular to ElementMirrorAxis (0 = X, 1 = Y, 2 = Z) through the entity's
        // origin along its own axis, or - ElementMirrorWorld - through the world's origin along the world's.
        // Cut and Mirror keeps the positive side of that axis, the negative one with ElementMirrorKeepNegative.
        // The seam welds within ElementWeldTolerance.
        int                  ElementMirrorAxis         = 0;
        bool                 ElementMirrorWorld        = false;
        bool                 ElementMirrorKeepNegative = false;
        Geometry::MirrorMode ElementMirrorMode         = Geometry::MirrorMode::CutAndMirror;
        // Plane Cut: the plane is perpendicular to ElementPlaneCutAxis, ElementPlaneCutOffset cm along that axis
        // from the entity's origin (or, ElementPlaneCutWorld, the world's). The positive side of the axis is
        // kept, the negative one with ElementPlaneCutKeepNegative; Keep Both Halves puts the other half on a
        // new entity. ElementPlaneCutFill closes the cut with a flat cap.
        int                    ElementPlaneCutAxis         = 0;
        float                  ElementPlaneCutOffset       = 0.0f;
        bool                   ElementPlaneCutWorld        = false;
        bool                   ElementPlaneCutKeepNegative = false;
        bool                   ElementPlaneCutFill         = true;
        Geometry::PlaneCutMode ElementPlaneCutMode         = Geometry::PlaneCutMode::DiscardNegativeSide;
        // Trim: the entity whose mesh (closed and convex) trims the edited one, picked in the panel from the
        // scene selection; Null until picked.
        Common::UUID       ElementTrimCutter;
        Geometry::TrimSide ElementTrimSide = Geometry::TrimSide::RemoveInside;

        // XForm tab (MeshXformOperations.hpp), acting on the scene selection's entities. Edit Pivot moves the
        // origin to XformPivot (XformPivotWorldPoint for World Point); Bake Transform bakes the XformBake
        // parts; Split cuts by XformSplit; Pattern lays XformPattern out, merged into the source or - with
        // XformPatternSeparate - as new entities.
        Geometry::PivotLocation   XformPivot = Geometry::PivotLocation::BoundsBase;
        glm::vec3                 XformPivotWorldPoint{ 0.0f };
        Geometry::BakeOptions     XformBake;
        Geometry::SplitMethod     XformSplit = Geometry::SplitMethod::ConnectedComponents;
        Geometry::PatternSettings XformPattern;
        bool                      XformPatternSeparate = false;

        // Create tool: the shape a click places, and a one-shot that places it where the viewport centre
        // looks (the palette's way to place without a mouse).
        ShapeSettings CreateShape;
        OutputSettings Output; // shared by Create Shape and Cube Grid, as UE's modeling mode shares it
        bool          ReqPlaceCentre = false;
        // Tool -> panel (read-only stats for the properties panel)
        int  Cubes      = 0;
        bool CornerMode = false; // the tool is currently in Corner Mode
    };
} // namespace Desert::Editor::Core
