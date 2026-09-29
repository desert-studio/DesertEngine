// Ported from UE 5.8 (see MeshSimplification.hpp and MeshConstraints.hpp for the files and lines).
#include "MeshSimplification.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/IndexUtil.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Desert::Geometry
{
    // ---- QuadricError.h ----

    QuadricErrord::QuadricErrord( const glm::dvec3& normal, const glm::dvec3& point )
         : Axx( normal.x * normal.x ), Axy( normal.x * normal.y ), Axz( normal.x * normal.z ),
           Ayy( normal.y * normal.y ), Ayz( normal.y * normal.z ), Azz( normal.z * normal.z )
    {
        const glm::dvec3 v = MultiplyA( point );
        Bx                 = -v.x;
        By                 = -v.y;
        Bz                 = -v.z;
        C                  = glm::dot( point, v );
    }

    void QuadricErrord::Add( double weight, const QuadricErrord& other )
    {
        Axx += weight * other.Axx;
        Axy += weight * other.Axy;
        Axz += weight * other.Axz;
        Ayy += weight * other.Ayy;
        Ayz += weight * other.Ayz;
        Azz += weight * other.Azz;
        Bx += weight * other.Bx;
        By += weight * other.By;
        Bz += weight * other.Bz;
        C += weight * other.C;
    }

    glm::dvec3 QuadricErrord::MultiplyA( const glm::dvec3& p ) const
    {
        return { Axx * p.x + Axy * p.y + Axz * p.z, Axy * p.x + Ayy * p.y + Ayz * p.z,
                 Axz * p.x + Ayz * p.y + Azz * p.z };
    }

    double QuadricErrord::Evaluate( const glm::dvec3& p ) const
    {
        return glm::dot( p, MultiplyA( p ) ) + 2.0 * ( p.x * Bx + p.y * By + p.z * Bz ) + C;
    }

    // SolveAxEqualsb( -b ): Cramer's rule on the symmetric 3x3 A.
    bool QuadricErrord::OptimalPoint( glm::dvec3& out, double minThresh ) const
    {
        double       a11 = Azz * Ayy - Ayz * Ayz;
        double       a12 = Axz * Ayz - Azz * Axy;
        double       a13 = Axy * Ayz - Axz * Ayy;
        double       a22 = Azz * Axx - Axz * Axz;
        double       a23 = Axy * Axz - Axx * Ayz;
        double       a33 = Axx * Ayy - Axy * Axy;
        const double det = ( Axx * a11 ) + ( Axy * a12 ) + ( Axz * a13 );
        if ( std::abs( det ) <= minThresh )
            return false;
        const double inv = 1.0 / det;
        a11 *= inv;
        a12 *= inv;
        a13 *= inv;
        a22 *= inv;
        a23 *= inv;
        a33 *= inv;
        out = { a11 * -Bx + a12 * -By + a13 * -Bz, a12 * -Bx + a22 * -By + a23 * -Bz,
                a13 * -Bx + a23 * -By + a33 * -Bz };
        return true;
    }

    // ---- MeshConstraintsUtil ----

    bool ConstrainEdgeBoundariesAndSeams( int edge, const DynamicMesh3& mesh, const BoundaryConstraintFlags& flags,
                                          EdgeConstraint& outEdge, VertexConstraint& outA, VertexConstraint& outB )
    {
        outA    = VertexConstraint::Unconstrained();
        outB    = VertexConstraint::Unconstrained();
        outEdge = EdgeConstraint{};
        if ( !mesh.IsEdge( edge ) )
            return false;
        const DynamicMeshAttributeSet* attributes     = mesh.Attributes();
        const bool                     isMeshBoundary = mesh.IsBoundaryEdge( edge );
        const bool isGroupBoundary = mesh.HasTriangleGroups() && mesh.IsGroupBoundaryEdge( edge );
        const bool isSeam          = attributes != nullptr && attributes->IsSeamEdge( edge );

        VertexConstraint current   = VertexConstraint::Unconstrained();
        EdgeRefineFlags  edgeFlags = EdgeRefineFlags::NoConstraint;
        const auto       apply     = [&]( EdgeRefineFlags boundary )
        {
            const bool canCollapse = EdgeConstraint::CanCollapse( boundary );
            const bool canFlip     = EdgeConstraint::CanFlip( boundary );
            current.CannotDelete   = current.CannotDelete || ( !canCollapse && !canFlip );
            current.CanMove        = current.CanMove && ( canCollapse || canFlip );
            edgeFlags              = edgeFlags | boundary;
        };
        if ( isMeshBoundary )
            apply( flags.MeshBoundary );
        if ( isGroupBoundary )
            apply( flags.GroupBoundary );
        if ( isSeam )
        {
            // bAllowSeamSplits = true, bAllowSeamSmoothing = bAllowSeamCollapse.
            const bool collapse  = flags.AllowSeamCollapse;
            current.CannotDelete = current.CannotDelete || !collapse;
            current.CanMove      = current.CanMove && collapse;
            edgeFlags            = edgeFlags | EdgeRefineFlags::NoFlip;
            if ( !collapse )
                edgeFlags = edgeFlags | EdgeRefineFlags::NoCollapse;
            else
            {
                // A seam's first and last edge keep the seam's ends where they are (UV and normal layers only).
                bool seamEnd = false;
                for ( int i = 0; !seamEnd && i < attributes->NumUVLayers(); ++i )
                    seamEnd = attributes->GetUVLayer( i )->IsSeamEndEdge( edge );
                for ( int i = 0; !seamEnd && i < attributes->NumNormalLayers(); ++i )
                    seamEnd = attributes->GetNormalLayer( i )->IsSeamEndEdge( edge );
                if ( seamEnd )
                    edgeFlags = edgeFlags | EdgeRefineFlags::NoCollapse;
            }
        }
        if ( edgeFlags == EdgeRefineFlags::NoConstraint && current.IsUnconstrained() )
            return false;
        outEdge.Flags = edgeFlags;
        outA          = current;
        outB          = current;
        return true;
    }

    void ConstrainAllBoundariesAndSeams( MeshConstraints& constraints, const DynamicMesh3& mesh,
                                         const BoundaryConstraintFlags& flags )
    {
        for ( const int edge : mesh.EdgeIndicesItr() )
        {
            EdgeConstraint   edgeConstraint;
            VertexConstraint a;
            VertexConstraint b;
            if ( !ConstrainEdgeBoundariesAndSeams( edge, mesh, flags, edgeConstraint, a, b ) )
                continue;
            const Index2i verts = mesh.GetEdgeV( edge );
            constraints.SetOrUpdateEdgeConstraint( edge, edgeConstraint );
            a.Combine( constraints.GetVertexConstraint( verts.A ) );
            constraints.SetOrUpdateVertexConstraint( verts.A, a );
            b.Combine( constraints.GetVertexConstraint( verts.B ) );
            constraints.SetOrUpdateVertexConstraint( verts.B, b );
        }
    }

    // ---- TMeshSimplification ----

    void QemSimplification::SimplifyToTriangleCount( int count )
    {
        m_Mode   = TargetMode::TriangleCount;
        m_Target = std::max( 1, count );
        DoSimplify();
    }

    void QemSimplification::SimplifyToVertexCount( int count )
    {
        m_Mode   = TargetMode::VertexCount;
        m_Target = std::max( 3, count );
        DoSimplify();
    }

    void QemSimplification::Precompute()
    {
        m_HaveBoundary = false;
        m_IsBoundaryVertex.assign( static_cast<size_t>( m_Mesh.MaxVertexID() ), false );
        for ( const int edge : m_Mesh.BoundaryEdgeIndicesItr() )
        {
            const Index2i ev         = m_Mesh.GetEdgeV( edge );
            m_IsBoundaryVertex[ev.A] = true;
            m_IsBoundaryVertex[ev.B] = true;
            m_HaveBoundary           = true;
        }
    }

    void QemSimplification::InitializeTriQuadrics()
    {
        const auto count = static_cast<size_t>( m_Mesh.MaxTriangleID() );
        m_TriQuadrics.assign( count, QuadricErrord{} );
        m_TriAreas.assign( count, 0.0 );
        glm::dvec3 normal( 0.0 );
        glm::dvec3 centroid( 0.0 );
        for ( const int tri : m_Mesh.TriangleIndicesItr() )
        {
            m_Mesh.GetTriInfo( tri, normal, m_TriAreas[tri], centroid );
            m_TriQuadrics[tri] = QuadricErrord( normal, centroid );
        }
    }

    void QemSimplification::InitializeVertexQuadrics()
    {
        m_VertQuadrics.assign( static_cast<size_t>( m_Mesh.MaxVertexID() ), QuadricErrord{} );
        for ( const int vertex : m_Mesh.VertexIndicesItr() )
            for ( const int tri : m_Mesh.VtxTrianglesItr( vertex ) )
                m_VertQuadrics[vertex].Add( m_TriAreas[tri], m_TriQuadrics[tri] );
    }

    // CreateSeamQuadric (QuadricError.h:831-870) for both faces of @p edge, scaled by SeamEdgeWeight
    // (MeshSimplification.cpp:286-305): the plane through the edge perpendicular to each face, so a vertex that
    // leaves the seam's line pays for it.
    QuadricErrord QemSimplification::SeamQuadric( int edge ) const
    {
        constexpr double         kSeamEdgeWeight = 256.0;
        const DynamicMesh3::Edge e               = m_Mesh.GetEdge( edge );
        const glm::dvec3         p0              = m_Mesh.GetVertex( e.Vert.A );
        const glm::dvec3         p1              = m_Mesh.GetVertex( e.Vert.B );
        const auto               side            = [&]( int tri )
        {
            const glm::dvec3 normal   = m_Mesh.GetTriNormal( tri );
            const double     lengthSq = glm::dot( normal, normal );
            const double     edgeSq   = glm::dot( p1 - p0, p1 - p0 );
            QuadricErrord    q;
            if ( std::abs( lengthSq - 1.0 ) > 1e5 * 2.220446049250313e-16 || edgeSq < 1e3 * 2.220446049250313e-16 )
                return q;
            const double length = std::sqrt( edgeSq );
            q.Add( length, QuadricErrord( glm::cross( ( p1 - p0 ) / length, normal ), p0 ) );
            return q;
        };
        QuadricErrord seam;
        seam.Add( kSeamEdgeWeight, side( e.Tri.A ) );
        if ( e.Tri.B != DynamicMesh3::InvalidID )
            seam.Add( kSeamEdgeWeight, side( e.Tri.B ) );
        return seam;
    }

    // InitializeSeamQuadrics with constraints: every constrained edge (seam, mesh or group boundary) carries one.
    void QemSimplification::InitializeSeamQuadrics()
    {
        m_SeamQuadrics.clear();
        if ( !Boundaries.AllowSeamCollapse || !m_Constraints )
            return;
        for ( const auto& [edge, constraint] : m_Constraints->GetEdgeConstraints() )
            if ( m_Mesh.IsEdge( edge ) )
                m_SeamQuadrics.emplace( edge, SeamQuadric( edge ) );
    }

    // bRetainQuadricMemory = false: the edge's own triangles are counted once in each end's quadric; the sum
    // counts them twice, so one copy is removed.
    QuadricErrord QemSimplification::AssembleEdgeQuadric( const DynamicMesh3::Edge& edge ) const
    {
        QuadricErrord q = m_VertQuadrics[edge.Vert.A];
        q.Add( 1.0, m_VertQuadrics[edge.Vert.B] );
        if ( edge.Tri.A != DynamicMesh3::InvalidID )
            q.Add( -m_TriAreas[edge.Tri.A], m_TriQuadrics[edge.Tri.A] );
        if ( edge.Tri.B != DynamicMesh3::InvalidID )
            q.Add( -m_TriAreas[edge.Tri.B], m_TriQuadrics[edge.Tri.B] );
        // AddSeamQuadricsToEdge over both ends (an edge that is itself a seam is counted from each end, as in UE).
        if ( !m_SeamQuadrics.empty() )
            for ( const int vertex : { edge.Vert.A, edge.Vert.B } )
                for ( const int around : m_Mesh.VtxEdgesItr( vertex ) )
                    if ( const auto found = m_SeamQuadrics.find( around ); found != m_SeamQuadrics.end() )
                        q.Add( 1.0, found->second );
        return q;
    }

    void QemSimplification::InitializeQueue()
    {
        const int maxEdge = m_Mesh.MaxEdgeID();
        m_EdgeQuadrics.assign( static_cast<size_t>( maxEdge ), QEdge{} );
        m_Queue.Initialize( maxEdge );
        struct EdgeError
        {
            float Error;
            int   Edge;
        };
        std::vector<EdgeError> errors;
        errors.reserve( static_cast<size_t>( m_Mesh.EdgeCount() ) );
        for ( const int edge : m_Mesh.EdgeIndicesItr() )
        {
            const DynamicMesh3::Edge e     = m_Mesh.GetEdge( edge );
            const QuadricErrord      q     = AssembleEdgeQuadric( e );
            const glm::dvec3         point = OptimalPoint( edge, q, e.Vert.A, e.Vert.B );
            m_EdgeQuadrics[edge]           = { q, point };
            errors.push_back( { static_cast<float>( q.Evaluate( point ) ), edge } );
        }
        // UE sorts first so the heap is built in error order (MeshSimplification.cpp:630).
        std::sort( errors.begin(), errors.end(),
                   []( const EdgeError& x, const EdgeError& y ) { return x.Error < y.Error; } );
        for ( const EdgeError& e : errors )
            m_Queue.Insert( e.Edge, e.Error );
    }

    glm::dvec3 QemSimplification::OptimalPoint( int edge, const QuadricErrord& q, int a, int b ) const
    {
        const glm::dvec3 va        = m_Mesh.GetVertex( a );
        const glm::dvec3 vb        = m_Mesh.GetVertex( b );
        glm::dvec3       best      = va;
        double           bestError = q.Evaluate( va );
        const auto       consider  = [&]( const glm::dvec3& p )
        {
            const double error = q.Evaluate( p );
            if ( error < bestError )
            {
                bestError = error;
                best      = p;
            }
        };
        // bPreserveBoundaryShape: a boundary edge collapses to a point ON the edge, an edge with one boundary
        // end to that end.
        if ( m_HaveBoundary )
        {
            if ( m_Mesh.IsBoundaryEdge( edge ) )
            {
                consider( vb );
                consider( ( va + vb ) * 0.5 );
                glm::dvec3 unconstrained( 0.0 );
                if ( q.OptimalPoint( unconstrained ) )
                {
                    const glm::dvec3 d      = vb - va;
                    const double     length = glm::dot( d, d );
                    const double     t =
                         length > 0.0 ? std::clamp( glm::dot( unconstrained - va, d ) / length, 0.0, 1.0 ) : 0.0;
                    consider( va + t * d );
                }
                return best;
            }
            if ( m_IsBoundaryVertex[a] )
                return va;
            if ( m_IsBoundaryVertex[b] )
                return vb;
        }
        glm::dvec3 result( 0.0 );
        if ( q.OptimalPoint( result ) )
            return result;
        consider( vb );
        consider( ( va + vb ) * 0.5 );
        return best;
    }

    // FMeshRefinerBase::CanCollapseVertex without projection targets or fixed sets.
    bool QemSimplification::CanCollapseVertex( int a, int b, int& collapseTo ) const
    {
        collapseTo = -1;
        if ( !m_Constraints )
            return true;
        const VertexConstraint ca = m_Constraints->GetVertexConstraint( a );
        const VertexConstraint cb = m_Constraints->GetVertexConstraint( b );
        if ( !ca.CanMove && !cb.CanMove )
            return false;
        if ( !ca.CannotDelete && !cb.CannotDelete )
            return true;
        if ( ca.CannotDelete && !cb.CannotDelete )
        {
            collapseTo = a;
            return true;
        }
        if ( cb.CannotDelete && !ca.CannotDelete )
        {
            collapseTo = b;
            return true;
        }
        return false;
    }

    // TMeshSimplification::CanCollapseEdge (MeshSimplification.cpp:1735-1760): an end touching a side edge that
    // may not merge topology (NoTopologyMerge) is kept; a seam or a free mesh border only forbids flips.
    bool QemSimplification::CanCollapseEdge( int a, int b, int c, int d, int tc, int td, int& collapseTo ) const
    {
        collapseTo = -1;
        if ( !m_Constraints )
            return true;
        if ( !CanCollapseVertex( a, b, collapseTo ) )
            return false;
        bool retainA = false;
        bool retainB = false;
        if ( c != DynamicMesh3::InvalidID )
        {
            retainA = retainA ||
                      !m_Constraints->GetEdgeConstraint( m_Mesh.FindEdgeFromTri( a, c, tc ) ).CanMergeTopology();
            retainB = retainB ||
                      !m_Constraints->GetEdgeConstraint( m_Mesh.FindEdgeFromTri( b, c, tc ) ).CanMergeTopology();
        }
        if ( d != DynamicMesh3::InvalidID )
        {
            retainA = retainA ||
                      !m_Constraints->GetEdgeConstraint( m_Mesh.FindEdgeFromTri( a, d, td ) ).CanMergeTopology();
            retainB = retainB ||
                      !m_Constraints->GetEdgeConstraint( m_Mesh.FindEdgeFromTri( b, d, td ) ).CanMergeTopology();
        }
        if ( retainA && retainB )
            return false;
        if ( collapseTo == -1 )
        {
            if ( retainA )
                collapseTo = a;
            else if ( retainB )
                collapseTo = b;
            return true;
        }
        return ( collapseTo != a || !retainB ) && ( collapseTo != b || !retainA );
    }

    // FMeshRefinerBase::CheckIfCollapseCreatesFlipOrInvalid with ComputeEdgeFlipMetric
    // (MeshRefinerBase.h:157-166): tolerance 0 compares the raw cross products, any other tolerance the unit
    // normals.
    bool QemSimplification::CreatesFlipOrInvalid( int vertex, int other, const glm::dvec3& newPosition, int tc,
                                                  int td ) const
    {
        glm::dvec3 va( 0.0 );
        glm::dvec3 vb( 0.0 );
        glm::dvec3 vc( 0.0 );
        for ( const int tri : m_Mesh.VtxTrianglesItr( vertex ) )
        {
            if ( tri == tc || tri == td )
                continue;
            const Index3i t = m_Mesh.GetTriangle( tri );
            if ( t.A == other || t.B == other || t.C == other )
                return true;
            m_Mesh.GetTriVertices( tri, va, vb, vc );
            const glm::dvec3 current = glm::cross( vb - va, vc - va );
            glm::dvec3       moved( 0.0 );
            if ( t.A == vertex )
                moved = glm::cross( vb - newPosition, vc - newPosition );
            else if ( t.B == vertex )
                moved = glm::cross( newPosition - va, vc - va );
            else
                moved = glm::cross( vb - va, newPosition - va );
            const double metric = m_EdgeFlipTolerance == 0.0
                                       ? glm::dot( current, moved )
                                       : glm::dot( glm::normalize( current ), glm::normalize( moved ) );
            if ( !( metric > m_EdgeFlipTolerance ) )
                return true;
        }
        return false;
    }

    // FMeshRefinerBase::CheckIfCollapseCreatesTinyTriangle (MeshRefinerBase.cpp:61-95): a moved triangle whose
    // squared doubled area drops below TinyTriangleThreshold (SMALL_NUMBER) refuses the collapse, unless it was
    // already tiny.
    bool QemSimplification::CreatesTinyTriangle( int vertex, int other, const glm::dvec3& newPosition, int tc,
                                                 int td ) const
    {
        constexpr double kTinyTriangleThreshold = 1e-8;
        for ( const int tri : m_Mesh.VtxTrianglesItr( vertex ) )
        {
            if ( tri == tc || tri == td )
                continue;
            const Index3i t = m_Mesh.GetTriangle( tri );
            if ( t.A == other || t.B == other || t.C == other )
                return true;
            glm::dvec3 v[3];
            m_Mesh.GetTriVertices( tri, v[0], v[1], v[2] );
            const glm::dvec3 current = glm::cross( v[1] - v[0], v[2] - v[0] );
            if ( glm::dot( current, current ) < kTinyTriangleThreshold )
                continue;
            int movedIndex = 2;
            if ( t.A == vertex )
                movedIndex = 0;
            else if ( t.B == vertex )
                movedIndex = 1;
            v[movedIndex]          = newPosition;
            const glm::dvec3 moved = glm::cross( v[1] - v[0], v[2] - v[0] );
            if ( glm::dot( moved, moved ) < kTinyTriangleThreshold )
                return true;
        }
        return false;
    }

    void QemSimplification::UpdateConstraintsAround( int edge )
    {
        if ( !m_Constraints )
            return;
        EdgeConstraint   edgeConstraint;
        VertexConstraint a;
        VertexConstraint b;
        if ( !ConstrainEdgeBoundariesAndSeams( edge, m_Mesh, Boundaries, edgeConstraint, a, b ) )
            return;
        const Index2i verts = m_Mesh.GetEdgeV( edge );
        m_Constraints->SetOrUpdateEdgeConstraint( edge, edgeConstraint );
        a.Combine( m_Constraints->GetVertexConstraint( verts.A ) );
        m_Constraints->SetOrUpdateVertexConstraint( verts.A, a );
        b.Combine( m_Constraints->GetVertexConstraint( verts.B ) );
        m_Constraints->SetOrUpdateVertexConstraint( verts.B, b );
    }

    QemSimplification::CollapseResult QemSimplification::CollapseEdge( int edge, glm::dvec3 newPosition,
                                                                       DynamicMeshInfo::EdgeCollapseInfo& info )
    {
        if ( m_Constraints )
        {
            const EdgeConstraint constraint = m_Constraints->GetEdgeConstraint( edge );
            if ( constraint.NoModifications() || !constraint.CanCollapse() )
                return CollapseResult::Ignored;
        }
        if ( !m_Mesh.IsEdge( edge ) )
            return CollapseResult::Failed;
        const DynamicMesh3::Edge e  = m_Mesh.GetEdge( edge );
        const int                a  = e.Vert.A;
        const int                b  = e.Vert.B;
        const int                t0 = e.Tri.A;
        const int                t1 = e.Tri.B;
        const int                c  = IndexUtil::FindTriOtherVtx( a, b, m_Mesh.GetTriangle( t0 ) );
        const int                d  = t1 == DynamicMesh3::InvalidID ? DynamicMesh3::InvalidID
                                                                    : IndexUtil::FindTriOtherVtx( a, b, m_Mesh.GetTriangle( t1 ) );

        int collapseTo = -1;
        if ( !CanCollapseEdge( a, b, c, d, t0, t1, collapseTo ) )
            return CollapseResult::Ignored;
        if ( m_HaveBoundary )
        {
            if ( collapseTo != -1 &&
                 ( ( m_IsBoundaryVertex[b] && collapseTo != b ) || ( m_IsBoundaryVertex[a] && collapseTo != a ) ) )
                return CollapseResult::Ignored;
            if ( m_IsBoundaryVertex[b] )
                collapseTo = b;
            else if ( m_IsBoundaryVertex[a] )
                collapseTo = a;
        }

        int  keep                   = b;
        int  removed                = a;
        bool constraintsFixPosition = false;
        if ( collapseTo != -1 )
        {
            keep                   = collapseTo;
            removed                = keep == a ? b : a;
            constraintsFixPosition = m_Constraints && !m_Constraints->GetVertexConstraint( collapseTo ).CanMove;
        }
        double t = 0.0;
        if ( constraintsFixPosition )
            newPosition = m_Mesh.GetVertex( keep );
        else
        {
            // CurveUtil::ProjectToSegment( keep, removed, newPosition ).
            const glm::dvec3 vKeep   = m_Mesh.GetVertex( keep );
            const glm::dvec3 segment = m_Mesh.GetVertex( removed ) - vKeep;
            const double     length  = glm::dot( segment, segment );
            t = length > 0.0 ? std::clamp( glm::dot( newPosition - vKeep, segment ) / length, 0.0, 1.0 ) : 0.0;
        }
        if ( CreatesFlipOrInvalid( a, b, newPosition, t0, t1 ) ||
             CreatesFlipOrInvalid( b, a, newPosition, t0, t1 ) )
            return CollapseResult::Ignored;
        if ( PreventTinyTriangles && ( CreatesTinyTriangle( a, b, newPosition, t0, t1 ) ||
                                       CreatesTinyTriangle( b, a, newPosition, t0, t1 ) ) )
            return CollapseResult::Ignored;

        if ( m_Mesh.CollapseEdge( keep, removed, t, info ) != MeshResult::Ok )
            return CollapseResult::Failed;
        m_Mesh.SetVertex( keep, newPosition );
        if ( m_Constraints )
        {
            m_Constraints->ClearEdgeConstraint( edge );
            for ( int side = 0; side < 2; ++side )
            {
                const int removedEdge = info.RemovedEdges[side];
                if ( removedEdge == DynamicMesh3::InvalidID || !m_Constraints->HasEdgeConstraint( removedEdge ) )
                    continue;
                m_Constraints->ClearEdgeConstraint( info.KeptEdges[side] );
                m_Constraints->ClearEdgeConstraint( removedEdge );
                UpdateConstraintsAround( info.KeptEdges[side] );
            }
            m_Constraints->ClearVertexConstraint( removed );
        }
        return CollapseResult::Collapsed;
    }

    void QemSimplification::UpdateNeighborhood( const DynamicMeshInfo::EdgeCollapseInfo& info )
    {
        const int kept = info.KeptVertex;
        // MeshSimplification.cpp:786-870: the kept edges carry a seam quadric exactly when they are constrained,
        // the removed edges drop theirs, and every seam quadric around the kept vertex is rebuilt.
        if ( Boundaries.AllowSeamCollapse && m_Constraints )
        {
            for ( int side = 0; side < 2; ++side )
            {
                const int keptEdge = info.KeptEdges[side];
                if ( keptEdge == DynamicMesh3::InvalidID )
                    continue;
                if ( m_Constraints->HasEdgeConstraint( keptEdge ) )
                    m_SeamQuadrics[keptEdge] = QuadricErrord{};
                else
                    m_SeamQuadrics.erase( keptEdge );
            }
            for ( int side = 0; side < 2; ++side )
                if ( info.RemovedEdges[side] != DynamicMesh3::InvalidID )
                    m_SeamQuadrics.erase( info.RemovedEdges[side] );
            for ( const int around : m_Mesh.VtxEdgesItr( kept ) )
                if ( const auto found = m_SeamQuadrics.find( around ); found != m_SeamQuadrics.end() )
                    found->second = SeamQuadric( around );
        }
        glm::dvec3 normal( 0.0 );
        glm::dvec3 centroid( 0.0 );
        for ( const int tri : m_Mesh.VtxTrianglesItr( kept ) )
        {
            const double        oldArea = m_TriAreas[tri];
            const QuadricErrord oldQ    = m_TriQuadrics[tri];
            double              newArea = 0.0;
            m_Mesh.GetTriInfo( tri, normal, newArea, centroid );
            const QuadricErrord newQ( normal, centroid );
            m_TriAreas[tri]     = newArea;
            m_TriQuadrics[tri]  = newQ;
            const Index3i verts = m_Mesh.GetTriangle( tri );
            for ( int i = 0; i < 3; ++i )
            {
                if ( verts[i] == kept )
                    continue;
                m_VertQuadrics[verts[i]].Add( -oldArea, oldQ );
                m_VertQuadrics[verts[i]].Add( newArea, newQ );
            }
        }
        for ( int i = 0; i < 2; ++i )
        {
            const int removedTri = info.RemovedTris[i];
            if ( removedTri == DynamicMesh3::InvalidID )
                continue;
            m_VertQuadrics[info.OpposingVerts[i]].Add( -m_TriAreas[removedTri], m_TriQuadrics[removedTri] );
            m_TriQuadrics[removedTri] = QuadricErrord{};
            m_TriAreas[removedTri]    = 0.0;
        }
        QuadricErrord keptQ;
        for ( const int tri : m_Mesh.VtxTrianglesItr( kept ) )
            keptQ.Add( m_TriAreas[tri], m_TriQuadrics[tri] );
        m_VertQuadrics[kept] = keptQ;

        // Every edge of the kept vertex's one-ring and of its neighbours' rings (UE's two-ring update).
        std::vector<int> edges;
        for ( const int adjacent : m_Mesh.VtxEdgesItr( kept ) )
        {
            if ( std::find( edges.begin(), edges.end(), adjacent ) == edges.end() )
                edges.push_back( adjacent );
            const Index2i verts    = m_Mesh.GetEdgeV( adjacent );
            const int     neighbor = verts.A == kept ? verts.B : verts.A;
            for ( const int edge : m_Mesh.VtxEdgesItr( neighbor ) )
                if ( std::find( edges.begin(), edges.end(), edge ) == edges.end() )
                    edges.push_back( edge );
        }
        for ( const int edge : edges )
        {
            const DynamicMesh3::Edge e     = m_Mesh.GetEdge( edge );
            const QuadricErrord      q     = AssembleEdgeQuadric( e );
            const glm::dvec3         point = OptimalPoint( edge, q, e.Vert.A, e.Vert.B );
            const auto               error = static_cast<float>( q.Evaluate( point ) );
            m_EdgeQuadrics[edge]           = { q, point };
            if ( m_Queue.Contains( edge ) )
                m_Queue.Update( edge, error );
            else
                m_Queue.Insert( edge, error );
        }
    }

    void QemSimplification::DoSimplify()
    {
        m_Collapses = 0;
        if ( m_Mesh.TriangleCount() == 0 )
            return;
        Precompute();
        InitializeTriQuadrics();
        InitializeVertexQuadrics();
        InitializeSeamQuadrics();
        InitializeQueue();
        while ( m_Queue.GetCount() > 0 )
        {
            const int current = m_Mode == TargetMode::VertexCount ? m_Mesh.VertexCount() : m_Mesh.TriangleCount();
            if ( current <= m_Target )
                break;
            const int edge = m_Queue.Dequeue();
            if ( !m_Mesh.IsEdge( edge ) )
                continue;
            DynamicMeshInfo::EdgeCollapseInfo info;
            if ( CollapseEdge( edge, m_EdgeQuadrics[edge].CollapsePoint, info ) == CollapseResult::Collapsed )
            {
                ++m_Collapses;
                UpdateNeighborhood( info );
            }
        }
    }
} // namespace Desert::Geometry
