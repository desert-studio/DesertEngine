#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Geometry/EditMeshTopologyOperations.hpp>

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
    };

    // What an operation reads besides the selection. The panel, the hotkeys and the palette build it from
    // ModelingState (ArgsFromModelingState) - one value per knob; the knife adds its plane.
    struct MeshOperationArgs
    {
        float Distance      = 0.0f;  // Extrude .. Outset (cm, signed for Push/Pull and Offset); Bevel's width
        float LoopPosition  = 0.5f;  // Insert Edge Loop, in (0, 1) along each ring edge
        float WeldTolerance = 0.01f; // Clean, cm
        // Cut only: the plane in the MESH's space (the knife maps its screen line through the entity's
        // transform). Absent, Cut is refused - it has no line to cut along.
        std::optional<Geometry::CutPlane> CutPlane;
    };

    [[nodiscard]] const char* ToString( MeshOperation operation );
    // Extrude .. Outset and Bevel read MeshOperationArgs::Distance; the others do not.
    [[nodiscard]] bool TakesDistance( MeshOperation operation );

    // ModelingState's ElementOpDistance / ElementLoopPosition / ElementWeldTolerance, no cut plane.
    [[nodiscard]] MeshOperationArgs ArgsFromModelingState();

    [[nodiscard]] Common::BoolResultStr ApplyMeshOperation( ::Desert::Core::Scene& scene, MeshOperation operation,
                                                            const MeshOperationArgs& args );
} // namespace Desert::Editor::Core
