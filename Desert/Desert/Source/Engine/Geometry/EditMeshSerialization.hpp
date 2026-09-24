#pragma once

#include "EditMesh.hpp"
#include "SavedMeshForm.hpp"

#include <Common/Core/ResultStr.hpp>

namespace Desert::Geometry
{
    // Total: every live element of every enabled layer is written.
    [[nodiscard]] EditMeshSer ToSerialized( const EditMesh& source );

    // Rebuilds the mesh and verifies it with EditMesh::CheckValidity. Refused, naming the array and the
    // numbers, on anything a writer of this form could not have produced: a length that is not a whole
    // number of records, an index out of range, a triangle EditMesh refuses (degenerate, duplicate,
    // non-manifold), a triangle that is only partly set in a layer, or an element no triangle uses.
    [[nodiscard]] Common::ResultStr<EditMesh> FromSerialized( const EditMeshSer& saved );
} // namespace Desert::Geometry
