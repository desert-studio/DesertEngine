#include "DynamicMeshSelection.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace Desert::Geometry
{
    // The ported core under the names ElementSelectionAlgorithms.inl reads. Polygroups come from the
    // FGroupTopology when the view carries one (picking and the operations), from the triangle groups otherwise
    // (Exists / KeyOf, which only ask whether a live triangle carries the group).
    class FDynamicMeshElements
    {
    public:
        explicit FDynamicMeshElements( const FDynamicMesh3& mesh, const FGroupTopology* topology = nullptr )
             : m_Mesh( mesh ), m_Topology( topology )
        {
        }

        [[nodiscard]] std::vector<int> VertexIds() const
        {
            return Collect( m_Mesh.VertexIndicesItr() );
        }
        [[nodiscard]] std::vector<int> EdgeIds() const
        {
            return Collect( m_Mesh.EdgeIndicesItr() );
        }
        [[nodiscard]] std::vector<int> TriangleIds() const
        {
            return Collect( m_Mesh.TriangleIndicesItr() );
        }
        [[nodiscard]] bool IsVertex( int v ) const
        {
            return m_Mesh.IsVertex( v );
        }
        [[nodiscard]] bool IsEdge( int e ) const
        {
            return m_Mesh.IsEdge( e );
        }
        [[nodiscard]] bool IsTriangle( int t ) const
        {
            return m_Mesh.IsTriangle( t );
        }
        [[nodiscard]] int MaxVertexId() const
        {
            return m_Mesh.MaxVertexID();
        }
        [[nodiscard]] int MaxEdgeId() const
        {
            return m_Mesh.MaxEdgeID();
        }
        [[nodiscard]] int MaxTriangleId() const
        {
            return m_Mesh.MaxTriangleID();
        }
        [[nodiscard]] glm::vec3 GetPosition( int v ) const
        {
            const glm::dvec3 p = m_Mesh.GetVertex( v );
            return { static_cast<float>( p.x ), static_cast<float>( p.y ), static_cast<float>( p.z ) };
        }
        [[nodiscard]] std::array<int, 3> GetTriangle( int t ) const
        {
            const FIndex3i tri = m_Mesh.GetTriangle( t );
            return { tri.A, tri.B, tri.C };
        }
        [[nodiscard]] std::array<int, 3> GetTriangleEdges( int t ) const
        {
            const FIndex3i te = m_Mesh.GetTriEdges( t );
            return { te.A, te.B, te.C };
        }
        [[nodiscard]] std::array<int, 2> GetEdgeVertices( int e ) const
        {
            const FIndex2i ev = m_Mesh.GetEdgeV( e );
            return { ev.A, ev.B };
        }
        [[nodiscard]] std::vector<int> GetVertexNeighbours( int v ) const
        {
            std::vector<int> out;
            m_Mesh.EnumerateVertexEdges( v, [&]( int32_t e ) { out.push_back( OtherEnd( e, v ) ); } );
            return out;
        }
        [[nodiscard]] const FDynamicMeshElements& Attributes() const
        {
            return *this;
        }
        [[nodiscard]] int GetPolyGroup( int t ) const
        {
            return m_Topology ? m_Topology->GetGroupID( t ) : m_Mesh.GetTriangleGroup( t );
        }

    private:
        [[nodiscard]] int OtherEnd( int e, int v ) const
        {
            const FIndex2i ev = m_Mesh.GetEdgeV( e );
            return ev.A == v ? ev.B : ev.A;
        }

        template <class Range>
        static std::vector<int> Collect( Range range )
        {
            std::vector<int> out;
            for ( const int id : range )
                out.push_back( id );
            return out;
        }

        const FDynamicMesh3&  m_Mesh;
        const FGroupTopology* m_Topology;
    };
} // namespace Desert::Geometry

#include "ElementSelectionAlgorithms.inl"

namespace Desert::Geometry
{
    // Ported from UE 5.8 Engine/Plugins/Runtime/MeshModelingToolset/Source/ModelingComponents/Private/Selection/
    // MeshTopologySelector.cpp:87-206 (FindSelectedElement), :208-260 (DoCornerBasedSelection), :374-420
    // (DoEdgeBasedSelection), :578-601 (IsOccluded), and Engine/Source/Runtime/GeometryCore/Private/Spatial/
    // GeometrySet3.cpp:95-240 (FindNearestPointToRay, FindNearestCurveToRay), adapted: one element kind per call
    // (our ElementMode; the corner/edge/face resolve tolerances of :150-178 only arbitrate between kinds, so they
    // have nothing to arbitrate), corners and group edges of a per-triangle topology are the mesh's own vertices
    // and edges (UE's FTriangleGroupTopology), a linear scan in place of FGeometrySet3 / FDynamicMeshAABBTree3,
    // the visual-angle PointSnapQuery replaced by the viewport-pixel tolerance of PickView, back faces hit and
    // occlude (the mechanic's bHitBackFaces default), an occluded edge skipped rather than failing the pick
    // (PickNearestEdge says why), and the occlusion ray bounded at the eye with the relative
    // slack of Visible() instead of UE's absolute 100 * ZeroTolerance, which is below float resolution at
    // centimetre scale.
    namespace
    {
        // Ray.ClosestPoint(A) against B, measured in viewport pixels (PointsWithinToleranceTest).
        bool WithinTolerance( const PickView& view, const glm::vec3& rayPoint, const glm::vec3& geoPoint,
                              float& outPixels )
        {
            glm::vec2 a{};
            glm::vec2 b{};
            if ( !ProjectToViewport( rayPoint, view.ViewProj, view.ViewportPos, view.ViewportSize, a ) ||
                 !ProjectToViewport( geoPoint, view.ViewProj, view.ViewportPos, view.ViewportSize, b ) )
                return false;
            outPixels = glm::length( a - b );
            return outPixels <= view.TolerancePixels;
        }

        // IsOccluded: a ray from the point back to the eye crossing any triangle before it.
        bool Occluded( const FDynamicMeshElements& mesh, const PickView& view, const glm::vec3& point )
        {
            const glm::vec3 toEye = view.RayOrigin - point;
            const float     dist  = glm::length( toEye );
            if ( dist <= 0.0f )
                return false;
            const glm::vec3 dir    = toEye / dist;
            const float     slack  = 1e-4f * dist + 1e-2f;
            const glm::vec3 origin = point + slack * dir;
            for ( const int t : mesh.TriangleIds() )
            {
                const auto& tri = mesh.GetTriangle( t );
                float       hitT;
                if ( RayTriangle( origin, dir, WorldPosition( mesh, view, tri[0] ),
                                  WorldPosition( mesh, view, tri[1] ), WorldPosition( mesh, view, tri[2] ),
                                  hitT ) &&
                     hitT < dist - slack )
                    return true;
            }
            return false;
        }

        // FDistRay3Segment3::SquaredDistance: the closest points of the ray (t >= 0) and the segment.
        void RaySegmentClosest( const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b,
                                float& outRayT, glm::vec3& outSegPoint )
        {
            const glm::vec3 ab  = b - a;
            const float     len = glm::dot( ab, ab );
            float           s   = 0.0f;
            if ( len > 0.0f )
            {
                const glm::vec3 w     = a - o;
                const float     bd    = glm::dot( ab, d );
                const float     denom = len - bd * bd;
                s = denom > 1e-12f ? ( bd * glm::dot( w, d ) - glm::dot( w, ab ) ) / denom : 0.0f;
                s = std::clamp( s, 0.0f, 1.0f );
            }
            float t = glm::dot( a + s * ab - o, d );
            if ( t < 0.0f )
            {
                t = 0.0f;
                s = len > 0.0f ? std::clamp( glm::dot( o - a, ab ) / len, 0.0f, 1.0f ) : 0.0f;
            }
            outRayT     = t;
            outSegPoint = a + s * ab;
        }

        // DoCornerBasedSelection over FindNearestPointToRay: of the candidate vertices within tolerance, the one
        // NEAREST ALONG THE RAY; a miss when that one is occluded (no fallback to the next, as in UE).
        ElementHit PickNearestVertex( const FDynamicMeshElements& mesh, const PickView& view,
                                      const std::vector<int>& candidates )
        {
            ElementHit best;
            glm::vec3  bestPoint{ 0.0f };
            float      bestT = kNoHit;
            for ( const int v : candidates )
            {
                const glm::vec3 p = WorldPosition( mesh, view, v );
                const float     t = glm::dot( p - view.RayOrigin, view.RayDirection );
                if ( t < 0.0f || t >= bestT )
                    continue;
                float pixels;
                if ( !WithinTolerance( view, view.RayOrigin + t * view.RayDirection, p, pixels ) )
                    continue;
                bestT     = t;
                best      = { v, t, pixels };
                bestPoint = p;
            }
            if ( best.IsHit() && Occluded( mesh, view, bestPoint ) )
                return {};
            return best;
        }

        // DoEdgeBasedSelection over FindNearestCurveToRay: of the candidate mesh edges within tolerance, the one
        // whose |Area(RayOrigin, CurvePosition, RayPosition)| is least - closeness to the ray balanced against
        // closeness to the eye - among the VISIBLE ones. UE takes the least metric and then bails if it is
        // occluded; but an edge lying exactly behind the picked one (a face diagonal seen head-on has the
        // opposite face's diagonal on the same ray) has the same metric up to float noise, so the hidden one
        // could win on noise and the pick missed. Occluded candidates are skipped instead, which keeps UE's
        // "never select what is hidden" and makes the tie deterministic. A polyline's nearest point is its
        // nearest segment's, so scanning a group edge's segments is FindNearestCurveToRay over that curve.
        ElementHit PickNearestEdge( const FDynamicMeshElements& mesh, const PickView& view,
                                    const std::vector<int>& candidates )
        {
            ElementHit best;
            float      bestMetric = kNoHit;
            for ( const int e : candidates )
            {
                const auto ev = mesh.GetEdgeVertices( e );
                float      rayT;
                glm::vec3  curvePoint{};
                RaySegmentClosest( view.RayOrigin, view.RayDirection, WorldPosition( mesh, view, ev[0] ),
                                   WorldPosition( mesh, view, ev[1] ), rayT, curvePoint );
                const glm::vec3 rayPoint = view.RayOrigin + rayT * view.RayDirection;
                float           pixels;
                if ( !WithinTolerance( view, rayPoint, curvePoint, pixels ) )
                    continue;
                const float metric =
                     0.5f * glm::length( glm::cross( curvePoint - view.RayOrigin, rayPoint - view.RayOrigin ) );
                if ( metric < bestMetric && !Occluded( mesh, view, curvePoint ) )
                {
                    bestMetric = metric;
                    best       = { e, rayT, pixels };
                }
            }
            return best;
        }

        // GetGeometrySet's points: every corner's vertex (UE's FMeshTopologySelector picks corners, never the
        // vertices inside a group edge or a group).
        std::vector<int> CornerVertices( const FGroupTopology& topology )
        {
            std::vector<int> out;
            for ( const auto& corner : topology.Corners )
                out.push_back( corner.VertexID );
            return out;
        }

        // GetGeometrySet's curves: the mesh edges of every group edge (GetGroupEdgeEdges). A diagonal inside a
        // group belongs to no group edge, so it cannot be picked.
        std::vector<int> GroupEdgeSegments( const FGroupTopology& topology )
        {
            std::vector<int> out;
            for ( int g = 0; g < static_cast<int32_t>( topology.Edges.size() ); ++g )
                for ( const int e : topology.GetGroupEdgeEdges( g ) )
                    out.push_back( e );
            return out;
        }
    } // namespace

    const char* ToString( TopologyLevel level )
    {
        switch ( level )
        {
            case TopologyLevel::Group:
                return "Group";
            case TopologyLevel::Triangle:
                return "Triangle";
        }
        return "?";
    }

    ElementHit PickElement( const FDynamicMesh3& mesh, const FGroupTopology& topology, ElementMode mode,
                            const PickView& view, TopologyLevel level )
    {
        const FDynamicMeshElements elements( mesh, &topology );
        const bool                 groups = level == TopologyLevel::Group;
        switch ( mode )
        {
            case ElementMode::Vertex:
                return PickNearestVertex( elements, view,
                                          groups ? CornerVertices( topology ) : elements.VertexIds() );
            case ElementMode::Edge:
                return PickNearestEdge( elements, view,
                                        groups ? GroupEdgeSegments( topology ) : elements.EdgeIds() );
            case ElementMode::Triangle:
            case ElementMode::PolyGroup:
            {
                // The face hit: FindNearestHitTriangle, the group through the topology.
                const auto [triangle, rayT] = RayCast( elements, view );
                if ( triangle == InvalidId )
                    return {};
                return { mode == ElementMode::Triangle ? triangle : topology.GetGroupID( triangle ), rayT, 0.0f };
            }
        }
        return {};
    }

    std::vector<int> HitElements( const FGroupTopology& topology, ElementMode mode, TopologyLevel level,
                                  const ElementHit& hit )
    {
        if ( !hit.IsHit() )
            return {};
        if ( mode != ElementMode::Edge || level != TopologyLevel::Group )
            return { hit.Id };
        // GetGroupEdgeEdges( FindGroupEdgeID( segment ) ): the whole group edge the picked segment lies on.
        // The segment came from GroupEdgeSegments, so it always has one.
        std::vector<int> out;
        for ( const int e : topology.GetGroupEdgeEdges( topology.FindGroupEdgeID( hit.Id ) ) )
            out.push_back( e );
        return out;
    }

    ElementSelection ConvertSelection( const FDynamicMesh3& mesh, const FGroupTopology& topology,
                                       const ElementSelection& selection, ElementMode target )
    {
        return ConvertSelectionT( FDynamicMeshElements( mesh, &topology ), selection, target );
    }

    ElementSelection SelectConnected( const FDynamicMesh3& mesh, const FGroupTopology& topology,
                                      const ElementSelection& selection )
    {
        return SelectConnectedT( FDynamicMeshElements( mesh, &topology ), selection );
    }

    ElementSelection GrowSelection( const FDynamicMesh3& mesh, const FGroupTopology& topology,
                                    const ElementSelection& selection )
    {
        return GrowSelectionT( FDynamicMeshElements( mesh, &topology ), selection );
    }

    ElementSelection ShrinkSelection( const FDynamicMesh3& mesh, const FGroupTopology& topology,
                                      const ElementSelection& selection )
    {
        return ShrinkSelectionT( FDynamicMeshElements( mesh, &topology ), selection );
    }

    ElementSelection InvertSelection( const FDynamicMesh3& mesh, const FGroupTopology& topology,
                                      const ElementSelection& selection )
    {
        return InvertSelectionT( FDynamicMeshElements( mesh, &topology ), selection );
    }

    Common::BoolResultStr ElementSelection::Add( const FDynamicMesh3& mesh, int id )
    {
        return AddIn( FDynamicMeshElements( mesh ), id );
    }

    Common::BoolResultStr ElementSelection::Toggle( const FDynamicMesh3& mesh, int id )
    {
        return ToggleIn( FDynamicMeshElements( mesh ), id );
    }

    PruneReport ElementSelection::Prune( const FDynamicMesh3& mesh )
    {
        return PruneIn( FDynamicMeshElements( mesh ) );
    }
} // namespace Desert::Geometry
