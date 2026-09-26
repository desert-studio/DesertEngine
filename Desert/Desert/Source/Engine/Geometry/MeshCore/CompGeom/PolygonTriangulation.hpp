// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/CompGeom/PolygonTriangulation.h:29-47, adapted:
// namespace Desert::Geometry::PolygonTriangulation; only the 3D ear clip and its plane fit (MeshBevel's
// junction polygons need them). The 2D TriangulateSimplePolygon and the constrained Delaunay path are not ported.
#pragma once

#include "Engine/Geometry/MeshCore/IndexTypes.hpp"
#include "Engine/Geometry/MeshCore/VectorTypes.hpp"

#include <vector>

namespace Desert::Geometry::PolygonTriangulation
{
    /**
     * Compute an approximate plane for a closed 3D polygon (Newell's method).
     * @param PlaneNormalOut unit normal of the plane, oriented by the polygon's winding
     * @param PlanePointOut the average of the vertices, a point on the plane
     * @return half the length of the unnormalized Newell normal, i.e. the projected area of the polygon
     */
    template <typename RealType>
    RealType ComputePolygonPlane( const std::vector<glm::vec<3, RealType>>& VertexPositions,
                                  glm::vec<3, RealType>& PlaneNormalOut, glm::vec<3, RealType>& PlanePointOut );

    /**
     * Ear-clip a simple closed 3D polygon, testing ears against the plane from ComputePolygonPlane.
     * @param OutTriangles indices into VertexPositions
     * @param bOrientAsHoleFill when true (UE's default) every triangle is wound opposite to the polygon, the way a
     *        hole-fill of a boundary loop must be
     */
    template <typename RealType>
    void TriangulateSimplePolygon( const std::vector<glm::vec<3, RealType>>& VertexPositions,
                                   std::vector<Index3i>& OutTriangles, bool bOrientAsHoleFill = true );
} // namespace Desert::Geometry::PolygonTriangulation
