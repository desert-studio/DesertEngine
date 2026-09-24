#include "DynamicMeshSelection.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp"

#include <array>
#include <unordered_map>
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
            const FVector3d p = m_Mesh.GetVertex( v );
            return { static_cast<float>( p.X ), static_cast<float>( p.Y ), static_cast<float>( p.Z ) };
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
            m_Mesh.EnumerateVertexEdges( v, [&]( int32 e ) { out.push_back( OtherEnd( e, v ) ); } );
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
        [[nodiscard]] const FGroupTopology* Topology() const
        {
            return m_Topology;
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

    // The group -> triangles table the algorithms ask for: FGroupTopology's own when the view carries one. Found
    // by argument-dependent lookup from the template and preferred to it as the exact (non-template) match.
    std::unordered_map<int, std::vector<int>> TrianglesByGroup( const FDynamicMeshElements& mesh );
} // namespace Desert::Geometry

#include "ElementSelectionAlgorithms.inl"

namespace Desert::Geometry
{
    std::unordered_map<int, std::vector<int>> TrianglesByGroup( const FDynamicMeshElements& mesh )
    {
        std::unordered_map<int, std::vector<int>> groups;
        if ( const FGroupTopology* topology = mesh.Topology() )
        {
            for ( const FGroupTopology::FGroup& group : topology->Groups )
                groups[group.GroupID].assign( group.Triangles.begin(), group.Triangles.end() );
            return groups;
        }
        for ( const int t : mesh.TriangleIds() )
            groups[mesh.GetPolyGroup( t )].push_back( t );
        return groups;
    }

    ElementHit PickElement( const FDynamicMesh3& mesh, const FGroupTopology& topology, ElementMode mode,
                            const PickView& view )
    {
        return PickElementT( FDynamicMeshElements( mesh, &topology ), mode, view );
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
