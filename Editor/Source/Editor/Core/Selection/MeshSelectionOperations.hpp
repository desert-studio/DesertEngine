#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>

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
    };

    [[nodiscard]] const char* ToString( MeshOperation operation );
    // Delete takes no distance; every other operation reads ModelingState::ElementOpDistance.
    [[nodiscard]] bool TakesDistance( MeshOperation operation );

    [[nodiscard]] Common::BoolResultStr ApplyMeshOperation( ::Desert::Core::Scene& scene, MeshOperation operation,
                                                            float distance );
} // namespace Desert::Editor::Core
