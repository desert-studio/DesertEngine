#include "EditMeshPolyGroups.hpp"

#include "EditMeshNormals.hpp"

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>
#include <vector>

namespace Desert::Geometry
{
    namespace
    {
        // Flood fill over edge-adjacent triangles; joins(seed, from, to, edge) decides each crossing.
        template <typename Joins>
        int Flood( EditMesh& mesh, Joins&& joins )
        {
            EditMeshAttributes& attributes = mesh.Attributes();
            std::vector<int>    group( static_cast<size_t>( mesh.MaxTriangleId() ), InvalidId );
            std::vector<int>    stack;
            int                 groups = 0;
            for ( const int seed : mesh.TriangleIds() )
            {
                if ( group[seed] != InvalidId )
                    continue;
                group[seed] = groups;
                stack.assign( 1, seed );
                while ( !stack.empty() )
                {
                    const int t = stack.back();
                    stack.pop_back();
                    for ( const int e : mesh.GetTriangleEdges( t ) )
                    {
                        const auto& tris  = mesh.GetEdgeTriangles( e );
                        const int   other = tris[0] == t ? tris[1] : tris[0];
                        if ( other == InvalidId || group[other] != InvalidId || !joins( seed, t, other, e ) )
                            continue;
                        group[other] = groups;
                        stack.push_back( other );
                    }
                }
                ++groups;
            }
            for ( const int t : mesh.TriangleIds() )
                attributes.SetPolyGroup( t, group[t] );
            return groups;
        }
    } // namespace

    int GeneratePolyGroupsByAngle( EditMesh& mesh, float maxAngleDegrees )
    {
        const float minDot = std::cos( glm::radians( maxAngleDegrees ) );
        return Flood( mesh, [&]( int, int from, int to, int )
                      { return glm::dot( TriangleNormal( mesh, from ), TriangleNormal( mesh, to ) ) >= minDot; } );
    }

    int GeneratePolyGroupsCoplanar( EditMesh& mesh, float angleToleranceDegrees, float distanceTolerance )
    {
        const float minDot = std::cos( glm::radians( angleToleranceDegrees ) );
        return Flood( mesh,
                      [&]( int seed, int, int to, int )
                      {
                          const glm::vec3 n = TriangleNormal( mesh, seed );
                          if ( glm::dot( n, TriangleNormal( mesh, to ) ) < minDot )
                              return false;
                          const glm::vec3 origin = mesh.GetPosition( mesh.GetTriangle( seed )[0] );
                          for ( const int v : mesh.GetTriangle( to ) )
                              if ( std::abs( glm::dot( mesh.GetPosition( v ) - origin, n ) ) > distanceTolerance )
                                  return false;
                          return true;
                      } );
    }

    Common::ResultStr<int> GeneratePolyGroupsByUVIslands( EditMesh& mesh, int uvLayer )
    {
        const UVOverlay* uvs = mesh.Attributes().UV( uvLayer );
        if ( uvs == nullptr )
            return Common::MakeFormattedError<int>(
                 "GeneratePolyGroupsByUVIslands: UV layer {} requested, the mesh "
                 "has {}",
                 uvLayer, mesh.Attributes().UVLayerCount() );
        return Common::MakeSuccess(
             Flood( mesh, [&]( int, int, int, int e ) { return !uvs->IsSeamEdge( mesh, e ); } ) );
    }
} // namespace Desert::Geometry
