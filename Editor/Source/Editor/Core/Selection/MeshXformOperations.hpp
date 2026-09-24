#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Geometry/EditMeshXformOperations.hpp>

#include <cstdint>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::Core
{
    // UE's XForm tab: operations on WHOLE ENTITIES of the scene selection (not on a mesh-element selection, as
    // MeshSelectionOperations are), each with an editable mesh. Every call is one undo step.
    enum class XformOperation : uint8_t
    {
        EditPivot,     // every selected entity: the origin moves, the mesh moves back
        BakeTransform, // every selected entity: rotation / scale (/ location) into the vertices
        Merge,         // the selection into its FIRST entity; the others are deleted
        Split,         // every selected entity: part 0 stays, each other part becomes a new entity
        Pattern,       // every selected entity: copies merged into it, or new entities
    };

    [[nodiscard]] const char* ToString( XformOperation operation );

    struct XformArgs
    {
        Geometry::PivotLocation   Pivot = Geometry::PivotLocation::BoundsBase;
        glm::vec3                 PivotWorldPoint{ 0.0f };
        Geometry::BakeOptions     Bake;
        Geometry::SplitMethod     Split = Geometry::SplitMethod::ConnectedComponents;
        Geometry::PatternSettings Pattern;
        bool                      PatternSeparate = false;
    };

    [[nodiscard]] XformArgs XformArgsFromModelingState();

    // Refused, with the scene untouched, when nothing is selected, a selected entity has no editable mesh or no
    // transform, Edit Pivot / Bake / Merge meet an entity with children (their world transforms would move
    // with it), Merge has fewer than two entities, or the geometry refuses (named).
    [[nodiscard]] Common::BoolResultStr ApplyXformOperation( ::Desert::Core::Scene& scene,
                                                             XformOperation operation, const XformArgs& args );
} // namespace Desert::Editor::Core
