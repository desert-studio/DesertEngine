#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::Core
{
    // UE's Boolean and Trim tools (UCSGMeshesTool, and the same tool in trim mode; MeshModelingTools
    // CSGMeshesTool.h) on the TWO entities of the scene selection, A the first selected and B the second. Each
    // runs MeshBoolean (Engine/Geometry/MeshBooleanOperation.hpp) on copies, the other entity's mesh baked into
    // the operated one's space, and writes the result as one undo step. In UE each is a tool with a live
    // preview and an Accept; here each is an operation that runs on one click.

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

    [[nodiscard]] BooleanToolArgs BooleanArgsFromModelingState();

    // Refused, with the scene untouched: a selection of other than two entities; an entity without an editable
    // mesh or a transform, or with children (the result replaces the mesh, the children would be left out of
    // it); a cutter transform that flattens the mesh; and every refusal of RunMeshBoolean, named with its
    // numbers (an open mesh in a Boolean, an open cutter, a seam left open, an empty result, a cutter that
    // does not cross the trimmed mesh). The result is selected.
    [[nodiscard]] Common::BoolResultStr ApplyBooleanTool( ::Desert::Core::Scene& scene, BooleanTool tool,
                                                          const BooleanToolArgs& args );
} // namespace Desert::Editor::Core
