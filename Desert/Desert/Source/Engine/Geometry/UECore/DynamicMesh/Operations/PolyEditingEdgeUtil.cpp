// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Operations/
// PolyEditingEdgeUtil.cpp:11-94, adapted: namespace Desert::Geometry, behaviour unchanged.
#include "Engine/Geometry/UECore/DynamicMesh/Operations/PolyEditingEdgeUtil.hpp"

#include "Engine/Geometry/UECore/Distance/DistLine3Line3.hpp"

#include <cmath>

namespace Desert::Geometry
{
    void ComputeInsetLineSegmentsFromEdges( const DynamicMesh3& Mesh, const std::vector<int32_t>& EdgeList,
                                            double InsetDistance, std::vector<Line3d>& InsetLinesOut )
    {
        const int32_t NumEdges = static_cast<int32_t>( EdgeList.size() );
        InsetLinesOut.resize( NumEdges );
        for ( int32_t k = 0; k < NumEdges; ++k )
        {
            if ( Mesh.IsEdge( EdgeList[k] ) )
            {
                const DynamicMesh3::Edge   EdgeVT   = Mesh.GetEdge( EdgeList[k] );
                const glm::dvec3           A        = Mesh.GetVertex( EdgeVT.Vert.A );
                const glm::dvec3           B        = Mesh.GetVertex( EdgeVT.Vert.B );
                const glm::dvec3           EdgeDir  = Normalized( A - B );
                const glm::dvec3           Midpoint = ( A + B ) * 0.5;
                glm::dvec3                 Normal{};
                glm::dvec3                 Centroid{};
                double                     Area = 0.0;
                Mesh.GetTriInfo( EdgeVT.Tri.A, Normal, Area, Centroid );

                glm::dvec3 InsetDir = glm::cross( Normal, EdgeDir );
                if ( glm::dot( ( Centroid - Midpoint ), InsetDir ) < 0 )
                {
                    InsetDir = InsetDir * -1.0;
                }
                InsetLinesOut[k] = Line3d( Midpoint + InsetDistance * InsetDir, EdgeDir );
            }
            else
            {
                // UE keeps a default line here too: the caller passed an edge that no longer exists.
                InsetLinesOut[k] = Line3d();
            }
        }
    }

    glm::dvec3 SolveInsetVertexPositionFromLinePair( const glm::dvec3& Position, const Line3d& InsetEdgeLine1,
                                                     const Line3d& InsetEdgeLine2 )
    {
        if ( std::abs( glm::dot( InsetEdgeLine1.Direction, InsetEdgeLine2.Direction ) ) > 0.999 )
        {
            // The lines are parallel, so their intersection is not useful: use the nearest point on either line.
            return InsetEdgeLine1.NearestPoint( Position );
        }
        // Inset the point to the intersection point of the two lines.
        DistLine3Line3d Distance( InsetEdgeLine1, InsetEdgeLine2 );
        Distance.GetSquared();
        return 0.5 * ( Distance.m_Line1ClosestPoint + Distance.m_Line2ClosestPoint );
    }

    void SolveInsetVertexPositionsFromInsetLines( const DynamicMesh3&         Mesh,
                                                  const std::vector<Line3d>&  InsetEdgeLines,
                                                  const std::vector<int32_t>& VertexIDs,
                                                  std::vector<glm::dvec3>& VertexPositionsOut, bool bIsLoop )
    {
        const int32_t NumVertices = static_cast<int32_t>( VertexIDs.size() );
        VertexPositionsOut.resize( NumVertices );

        int32_t StartIndex = 0;
        int32_t EndIndex   = NumVertices;

        // An open vertex span has no two lines to intersect at its start/end, so those use the nearest points.
        if ( !bIsLoop )
        {
            StartIndex = 1;
            EndIndex   = NumVertices - 1;

            VertexPositionsOut[0]     = InsetEdgeLines[0].NearestPoint( Mesh.GetVertex( VertexIDs[0] ) );
            VertexPositionsOut.back() = InsetEdgeLines.back().NearestPoint( Mesh.GetVertex( VertexIDs.back() ) );
        }

        for ( int32_t vi = StartIndex; vi < EndIndex; ++vi )
        {
            const Line3d& PrevLine = ( vi == 0 ) ? InsetEdgeLines.back() : InsetEdgeLines[vi - 1];
            const Line3d& NextLine = InsetEdgeLines[vi];
            VertexPositionsOut[vi] =
                 SolveInsetVertexPositionFromLinePair( Mesh.GetVertex( VertexIDs[vi] ), PrevLine, NextLine );
        }
    }
} // namespace Desert::Geometry
