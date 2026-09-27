// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/PlanarHoleFiller.h:23-72 and
// Private/Operations/PlanarHoleFiller.cpp:9-68, with HoleFillUtil.h:31-107,138-159 (FillOverlayElements,
// FillColorOverlay for vertex loops), adapted: namespace Desert::Geometry, UE Core as std/glm. The caller-provided
// 2D triangulation function (UE passes ConstrainedDelaunayTriangulate over an FPlanarComplex nest) is replaced by
// the ported 3D ear clip, one loop at a time: a loop lying inside another (a section with a hole - a tube cut
// across) is NOT filled but refused, naming the loops - a limitation of this port (v1), which P14b lifts by
// porting ConstrainedDelaunay2 and PlanarComplex.
#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/HoleFiller.hpp"

#include <string>
#include <vector>

namespace Desert::Geometry
{
    /** Fills a set of boundary loops lying in one plane with flat triangulations. */
    class PlanarHoleFiller : public IHoleFiller
    {
    public:
        DynamicMesh3*                        m_Mesh        = nullptr;
        const std::vector<std::vector<int>>* m_VertexLoops = nullptr;
        glm::dvec3                           m_PlaneOrigin{ 0.0 };
        glm::dvec3                           m_PlaneNormal{ 0.0, 0.0, 1.0 };
        /** If the mesh has a primary colour layer, the fill triangles take the colours of the loop's edges. */
        bool m_bAutoFillPrimaryColors = true;
        /** Why Fill returned false. Empty on success. */
        std::string m_FailureReason;

        PlanarHoleFiller( DynamicMesh3* MeshIn, const std::vector<std::vector<int>>* VertexLoopsIn,
                          const glm::dvec3& PlaneOriginIn, const glm::dvec3& PlaneNormalIn )
             : m_Mesh( MeshIn ), m_VertexLoops( VertexLoopsIn ), m_PlaneOrigin( PlaneOriginIn ),
               m_PlaneNormal( PlaneNormalIn )
        {
        }

        /** @return false, with m_FailureReason, when a loop nests in another, a loop does not triangulate, or a
         *  fill triangle could not join the mesh (that triangle is skipped, as UE skips it). */
        bool Fill( int GroupID ) override;
    };
} // namespace Desert::Geometry
