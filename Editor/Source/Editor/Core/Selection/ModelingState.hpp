#pragma once

#include <optional>

#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>
#include <Editor/Core/Selection/MeshBooleanTool.hpp>
#include <Engine/Geometry/DynamicMeshSculpt.hpp>
#include <Engine/Geometry/DynamicMeshUVOperations.hpp>
#include <Engine/Geometry/DynamicMeshXformOperations.hpp>
#include <Engine/Geometry/MeshDeformOperations.hpp>
#include <Engine/Geometry/MeshHoleFillOperation.hpp>
#include <Engine/Geometry/MeshJacketOperation.hpp>
#include <Engine/Geometry/MeshProjectOperation.hpp>
#include <Engine/Geometry/MeshWeldEdgesOperation.hpp>
#include <Engine/Geometry/MeshVoxelOperations.hpp>
#include <Engine/Geometry/MeshPlaneOperation.hpp>
#include <Engine/Geometry/MeshRegionOperation.hpp>
#include <Engine/Geometry/MeshAttributeOperations.hpp>
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
            // UE's UVs palette (UVTool): each previews on the selected mesh and commits on Accept.
            AutoUV,
            UVProject,
            UVUnwrap,
            UVLayout,
            UVTransform, // UE's UV Editor Transform tool (UUVEditorTransformTool)
            // UE's Deform palette (MeshProcessingTool): previews on the selected mesh and commits on Accept.
            Smooth,
            Offset,
            Warp,    // UE's UMeshSpaceDeformerTool: Bend / Twist / Flare in the frame of its gizmo
            Lattice, // UE's ULatticeDeformerTool: a control-point lattice over the mesh's bounds (FFD)
            // UE's Attributes palette (same preview/Accept session as Deform, MeshProcessingTool).
            Tangents,      // UE's UMeshTangentsTool: MikkTSpace / Fast tangent frames, previewed as lines
            Normals,       // UE's UEditNormalsTool: recompute / split / invert the normal overlay
            EditMaterials, // UE's UEditMeshMaterialsTool: selected triangles -> a material slot
                           // (EditMaterialsTool)
            PaintGroups, // UE's UMeshGroupPaintTool: a brush / flood fill paints triangle groups (PaintGroupsTool)
            // UE's MeshOps palette (same preview/Accept session as Deform, MeshProcessingTool).
            HoleFill,  // UE's UHoleFillTool: the selected boundary loops closed Trivial / Minimal / Smooth
            Remesh,    // UE's URemeshMeshTool: the whole mesh remeshed to an even, isotropic triangulation
            Simplify,  // UE's USimplifyMeshTool: the whole mesh reduced by QEM edge collapses
            Jacket,    // UE's URemoveOccludedTrianglesTool: the triangles hidden inside the mesh removed
            Project,   // UE's UProjectToTargetTool: the first selected mesh remeshed onto the second's surface
            WeldEdges, // UE's UWeldMeshEdgesTool: coincident open edges merged (FMergeCoincidentMeshEdges)
            // UE's VoxOps palette (same preview/Accept session, MeshProcessingTool).
            VoxWrap,    // UE's UVoxelSolidifyMeshesTool: winding-number solid of the mesh, re-meshed closed
            VoxBoolean, // UE's UVoxelCSGMeshesTool: CSG of the two selected meshes' fields -> a new entity
            VoxMorph,   // UE's UVoxelMorphologyMeshesTool: Dilate/Contract/Close/Open of the mesh's field -> a new
                        // entity
            VoxBlend,   // UE's UVoxelBlendMeshesTool: the two selected meshes' fields blended -> a new entity
            // UE's Deform palette: Sculpt (UMeshVertexSculptTool) - brush strokes on the selected mesh
            // (Standard, Smooth, Move; Geometry/DynamicMeshSculpt.hpp).
            Sculpt,
        };

        // The shapes the Create tool places: UE's Shapes palette (EMakeMeshShapeType) plus the Pyramid, and UE's
        // revolve generators (FRevolvePlanarPolygonGenerator, FSpiralRevolvePlanarPolygonGenerator) as Revolve,
        // Spiral. Plane is not here: it is an upright card for the scene's Add menu; the palette's flat card is
        // Rectangle.
        enum class Shape
        {
            Box,
            Sphere,
            Cylinder,
            Cone,
            Capsule,
            Pyramid,
            Stairs,
            Torus,
            Arrow,
            Disc,
            Rectangle,
            Revolve,
            Spiral,
        };
        static constexpr Shape kShapes[] = { Shape::Box,     Shape::Sphere,  Shape::Cylinder,  Shape::Cone,
                                             Shape::Capsule, Shape::Pyramid, Shape::Stairs,    Shape::Torus,
                                             Shape::Arrow,   Shape::Disc,    Shape::Rectangle, Shape::Revolve,
                                             Shape::Spiral };

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
                case Shape::Torus:
                    return "Torus";
                case Shape::Arrow:
                    return "Arrow";
                case Shape::Disc:
                    return "Disc";
                case Shape::Rectangle:
                    return "Rectangle";
                case Shape::Revolve:
                    return "Revolve";
                case Shape::Spiral:
                    return "Spiral";
            }
            return "Box";
        }

        // Where a click puts the shape: on the ground plane (Y = 0), or on whatever scene surface is under
        // the cursor, falling back to the ground where there is none (UE's placement Ground / On Scene).
        // What a creating tool's Accept leaves in the scene (UE: UCreateMeshObjectTypeProperties::OutputType).
        // StaticMesh (the default, owner decision B4) writes a new .stmesh and the entity draws the asset;
        // Dynamic keeps the editable mesh on the entity, editable in place and saved inside the scene.
        enum class OutputType
        {
            StaticMesh,
            Dynamic,
        };

        struct OutputSettings
        {
            OutputType  Type   = OutputType::StaticMesh;
            std::string Folder = "Modeling"; // under the mesh folder, Assets/Meshes (StaticMeshOutput.hpp)
            std::string Name;                // empty: the entity's name
        };

        enum class Placement
        {
            Ground,
            OnScene,
        };

        // The Pyramid is not in UE's palette, so it has no UE property set; its three extents, in centimetres.
        struct PyramidSettings
        {
            float Width  = 100.0f; // X
            float Depth  = 100.0f; // Z
            float Height = 100.0f; // Y

            bool operator==( const PyramidSettings& ) const = default;
        };

        // One property set per shape, as UE keeps one UProcedural*ToolProperties per Add Primitive tool: each
        // shape remembers its own values, with UE's names and defaults. ModelingPanel shows the chosen one's.
        struct ShapeSettings
        {
            Shape                        Kind = Shape::Box;
            Geometry::BoxShape           Box;
            Geometry::SphereShape        Sphere;
            Geometry::CylinderShape      Cylinder;
            Geometry::ConeShape          Cone;
            Geometry::CapsuleShape       Capsule;
            PyramidSettings              Pyramid;
            Geometry::StairsShape        Stairs;
            Geometry::TorusShape         Torus;
            Geometry::ArrowShape         Arrow;
            Geometry::DiscShape          Disc;
            Geometry::RectangleShape     Rectangle;
            Geometry::RevolveShape       Revolve;
            Geometry::SpiralShape        Spiral;
            Geometry::ShapePolygroupMode Groups = Geometry::ShapePolygroupMode::PerFace;
            Geometry::ShapePivot         Pivot  = Geometry::ShapePivot::Base;
            Placement                    Place  = Placement::OnScene;
            // UE's bAlignToNormal (UAddPrimitiveTool's shape settings, on by default): the shape's up axis
            // turns to the normal of the surface it is placed on; off, it stands upright wherever it lands.
            bool AlignToNormal = true;

            bool operator==( const ShapeSettings& ) const = default;
        };

        static ModelingState& Get()
        {
            static ModelingState s;
            return s;
        }

        // Smallest CubeGrid block, in world units (= 1 cm). Both the panel and the tool clamp to it.
        static constexpr float MinCellSize = 1.0f;
        // CUBEGRID GRID POWER = UE FCubeGrid (CubeGrid.cpp; UCubeGridToolProperties::GridPower/BlockBaseSize,
        // CubeGridTool.h): the cell is BlockBaseSize * 2^GridPower, so Power up = a BIGGER cell. The default base
        // 3.125 cm at Power 5 is UE's one-metre block. GridPower and BlockBaseSize are the only stored values; the
        // cell size is derived from them everywhere (panel, tool, overlay, control channel).
        static constexpr float DefaultBlockBaseSize = 3.125f;
        static constexpr int   DefaultGridPower     = 5;
        static constexpr float MaxCellSize          = 100000.0f;
        static constexpr int   MaxGridPower = 14; // 3.125 cm * 2^14 = 512 m, the largest step under MaxCellSize

        [[nodiscard]] float CellSize() const
        {
            return BlockBaseSize * static_cast<float>( 1 << GridPower );
        }
        void SetGridPower( int power )
        {
            GridPower = power < 0 ? 0 : ( power > MaxGridPower ? MaxGridPower : power );
            while ( GridPower > 0 && CellSize() > MaxCellSize ) // a re-based (larger) base tops out sooner
                --GridPower;
        }
        // UE Ctrl+E / Ctrl+Q (the panel's x2 and /2, the palette's "Grid Power +1/-1"): one power step.
        void GridPowerUp()
        {
            SetGridPower( GridPower + 1 );
        }
        void GridPowerDown()
        {
            SetGridPower( GridPower - 1 );
        }
        // A typed Current Block Size (UE CurrentBlockSize): the power stays, the base is re-derived so
        // BlockBaseSize * 2^GridPower is exactly the size asked for — then Power up/down still doubles/halves it.
        void SetCurrentBlockSize( float size )
        {
            size          = size < MinCellSize ? MinCellSize : ( size > MaxCellSize ? MaxCellSize : size );
            BlockBaseSize = size / static_cast<float>( 1 << GridPower );
        }

        // Panel -> tool
        // No Output Type here on purpose: Accept has exactly one output today (a StaticMeshComponent carrying
        // the live mesh inline), so a Static/Dynamic choice would be a knob nothing reads. It comes back
        // with the reader that makes it mean something, defaulting to Static Mesh (owner's decision).
        Tool ActiveTool = Tool::None;
        // LOD MANAGER (UE 5.8 LODManagerTool, Modeling panel -> LODs): the level the active viewport previews the
        // selected mesh at, -1 = Auto (the renderer's screen-size pick). A preview, not an edit: the panel renews
        // the renderer's per-view lease each frame while its LODs tab is open, so leaving the tab returns Auto.
        int LODPreviewLevel = -1;
        // One-shot: the control channel's "LOD Manager: open" (true) / "close" (false) for the panel's rail.
        std::optional<bool> ReqLODManager;
        // CubeGrid grid (UE FCubeGrid): cell = BlockBaseSize * 2^GridPower in world units (1 unit = 1 cm); the
        // defaults give UE's one-metre block. Write them through SetGridPower / SetCurrentBlockSize.
        int   GridPower     = DefaultGridPower;
        float BlockBaseSize = DefaultBlockBaseSize;
        int   BlocksPerStep = 1; // cells extruded/removed per Push/Pull (UE "Blocks Per Step")

        // --- Grid frame (UE "Grid Reinitialization" / "Options") ---
        // World position of grid cell (0,0,0). Moving it re-aligns the lattice to an object's corner so
        // any block size stays flush with it, instead of tiling from the world origin.
        glm::vec3 GridOrigin = glm::vec3( 0.0f );
        // Orientation of the grid frame, Euler degrees about X, Y, Z (UE "Grid Frame Orientation"): the lattice
        // runs along the turned axes, so a blockout can follow a rotated building or a slope.
        glm::vec3 GridRotation = glm::vec3( 0.0f );
        // Targeting also considers OTHER scene meshes (build on top of an imported prop, snap the plane
        // onto it), not just the blockout being edited. UE calls this "Hit Unrelated Geometry".
        bool HitUnrelated = true;
        bool ShowGizmo    = false; // draw the grid frame axes at the origin

        // Corner Mode (Z): moves the selection's corner posts out of (or into) the face it lies on to build
        // ramps, roofs, wedges and leaning walls. Snap Size = the fraction of a block one press moves them.
        int  CornerSnapDiv = 2;     // 2 = half a block, 4 = quarter, 10 = a tenth
        bool ReqCornerMode = false; // one-shot: toggle Corner Mode
        // UE's Crosswise Diagonal (X in Corner Mode): split a non-planar sloped quad along the other diagonal.
        bool CornerCrosswise = false;

        // Accept also gives the committed piece a Mesh collider (its own triangles) + a static body, so a
        // blockout is walkable immediately, rooms and doorways included (Tools/BlockoutCollision.hpp).
        bool GenerateCollision = true;

        bool ReqAccept         = false; // one-shot: commit the blockout, start a fresh one
        // With ReqAccept: the tool bar's Accept - end the tool too, but only once the commit succeeded
        // (a refused Accept keeps the tool open; BlockoutSession.hpp).
        bool ReqAcceptEndsTool = false;
        bool ReqCancel         = false; // one-shot: delete the in-progress blockout
        bool ReqClear          = false; // one-shot: clear the cells (keep editing)
        bool ReqResetFromActor = false; // one-shot: put the grid origin on the selected entity
        bool ReqCubeGridEditSelected = false; // one-shot: reopen CubeGrid on the selected blockout

        // --- The mouse's part of CubeGrid, for the command palette and the control channel (which have no
        //     cursor). Each lands in the same code the mouse and E/Q reach inside CubeGridTool::Update. ---
        // Aim the tool from the viewport centre instead of the cursor: the hover grid, the selection a
        // request makes and Push/Pull all follow the centre while it is set.
        bool CubeGridAimCentre       = false;
        int  ReqCubeGridSelectBlocks = 0; // one-shot: select an N x N block square starting at the aim
        int  ReqCubeGridStep         = 0; // one-shot: +1 = E, -1 = Q (Push/Pull, or the corner posts)
        int  ReqCubeGridSlide        = 0; // one-shot: +1 = Shift+E (slide back / out), -1 = Shift+Q
        bool ReqCubeGridPaint        = false; // one-shot: Shift+B (the Quick Material onto the selection)
        bool ReqCubeGridPivot        = false; // one-shot: Ctrl+MMB (grid pivot onto the aimed face's corner)

        // Quick Materials (UE CubeGrid's Material property): the material Push/Pull gives every face it
        // creates and Shift+B paints onto the selected faces. Null = the engine default material. The tool
        // turns it into a per-face material ID through the blockout's own material set.
        Common::AssetHandle QuickMaterial; // default-constructed = null
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
        // Subdivide: UE's Subdivide tool settings with its defaults (level 3, Catmull-Clark, smooth corners,
        // generated normals).
        Geometry::SubdivideSettings ElementSubdivide{};
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
        // Boolean and Trim (MeshBooleanTool.hpp), on the scene selection's two entities.
        BooleanToolArgs Boolean;

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

        // UVs palette (UVTool, Geometry/DynamicMeshUVOperations.hpp): one settings block per tool; the tool
        // previews the selected mesh with them and recomputes whenever they change.
        Geometry::AutoUVSettings    UVAuto;
        Geometry::UVProjectSettings UVProject;
        Geometry::UVUnwrapSettings  UVUnwrap;
        Geometry::UVLayoutSettings  UVLayout;
        Geometry::UVTransformSettings UVTransform;
        // What the active UV tool's preview did (its report), or why there is none - the panel shows it.
        std::string UVStatus;
        // UE's UV tools' material mode Checkerboard (UExistingMeshMaterialProperties): the previewed mesh drawn
        // with a checker of its new UVs, CheckerDensity cells per UV unit (UE default 20).
        bool  UVShowChecker    = true;
        float UVCheckerDensity = 20.0f;

        // Deform palette (MeshProcessingTool, Geometry/MeshDeformOperations.hpp): UE's Smooth and Offset tools' property
        // sets; the tool previews the selected mesh with them and recomputes whenever they change.
        Geometry::SmoothSettings DeformSmooth;
        Geometry::OffsetSettings DeformOffset;
        // VoxOps -> VoxWrap (UE UVoxelSolidifyMeshesTool, Geometry/MeshVoxelOperations.hpp).
        Geometry::VoxWrapSettings VoxWrap;
        // VoxOps -> VoxBoolean (UE UVoxelCSGMeshesTool): its operation and voxel settings, and what Accept does
        // with the two inputs (UE's HandleSourcesProperties; the result is always a new entity, UE
        // Create-From-Selected).
        Geometry::VoxBooleanSettings VoxBoolean;
        BooleanInputs                VoxBooleanInputs = BooleanInputs::Delete;
        // VoxOps -> VoxMorph (UE UVoxelMorphologyMeshesTool, also a Create-From-Selected tool): its operation,
        // distance and voxel settings, and what Accept does with the input.
        Geometry::VoxMorphSettings VoxMorph;
        BooleanInputs              VoxMorphInputs = BooleanInputs::Delete;
        // VoxOps -> VoxBlend (UE UVoxelBlendMeshesTool, Create-From-Selected on two inputs): its operation, blend
        // falloff/power and voxel settings, and what Accept does with the inputs.
        Geometry::VoxBlendSettings VoxBlend;
        BooleanInputs              VoxBlendInputs = BooleanInputs::Delete;
        // Warp (UE UMeshSpaceDeformerTool): its operation settings and its gizmo - a rigid world frame whose Z is
        // the deformation axis and whose origin the bounds are measured from. The tool places it on the mesh's
        // bounds centre with Z along the longest side when it starts (UE Setup), then the viewport gizmo moves it.
        Geometry::WarpSettings DeformWarp;
        glm::mat4              WarpGizmo{ 1.0f };
        // Lattice (UE ULatticeDeformerTool): resolution + interpolation, the control points (object space of the
        // edited mesh, numbered as Geometry::FFDLattice::Index; the tool fills them at start and whenever the
        // resolution changes, which resets the lattice as in UE), the selected points (UE
        // ULatticeControlPointsMechanic selection) and the gizmo on them: a world frame placed on the
        // selection's centroid; moving or turning it carries the selected points along.
        Geometry::LatticeSettings DeformLattice;
        std::vector<glm::dvec3>   LatticePoints;
        std::vector<int32_t>      LatticeSelection;
        glm::mat4                 LatticeGizmo{ 1.0f };
        // Attributes palette -> Normals (UE UEditNormalsProperties, Geometry/MeshAttributeOperations.hpp);
        // previewed by MeshProcessingTool like the Deform tools, its report in DeformStatus.
        Geometry::NormalsSettings AttribNormals;
        // Attributes palette -> Tangents (UE UMeshTangentsToolProperties), the same session.
        Geometry::TangentsSettings AttribTangents;
        // MeshOps palette -> Hole Fill (UE UHoleFillToolProperties / USmoothHoleFillProperties), the same session.
        // The holes are the session input's MeshBoundaryLoops, numbered as its m_Loops; the selected ones are
        // filled in the preview. Select All / Clear are UE's tool actions; a click on a hole's edge toggles it.
        // HoleFillLoopCount is what the tool reports for the panel (0 while no session).
        Geometry::HoleFillSettings MeshHoleFill;
        std::vector<int32_t>       HoleFillSelected;
        bool                       HoleFillReqSelectAll = false;
        bool                       HoleFillReqClear     = false;
        int32_t                    HoleFillLoopCount    = 0;
        // MeshOps palette -> Jacket (UE URemoveOccludedTrianglesToolProperties), the same session.
        Geometry::JacketSettings MeshJacket;
        // MeshOps palette -> Remesh (UE URemeshMeshToolProperties), the same session: the preview is the remeshed
        // mesh, Accept is ONE undo step (lengths in world cm).
        Geometry::RemeshMeshSettings MeshRemesh;
        // MeshOps palette -> Simplify (UE USimplifyMeshToolProperties), the same session: the preview is the
        // simplified mesh, Accept is ONE undo step (Edge Length in world cm).
        Geometry::SimplifySettings MeshSimplify;
        // MeshOps palette -> Project (UE UProjectToTargetTool's URemeshMeshToolProperties), the same session.
        Geometry::ProjectSettings MeshProject;
        // MeshOps palette -> Weld Edges (UE UWeldMeshEdgesToolProperties), the same session.
        Geometry::WeldEdgesSettings MeshWeldEdges;
        // Attributes palette -> Edit Materials (UE UEditMeshMaterialsTool, EditMaterialsTool): the slot Assign
        // writes, the tool's one-shot requests, and what it reports for the panel (its slots, its status).
        int                      EditMaterialsActiveSlot    = 0;
        bool                     EditMaterialsReqAssign     = false;
        bool                     EditMaterialsReqAddSlot    = false;
        bool                     EditMaterialsReqRemoveSlot = false;
        std::string              EditMaterialsReqSlotMaterial; // a material asset path for the active slot
        std::vector<std::string> EditMaterialsSlotNames;
        std::string              EditMaterialsStatus;
        // Attributes palette -> Paint PolyGroups (UE UMeshGroupPaintTool / UGroupPaintBrushFilterProperties,
        // PaintGroupsTool): Brush paints the triangles under the brush, Fill floods the clicked group's connected
        // region; the group they write, the brush radius (world cm), New Group, and the tool's report.
        enum class PaintGroupsModeKind
        {
            Brush,
            Fill,
        };
        PaintGroupsModeKind PaintGroupsMode        = PaintGroupsModeKind::Brush;
        int                 PaintGroupsCurrent     = 0;
        float               PaintGroupsRadius      = 25.0f;
        bool                PaintGroupsReqNewGroup = false;
        std::string         PaintGroupsStatus;
        // What the active Deform tool's preview did (its report), or why there is none - the panel shows it.
        std::string DeformStatus;

        // Sculpt tool (SculptTool, Geometry/DynamicMeshSculpt.hpp): the brush every stroke uses.
        Geometry::SculptBrushSettings Sculpt;
        // Dynamic Sculpt (UE's UDynamicMeshSculptTool): the same tool with the brush's region remeshed toward
        // DynamicSculptTopology.TargetEdgeLength before every stamp (Refine = splits, Collapse = collapses).
        bool                     SculptDynamic = false;
        Geometry::RemeshSettings DynamicSculptTopology{ .TargetEdgeLength = 2.0f };

        // Create tool: the shape a click places (the palette places it at the viewport centre directly,
        // ViewportPanel::PlaceShapeInActiveViewport, so its refusal reaches the command's result).
        ShapeSettings CreateShape;
        OutputSettings Output; // shared by Create Shape and Cube Grid, as UE's modeling mode shares it
        // Tool -> panel (read-only stats for the properties panel)
        int  Cubes      = 0;
        bool CornerMode = false; // the tool is currently in Corner Mode
    };
} // namespace Desert::Editor::Core
