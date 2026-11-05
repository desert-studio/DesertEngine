#pragma once

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <memory>
#include <string>
#include <vector>

#include <cstdint>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Geometry
{
    class DynamicMesh3;
    struct DeformOutcome;
}

namespace Desert::Editor::Core
{
    // UE's Boolean and Trim tools (UCSGMeshesTool, and the same tool in trim mode; MeshModelingTools
    // CSGMeshesTool.h) on the TWO entities of the scene selection, A the first selected and B the second. As in UE
    // each is a TOOL SESSION (MeshProcessingTool, ModelingState::Tool::Boolean / Trim): RunBooleanTool computes
    // the result in the background on copies, A draws it as the preview while B is hidden, a change of the
    // operation recomputes it; Accept writes it with CommitBooleanTool as ONE undo step; Cancel leaves both
    // entities as they were.

    enum class BooleanTool : uint8_t
    {
        Boolean, // UE's Boolean: a closed solid out of two closed solids
        Trim,    // UE's Trim: one mesh (open or closed) cut by the other's closed surface, the cut left open
    };

    // UE's ECSGOperation.
    enum class CsgOperation : uint8_t
    {
        DifferenceAB, // A minus B
        DifferenceBA, // B minus A
        Intersect,
        Union,
    };

    // UE's ETrimOperation: which of the two is trimmed; the other is the cutter.
    enum class TrimTarget : uint8_t
    {
        TrimA,
        TrimB,
    };

    // UE's ETrimSide.
    enum class TrimSide : uint8_t
    {
        RemoveInside,  // the part of the trimmed mesh inside the cutter goes
        RemoveOutside, // only the part inside the cutter stays
    };

    // UE's UBaseCreateFromSelectedHandleSourceProperties::OutputWriteTo.
    enum class BooleanWriteTo : uint8_t
    {
        NewObject, // a copy of the operated entity (not its children) takes the result
        Input,     // the operated entity (A, or B for Difference B - A and Trim B) takes it in place
    };

    // UE's EHandleSourcesMethod: what happens to the inputs the result was not written into.
    enum class BooleanInputs : uint8_t
    {
        Delete,
        Hide,
        Keep,
    };

    [[nodiscard]] const char* ToString( BooleanTool tool );
    [[nodiscard]] const char* ToString( CsgOperation operation );
    [[nodiscard]] const char* ToString( TrimTarget target );
    [[nodiscard]] const char* ToString( TrimSide side );
    [[nodiscard]] const char* ToString( BooleanWriteTo writeTo );
    [[nodiscard]] const char* ToString( BooleanInputs inputs );

    struct BooleanToolArgs
    {
        CsgOperation   Operation = CsgOperation::DifferenceAB;
        TrimTarget     Trimmed   = TrimTarget::TrimA;
        TrimSide       Side      = TrimSide::RemoveInside;
        BooleanWriteTo WriteTo   = BooleanWriteTo::NewObject;
        BooleanInputs  Inputs    = BooleanInputs::Delete;
    };

    // UE UBaseCreateFromSelectedTool's two inputs, for an operator that runs on them in the background
    // (Boolean, Trim, VoxBoolean, VoxBlend: MeshProcessingTool): A and B of the scene selection,
    // A's tool target mesh and B's baked into A's object space.
    struct SelectedMeshPair
    {
        Common::UUID                                  A, B;
        std::string                                   NameA, NameB;
        std::shared_ptr<const Geometry::DynamicMesh3> MeshA, MeshBInA;
        // What each component holds now (ToolTargetMesh::Committed): the "before" a hidden input keeps.
        std::shared_ptr<const Geometry::DynamicMesh3> CommittedA, CommittedB;
        // With @p remapMaterials: A's material slots, then B's not already among them; MeshBInA's material IDs
        // index this table (a Boolean keeps B's triangles, so the result's slots are the table).
        std::vector<Common::AssetHandle> MaterialSlots;
    };
    // Refused, with the scene untouched: a selection of other than two entities; an entity without an editable
    // mesh or a transform, or with children (the result replaces the mesh, the children would be left out of it).
    [[nodiscard]] Common::ResultStr<SelectedMeshPair>
    SelectedPairInSpaceOfA( ::Desert::Core::Scene& scene, const char* toolName, bool remapMaterials );

    // The operator of the Boolean / Trim session, on copies (a JobSystem worker runs it): MeshBoolean
    // (Engine/Geometry/MeshBooleanOperation.hpp) in A's object space, the operated input (B for Difference
    // B - A and Trim B) as the target and the other as the cutter. Refused with every refusal of RunMeshBoolean,
    // named with its numbers (an open mesh in a Boolean, an open cutter, a seam left open, an empty result, a
    // cutter that does not cross the trimmed mesh).
    [[nodiscard]] Common::ResultStr<Geometry::DeformOutcome>
    RunBooleanTool( BooleanTool tool, const BooleanToolArgs& args, const SelectedMeshPair& inputs );
    // The session's Accept: @p resultInA (RunBooleanTool's mesh, in A's object space) is written to the operated
    // input (Write To Input; carried into B's space when B is operated) or to a copy of it (New Object), the
    // inputs not written into are deleted / hidden / kept - all ONE undo step. Refused with a cutter transform
    // that flattens the mesh or an input that left the scene. The result is selected; returns the entities the
    // result is on.
    [[nodiscard]] Common::ResultStr<std::vector<Common::UUID>>
    CommitBooleanTool( ::Desert::Core::Scene& scene, BooleanTool tool, const BooleanToolArgs& args,
                       const SelectedMeshPair&                              inputs,
                       const std::shared_ptr<const Geometry::DynamicMesh3>& resultInA );
} // namespace Desert::Editor::Core
