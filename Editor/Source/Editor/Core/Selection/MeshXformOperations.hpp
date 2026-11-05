#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <Engine/Geometry/DynamicMeshXformOperations.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

    // Merge's inputs (UE UCombineMeshesTool's targets), gathered on the main thread from the scene selection: the
    // first selected entity receives the result, every selected mesh is carried into its object space and its
    // material IDs remapped into one slot table (the receiver's slots first). One source for the Merge tool
    // session's preview (computed off the main thread from this snapshot) and for ApplyXformOperation's Merge.
    struct MergeInputs
    {
        struct Part
        {
            std::shared_ptr<const Geometry::DynamicMesh3> Mesh;   // the entity's tool-target mesh
            glm::mat4                                     ToInto; // its object space -> the receiver's
            std::vector<int>                              MaterialRemap;
        };
        std::vector<Common::UUID>        Entities; // the selection, in order; [0] receives the result
        std::string                      IntoName;
        std::vector<Part>                Parts;    // one per entity, same order
        std::vector<Common::AssetHandle> MaterialSlots;
    };

    // Refused as ApplyXformOperation's Merge is: fewer than two entities, one without an editable mesh or a
    // transform, or one with children.
    [[nodiscard]] Common::ResultStr<MergeInputs> GatherMergeInputs( ::Desert::Core::Scene& scene );
    // The merged mesh in the receiver's object space; reads only @p inputs (safe on a worker thread).
    [[nodiscard]] Common::ResultStr<Geometry::MergeOutcome> MergeGathered( const MergeInputs& inputs );

    // Refused, with the scene untouched, when nothing is selected, a selected entity has no editable mesh or no
    // transform, Edit Pivot / Bake / Merge meet an entity with children (their world transforms would move
    // with it), Merge has fewer than two entities, or the geometry refuses (named).
    [[nodiscard]] Common::BoolResultStr ApplyXformOperation( ::Desert::Core::Scene& scene,
                                                             XformOperation operation, const XformArgs& args );
} // namespace Desert::Editor::Core
