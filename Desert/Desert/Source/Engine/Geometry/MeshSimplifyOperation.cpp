#include "MeshSimplifyOperation.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshSimplification.hpp"

namespace Desert::Geometry
{
    void SimplifyToTriangleCount( DynamicMesh3& mesh, int targetTriangles )
    {
        QemSimplification simplifier( mesh );
        simplifier.SimplifyToTriangleCount( targetTriangles );
        if ( mesh.HasAttributes() && mesh.Attributes()->PrimaryNormals() != nullptr )
        {
            MeshNormals normals( &mesh );
            normals.RecomputeOverlayNormals( mesh.Attributes()->PrimaryNormals() );
            normals.CopyToOverlay( mesh.Attributes()->PrimaryNormals() );
        }
    }
} // namespace Desert::Geometry
