// UE 5.8 MeshModelingTools ProjectToTargetTool.cpp: UProjectToTargetTool is URemeshMeshTool with a projection
// target (FRemeshMeshOp::ProjectionTarget) - the second mesh, brought into the source's object space. The remesh
// itself is the one remesher (DynamicMeshRemesh.cpp, RemeshMeshOperation).
#include "MeshProjectOperation.hpp"

namespace Desert::Geometry
{
    Common::ResultStr<DeformOutcome> ProjectToTarget( const DynamicMesh3& source, const DynamicMesh3& target,
                                                      const glm::dmat4&      targetToSource,
                                                      const ProjectSettings& settings )
    {
        // UE: the target is copied into the tool mesh's frame (RemeshMeshOperation builds its AABB tree).
        DynamicMesh3 projectTo = target;
        for ( const int vid : projectTo.VertexIndicesItr() )
            projectTo.SetVertex( vid, glm::dvec3( targetToSource * glm::dvec4( projectTo.GetVertex( vid ), 1.0 ) ),
                                 false );
        return RemeshMeshOperation( source, glm::dmat4( 1.0 ), settings, &projectTo, "Project" );
    }
} // namespace Desert::Geometry
