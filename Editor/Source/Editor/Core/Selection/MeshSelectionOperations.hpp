#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <Engine/Geometry/EditMeshBridge.hpp> // the EditMesh operations cross here until P11-P17

#include <cstdint>
#include <optional>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::Core
{
    // The Select Elements tool's verbs (UE's PolyEdit Delete / Extrude / Push-Pull / Offset / Inset / Outset):
    // run a Geometry operation (EditMeshOperations.hpp) on the tracked entity's mesh and current selection,
    // put the result on the component through SetEditableMesh (its only writer), select what the operation
    // produced, and record the mesh AND that selection as ONE undo step. A refusal changes nothing and comes
    // back as the operation's own message.
    enum class MeshOperation : uint8_t
    {
        Delete,
        Extrude,
        PushPull,
        Offset,
        Inset,
        Outset,
        Bevel,
        InsertEdgeLoop,
        Cut,
        Clean,
        Subdivide, // mesh-wide, like Clean: no selection in, none out
        Mirror,    // mesh-wide
        PlaneCut,  // mesh-wide; Keep Both Halves splits the entity in two
        Trim,      // mesh-wide, by another entity's mesh
    };

    // What an operation reads besides the selection. The panel, the hotkeys and the palette build it from
    // ModelingState (ArgsFromModelingState) - one value per knob; the knife adds its plane.
    struct MeshOperationArgs
    {
        float Distance      = 0.0f;  // Extrude .. Outset (cm, signed for Push/Pull and Offset); Bevel's width
        float LoopPosition  = 0.5f;  // Insert Edge Loop, in (0, 1) along each ring edge
        float WeldTolerance   = 0.01f; // Clean, and Mirror's seam, cm
        int   SubdivideLevels = 1;
        Geometry::SubdivideScheme SubdivideScheme = Geometry::SubdivideScheme::Loop;
        int                       MirrorAxis      = 0;     // 0 = X, 1 = Y, 2 = Z
        bool                      MirrorWorld     = false; // the world's axis through its origin, not the entity's
        bool                      MirrorKeepNegative = false;
        Geometry::MirrorMode      MirrorMode         = Geometry::MirrorMode::CutAndMirror;
        int                       PlaneCutAxis         = 0;    // 0 = X, 1 = Y, 2 = Z
        float                     PlaneCutOffset       = 0.0f; // cm along the axis from the origin
        bool                      PlaneCutWorld        = false;
        bool                      PlaneCutKeepNegative = false;
        bool                      PlaneCutFill         = true;
        Geometry::PlaneCutMode    PlaneCutMode         = Geometry::PlaneCutMode::DiscardNegativeSide;
        Common::UUID              TrimCutter; // Null: no cutter picked, Trim is refused
        Geometry::TrimSide        TrimSide = Geometry::TrimSide::RemoveInside;
        // Cut only: the plane in the MESH's space (the knife maps its screen line through the entity's
        // transform). Absent, Cut is refused - it has no line to cut along.
        std::optional<Geometry::CutPlane> CutPlane;
    };

    [[nodiscard]] const char* ToString( MeshOperation operation );
    // Extrude .. Outset and Bevel read MeshOperationArgs::Distance; the others do not.
    [[nodiscard]] bool TakesDistance( MeshOperation operation );

    // ModelingState's ElementOpDistance / ElementLoopPosition / ElementWeldTolerance and the Subdivide / Mirror
    // knobs, no cut plane.
    [[nodiscard]] MeshOperationArgs ArgsFromModelingState();

    [[nodiscard]] Common::BoolResultStr ApplyMeshOperation( ::Desert::Core::Scene& scene, MeshOperation operation,
                                                            const MeshOperationArgs& args );
} // namespace Desert::Editor::Core
