// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Operations/
// PolyEditingUVUtil.cpp:11-50, adapted: see the header; the 2D triangle area is written out.
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/PolyEditingUVUtil.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/Parameterization/DynamicMeshUVEditor.hpp"
#include "Engine/Geometry/MeshCore/VectorUtil.hpp"

#include <cmath>

namespace Desert::Geometry
{
    bool ComputeArbitraryTrianglePatchUVs( DynamicMesh3& Mesh, DynamicMeshUVOverlay& UVOverlay,
                                           const std::vector<int32_t>& TriangleSet )
    {
        std::vector<int32_t> NbrTriSet;
        double        NbrUVAreaSum = 0.0;
        double        Nbr3DAreaSum = 0.0;
        for ( const int32_t tid : TriangleSet )
        {
            const Index3i NbrTris = Mesh.GetTriNeighbourTris( tid );
            for ( int32_t j = 0; j < 3; ++j )
            {
                if ( NbrTris[j] == DynamicMesh3::InvalidID ||
                     ( std::find( NbrTriSet.begin(), NbrTriSet.end(), NbrTris[j] ) != NbrTriSet.end() ) )
                    continue;
                NbrTriSet.push_back( NbrTris[j] );
                if ( !UVOverlay.IsSetTriangle( NbrTris[j] ) )
                    continue;
                glm::dvec3 A{};
                glm::dvec3 B{};
                glm::dvec3 C{};
                Mesh.GetTriVertices( NbrTris[j], A, B, C );
                Nbr3DAreaSum += VectorUtil::Area( A, B, C );
                glm::vec2 U{};
                glm::vec2 V{};
                glm::vec2 W{};
                UVOverlay.GetTriElements( NbrTris[j], U, V, W );
                NbrUVAreaSum += 0.5 * std::abs( static_cast<double>( V.x - U.x ) * ( W.y - U.y ) -
                                                static_cast<double>( V.y - U.y ) * ( W.x - U.x ) );
            }
        }
        const double UseUVScale = std::max<double>( std::sqrt( NbrUVAreaSum ), 0.0001 ) /
                                  std::max<double>( std::sqrt( Nbr3DAreaSum ), 0.0001 );

        DynamicMeshUVEditor  UVEditor( &Mesh, &UVOverlay );
        UVEditResult         UVEditResult;
        const bool           bOK = UVEditor.SetTriangleUVsFromExpMap( TriangleSet, &UVEditResult );
        UVEditor.TransformUVElements( UVEditResult.NewUVElements, [UseUVScale]( const glm::vec2& UV )
                                      { return UV * static_cast<float>( UseUVScale ); } );
        return bOK;
    }
} // namespace Desert::Geometry
