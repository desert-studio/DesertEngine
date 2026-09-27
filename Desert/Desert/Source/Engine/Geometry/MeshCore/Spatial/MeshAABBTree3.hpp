// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Spatial/MeshAABBTree3.h:64-180,238-335,1060-1093,
// 1150-1239,1265-1500,2113-2141,2172-2312 (+ SpatialInterfaces.h:56-78 FQueryOptions, BoxTypes.h:492-529
// Contains/Intersects), adapted: glm, namespace Desert::Geometry, DynamicMesh3 only (no TriangleMeshType
// template), std::vector for TDynamicVector (InsertAt = set-at-index, grown on demand). Kept only what
// FMeshBoolean needs: Build (top-down midpoint split, 3 triangles per leaf, split axis Depth % 3),
// FindNearestTriangle with MaxDistance and TriangleFilter, FindAllIntersections between two trees, SetTolerance
// (box epsilon), and the box/index layout that FastWindingTree walks. Left out: ray casts, vertex queries,
// self-intersection, the per-tree transform of FindAllIntersections (the Boolean passes nullptr),
// bAllowUnsafeModifiedMeshQueries (a query on a stale tree is an assert here, as UE's DO_GUARD_SLOW check),
// SetBuildOptions (nobody overrides the leaf size or split axis). The intersection result carries SEGMENTS only:
// the only consumer is MeshMeshCut, which reads Segments alone, and our IntrTriangle3Triangle3 port never reports
// coplanar polygons (Quantity > 2); a single touching point (Quantity == 1) has no length to cut along, so UE's
// Points list would be written and never read.

#pragma once

#include "Engine/Geometry/MeshCore/BoxTypes.hpp"
#include "Engine/Geometry/MeshCore/Distance/DistPoint3Triangle3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/Intersection/IntrTriangle3Triangle3.hpp"
#include "Engine/Geometry/MeshCore/MathUtil.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace Desert::Geometry
{
    namespace MeshIntersection
    {
        struct SegmentIntersection
        {
            // {triangle of the tree that was queried, triangle of the other tree}
            int        TriangleID[2]{ -1, -1 };
            glm::dvec3 Point[2]{};
        };

        struct IntersectionsQueryResult
        {
            std::vector<SegmentIntersection> Segments;
        };
    } // namespace MeshIntersection

    namespace MeshAABBTreeDetail
    {
        // UE TAxisAlignedBox3::Contains / Intersects (BoxTypes.h:492-529); our BoxTypes port has neither.
        inline bool BoxContains( const AxisAlignedBox3d& Box, const glm::dvec3& V )
        {
            return ( Box.Min.x <= V.x ) && ( Box.Min.y <= V.y ) && ( Box.Min.z <= V.z ) && ( Box.Max.x >= V.x ) &&
                   ( Box.Max.y >= V.y ) && ( Box.Max.z >= V.z );
        }

        inline bool BoxIntersects( const AxisAlignedBox3d& A, const AxisAlignedBox3d& B )
        {
            return !( ( B.Max.x <= A.Min.x ) || ( B.Min.x >= A.Max.x ) || ( B.Max.y <= A.Min.y ) ||
                      ( B.Min.y >= A.Max.y ) || ( B.Max.z <= A.Min.z ) || ( B.Min.z >= A.Max.z ) );
        }

        template <typename T>
        void SetAt( std::vector<T>& Vector, const T& Value, int Index )
        {
            if ( Index >= static_cast<int>( Vector.size() ) )
            {
                Vector.resize( static_cast<size_t>( Index ) + 1 );
            }
            Vector[static_cast<size_t>( Index )] = Value;
        }
    } // namespace MeshAABBTreeDetail

    // UE IMeshSpatial::FQueryOptions (declared outside the tree so its default member initializers can back
    // default arguments inside the class).
    struct MeshQueryOptions
    {
        double                     MaxDistance = std::numeric_limits<double>::max();
        std::function<bool( int )> TriangleFilter;

        MeshQueryOptions() = default;
        explicit MeshQueryOptions( std::function<bool( int )> Filter ) : TriangleFilter( std::move( Filter ) )
        {
        }
        explicit MeshQueryOptions( double MaxDistanceIn, std::function<bool( int )> Filter = nullptr )
             : MaxDistance( MaxDistanceIn ), TriangleFilter( std::move( Filter ) )
        {
        }
    };

    class DynamicMeshAABBTree3
    {
    public:
        using QueryOptions = MeshQueryOptions;

        using IntersectionFunc = std::function<bool( IntrTriangle3Triangle3& )>;

        DynamicMeshAABBTree3() = default;
        explicit DynamicMeshAABBTree3( const DynamicMesh3* SourceMesh, bool bAutoBuild = true )
        {
            SetMesh( SourceMesh, bAutoBuild );
        }

        void SetMesh( const DynamicMesh3* SourceMesh, bool bAutoBuild = true )
        {
            m_Mesh            = SourceMesh;
            m_MeshChangeStamp = 0;
            if ( bAutoBuild )
            {
                Build();
            }
        }

        const DynamicMesh3* GetMesh() const
        {
            return m_Mesh;
        }

        void Build()
        {
            BuildTopDown();
            m_MeshChangeStamp = m_Mesh->GetChangeStamp();
        }

        // The tree matches the mesh it was built from (no edit since Build()).
        bool IsValid() const
        {
            return RootIndex >= 0 && m_MeshChangeStamp == m_Mesh->GetChangeStamp();
        }

        uint64_t GetMeshChangeStamp() const
        {
            return m_MeshChangeStamp;
        }

        // Box intersection tolerance (UE SetTolerance): the FindAllIntersections box test and box_contains widen
        // this tree's boxes by it.
        void SetTolerance( double Tolerance )
        {
            m_BoxEps = Tolerance;
        }

        int FindNearestTriangle( const glm::dvec3& P, double& NearestDistSqr,
                                 const QueryOptions& Options = QueryOptions() ) const
        {
            assert( IsValid() );
            NearestDistSqr = ( Options.MaxDistance < std::numeric_limits<double>::max() )
                                  ? Options.MaxDistance * Options.MaxDistance
                                  : std::numeric_limits<double>::max();
            int NearestID  = DynamicMesh3::InvalidID;
            FindNearestTri( RootIndex, P, NearestDistSqr, NearestID, Options );
            return NearestID;
        }

        // Every crossing between this tree's triangles and OtherTree's. IntersectionFn runs the pair test
        // (default: Find() at the triangle-pair default tolerance); the Boolean passes one that sets its snap
        // tolerance first.
        MeshIntersection::IntersectionsQueryResult
        FindAllIntersections( const DynamicMeshAABBTree3& OtherTree, const QueryOptions& Options = QueryOptions(),
                              const QueryOptions& OtherTreeOptions = QueryOptions(),
                              IntersectionFunc    IntersectionFn   = nullptr ) const
        {
            if ( !IntersectionFn )
            {
                IntersectionFn = []( IntrTriangle3Triangle3& Intr ) { return Intr.Find(); };
            }
            MeshIntersection::IntersectionsQueryResult Result;
            assert( IsValid() && OtherTree.IsValid() );
            FindIntersections( RootIndex, OtherTree, OtherTree.RootIndex, 0, Result, IntersectionFn, Options,
                               OtherTreeOptions );
            return Result;
        }

        AxisAlignedBox3d GetBox( int IBox ) const
        {
            const glm::dvec3& c = BoxCenters[static_cast<size_t>( IBox )];
            const glm::dvec3& e = BoxExtents[static_cast<size_t>( IBox )];
            return AxisAlignedBox3d( c - e, c + e );
        }

        AxisAlignedBox3d GetBoxEps( int IBox, double Epsilon = ZeroTolerance<double> ) const
        {
            const glm::dvec3& c = BoxCenters[static_cast<size_t>( IBox )];
            const glm::dvec3  e = BoxExtents[static_cast<size_t>( IBox )] + glm::dvec3( Epsilon );
            return AxisAlignedBox3d( c - e, c + e );
        }

        double BoxDistanceSqr( int IBox, const glm::dvec3& V ) const
        {
            const glm::dvec3& c  = BoxCenters[static_cast<size_t>( IBox )];
            const glm::dvec3& e  = BoxExtents[static_cast<size_t>( IBox )];
            const double      dx = std::max( std::fabs( V.x - c.x ) - e.x, 0.0 );
            const double      dy = std::max( std::fabs( V.y - c.y ) - e.y, 0.0 );
            const double      dz = std::max( std::fabs( V.z - c.z ) - e.z, 0.0 );
            return dx * dx + dy * dy + dz * dz;
        }

        bool BoxContains( int IBox, const glm::dvec3& P ) const
        {
            return MeshAABBTreeDetail::BoxContains( GetBoxEps( IBox, m_BoxEps ), P );
        }

        bool BoxBoxIntersect( int IBox, const AxisAlignedBox3d& TestBox ) const
        {
            return MeshAABBTreeDetail::BoxIntersects( GetBoxEps( IBox, m_BoxEps ), TestBox );
        }

        // Layout (UE's, read by FastWindingTree): BoxToIndex[box] points into IndexList. Below TrianglesEnd a box
        // is a leaf [N t1 .. tN]; above it an internal node [c1 c2] or [-(c+1)] for one child, children stored +1.
        std::vector<int>        BoxToIndex;
        std::vector<glm::dvec3> BoxCenters;
        std::vector<glm::dvec3> BoxExtents;
        std::vector<int>        IndexList;
        int                     TrianglesEnd = -1;
        int                     RootIndex    = -1;

    private:
        struct BoxesSet
        {
            std::vector<int>        BoxToIndex;
            std::vector<glm::dvec3> BoxCenters;
            std::vector<glm::dvec3> BoxExtents;
            std::vector<int>        IndexList;
            int                     IBoxCur     = 0;
            int                     IIndicesCur = 0;
        };

        static constexpr int kTopDownLeafMaxTriCount = 3;

        const DynamicMesh3* m_Mesh            = nullptr;
        uint64_t            m_MeshChangeStamp = 0;
        double              m_BoxEps          = ZeroTolerance<double>;

        void BuildTopDown()
        {
            std::vector<int>        Triangles;
            std::vector<glm::dvec3> Centers;
            Triangles.reserve( static_cast<size_t>( m_Mesh->TriangleCount() ) );
            Centers.reserve( static_cast<size_t>( m_Mesh->TriangleCount() ) );
            for ( int ti = 0; ti < m_Mesh->MaxTriangleID(); ti++ )
            {
                if ( !m_Mesh->IsTriangle( ti ) )
                {
                    continue;
                }
                const glm::dvec3 Centroid = m_Mesh->GetTriCentroid( ti );
                const double     d2       = glm::dot( Centroid, Centroid );
                // A NaN/inf centroid would poison every box above it; UE skips the triangle.
                if ( std::isfinite( d2 ) )
                {
                    Triangles.push_back( ti );
                    Centers.push_back( Centroid );
                }
            }

            BoxesSet         Tris;
            BoxesSet         Nodes;
            AxisAlignedBox3d RootBox;
            const int RootNode = SplitTriSetMidpoint( Triangles, Centers, 0, static_cast<int>( Triangles.size() ),
                                                      0, kTopDownLeafMaxTriCount, Tris, Nodes, RootBox );

            BoxToIndex            = Tris.BoxToIndex;
            BoxCenters            = Tris.BoxCenters;
            BoxExtents            = Tris.BoxExtents;
            IndexList             = Tris.IndexList;
            TrianglesEnd          = Tris.IIndicesCur;
            const int iIndexShift = TrianglesEnd;
            const int iBoxShift   = Tris.IBoxCur;

            // Append the internal nodes after the leaves, shifting their box and index references.
            for ( int i = 0; i < Nodes.IBoxCur; ++i )
            {
                const auto ui = static_cast<size_t>( i );
                MeshAABBTreeDetail::SetAt( BoxCenters, Nodes.BoxCenters[ui], iBoxShift + i );
                MeshAABBTreeDetail::SetAt( BoxExtents, Nodes.BoxExtents[ui], iBoxShift + i );
                MeshAABBTreeDetail::SetAt( BoxToIndex, iIndexShift + Nodes.BoxToIndex[ui], iBoxShift + i );
            }
            for ( int i = 0; i < Nodes.IIndicesCur; ++i )
            {
                int ChildBox = Nodes.IndexList[static_cast<size_t>( i )];
                if ( ChildBox < 0 )
                {
                    ChildBox = ( -ChildBox ) - 1; // a leaf box
                }
                else
                {
                    ChildBox += iBoxShift;
                }
                MeshAABBTreeDetail::SetAt( IndexList, ChildBox + 1, iIndexShift + i );
            }
            RootIndex = RootNode + iBoxShift;
        }

        int SplitTriSetMidpoint( std::vector<int>& Triangles, std::vector<glm::dvec3>& Centers, int IStart,
                                 int ICount, int Depth, int MinTriCount, BoxesSet& Tris, BoxesSet& Nodes,
                                 AxisAlignedBox3d& Box ) const
        {
            Box      = !Triangles.empty() ? AxisAlignedBox3d::Empty()
                                          : AxisAlignedBox3d( glm::dvec3( 0 ), glm::dvec3( 0 ) );
            int IBox = -1;

            if ( ICount <= MinTriCount )
            {
                IBox = Tris.IBoxCur++;
                MeshAABBTreeDetail::SetAt( Tris.BoxToIndex, Tris.IIndicesCur, IBox );
                MeshAABBTreeDetail::SetAt( Tris.IndexList, ICount, Tris.IIndicesCur++ );
                for ( int i = 0; i < ICount; ++i )
                {
                    const int ti = Triangles[static_cast<size_t>( IStart + i )];
                    MeshAABBTreeDetail::SetAt( Tris.IndexList, ti, Tris.IIndicesCur++ );
                    Box.Contain( m_Mesh->GetTriBounds( ti ) );
                }
                MeshAABBTreeDetail::SetAt( Tris.BoxCenters, Box.Center(), IBox );
                MeshAABBTreeDetail::SetAt( Tris.BoxExtents, Box.Extents(), IBox );
                return -( IBox + 1 );
            }

            const int axis    = Depth % 3;
            double    IntvMin = std::numeric_limits<double>::max();
            double    IntvMax = -std::numeric_limits<double>::max();
            for ( int i = 0; i < ICount; ++i )
            {
                const double c = Centers[static_cast<size_t>( IStart + i )][axis];
                IntvMin        = std::min( IntvMin, c );
                IntvMax        = std::max( IntvMax, c );
            }
            const double midpoint = ( IntvMin + IntvMax ) * 0.5;

            int n0 = 0;
            int n1 = 0;
            if ( IntvMax - IntvMin > ZeroTolerance<double> )
            {
                // Hoare partition of the centroids around the midpoint.
                int l = 0;
                int r = ICount - 1;
                while ( l < r )
                {
                    while ( l < ICount && Centers[static_cast<size_t>( IStart + l )][axis] <= midpoint )
                    {
                        l++;
                    }
                    while ( r >= 0 && Centers[static_cast<size_t>( IStart + r )][axis] > midpoint )
                    {
                        r--;
                    }
                    if ( l >= r )
                    {
                        break;
                    }
                    std::swap( Centers[static_cast<size_t>( IStart + l )],
                               Centers[static_cast<size_t>( IStart + r )] );
                    std::swap( Triangles[static_cast<size_t>( IStart + l )],
                               Triangles[static_cast<size_t>( IStart + r )] );
                }
                n0 = l;
                n1 = ICount - n0;
                if ( n0 == 0 || n0 == ICount )
                {
                    n0 = ICount / 2;
                    n1 = ICount - n0;
                }
            }
            else
            {
                // All centroids coincide on this axis: split by count.
                n0 = ICount / 2;
                n1 = ICount - n0;
            }

            AxisAlignedBox3d Box1;
            const int        Child0 =
                 SplitTriSetMidpoint( Triangles, Centers, IStart, n0, Depth + 1, MinTriCount, Tris, Nodes, Box );
            const int Child1 = SplitTriSetMidpoint( Triangles, Centers, IStart + n0, n1, Depth + 1, MinTriCount,
                                                    Tris, Nodes, Box1 );
            Box.Contain( Box1 );

            IBox = Nodes.IBoxCur++;
            MeshAABBTreeDetail::SetAt( Nodes.BoxToIndex, Nodes.IIndicesCur, IBox );
            MeshAABBTreeDetail::SetAt( Nodes.IndexList, Child0, Nodes.IIndicesCur++ );
            MeshAABBTreeDetail::SetAt( Nodes.IndexList, Child1, Nodes.IIndicesCur++ );
            MeshAABBTreeDetail::SetAt( Nodes.BoxCenters, Box.Center(), IBox );
            MeshAABBTreeDetail::SetAt( Nodes.BoxExtents, Box.Extents(), IBox );
            return IBox;
        }

        double TriDistanceSqr( int ti, const glm::dvec3& P ) const
        {
            Triangle3d Tri;
            m_Mesh->GetTriVertices( ti, Tri.V[0], Tri.V[1], Tri.V[2] );
            DistPoint3Triangle3<double> Dist( P, Tri );
            return Dist.GetSquared();
        }

        void FindNearestTri( int IBox, const glm::dvec3& P, double& NearestDistSqr, int& TID,
                             const QueryOptions& Options ) const
        {
            const int idx = BoxToIndex[static_cast<size_t>( IBox )];
            if ( idx < TrianglesEnd )
            {
                const int NumTris = IndexList[static_cast<size_t>( idx )];
                for ( int i = 1; i <= NumTris; ++i )
                {
                    const int ti = IndexList[static_cast<size_t>( idx + i )];
                    if ( Options.TriangleFilter && !Options.TriangleFilter( ti ) )
                    {
                        continue;
                    }
                    const double TriDistSqr = TriDistanceSqr( ti, P );
                    if ( TriDistSqr < NearestDistSqr )
                    {
                        NearestDistSqr = TriDistSqr;
                        TID            = ti;
                    }
                }
                return;
            }

            int iChild1 = IndexList[static_cast<size_t>( idx )];
            if ( iChild1 < 0 )
            {
                // One child: descend if nearer than the current minimum.
                iChild1 = ( -iChild1 ) - 1;
                if ( BoxDistanceSqr( iChild1, P ) <= NearestDistSqr )
                {
                    FindNearestTri( iChild1, P, NearestDistSqr, TID, Options );
                }
                return;
            }

            // Two children: descend the closer box first so the farther one is usually culled.
            iChild1                    = iChild1 - 1;
            const int    iChild2       = IndexList[static_cast<size_t>( idx + 1 )] - 1;
            const double Child1DistSqr = BoxDistanceSqr( iChild1, P );
            const double Child2DistSqr = BoxDistanceSqr( iChild2, P );
            const bool   bFirstIs1     = Child1DistSqr < Child2DistSqr;
            const int    iNear         = bFirstIs1 ? iChild1 : iChild2;
            const int    iFar          = bFirstIs1 ? iChild2 : iChild1;
            const double NearDistSqr   = bFirstIs1 ? Child1DistSqr : Child2DistSqr;
            const double FarDistSqr    = bFirstIs1 ? Child2DistSqr : Child1DistSqr;
            if ( NearDistSqr < NearestDistSqr )
            {
                FindNearestTri( iNear, P, NearestDistSqr, TID, Options );
                if ( FarDistSqr < NearestDistSqr )
                {
                    FindNearestTri( iFar, P, NearestDistSqr, TID, Options );
                }
            }
        }

        static void AddTriTriIntersectionResult( const IntrTriangle3Triangle3& Intr, int TID_A, int TID_B,
                                                 MeshIntersection::IntersectionsQueryResult& Result )
        {
            if ( Intr.Quantity == 2 )
            {
                MeshIntersection::SegmentIntersection Seg;
                Seg.TriangleID[0] = TID_A;
                Seg.TriangleID[1] = TID_B;
                Seg.Point[0]      = Intr.Points[0];
                Seg.Point[1]      = Intr.Points[1];
                Result.Segments.push_back( Seg );
            }
        }

        void FindIntersections( int iBox, const DynamicMeshAABBTree3& OtherTree, int oBox, int depth,
                                MeshIntersection::IntersectionsQueryResult& Result,
                                const IntersectionFunc& IntersectFn, const QueryOptions& Options,
                                const QueryOptions& OtherTreeOptions ) const
        {
            const int idx = BoxToIndex[static_cast<size_t>( iBox )];
            const int odx = OtherTree.BoxToIndex[static_cast<size_t>( oBox )];

            if ( idx < TrianglesEnd && odx < OtherTree.TrianglesEnd )
            {
                // Two leaves: test every pair. Triangle0 = the other tree's, Triangle1 = ours, as in UE.
                IntrTriangle3Triangle3 Intr;
                Triangle3d             Tri;
                Triangle3d             OTri;
                const int              NumTris  = IndexList[static_cast<size_t>( idx )];
                const int              ONumTris = OtherTree.IndexList[static_cast<size_t>( odx )];
                for ( int j = 1; j <= ONumTris; ++j )
                {
                    const int tj = OtherTree.IndexList[static_cast<size_t>( odx + j )];
                    if ( OtherTreeOptions.TriangleFilter && !OtherTreeOptions.TriangleFilter( tj ) )
                    {
                        continue;
                    }
                    OtherTree.m_Mesh->GetTriVertices( tj, OTri.V[0], OTri.V[1], OTri.V[2] );
                    Intr.SetTriangle0( OTri );
                    for ( int i = 1; i <= NumTris; ++i )
                    {
                        const int ti = IndexList[static_cast<size_t>( idx + i )];
                        if ( Options.TriangleFilter && !Options.TriangleFilter( ti ) )
                        {
                            continue;
                        }
                        m_Mesh->GetTriVertices( ti, Tri.V[0], Tri.V[1], Tri.V[2] );
                        Intr.SetTriangle1( Tri );
                        if ( IntersectFn( Intr ) )
                        {
                            AddTriTriIntersectionResult( Intr, ti, tj, Result );
                        }
                    }
                }
                return;
            }

            // Alternate which tree descends by depth; a leaf cannot descend.
            bool bDescendOther = ( idx < TrianglesEnd || depth % 2 == 0 );
            if ( bDescendOther && odx < OtherTree.TrianglesEnd )
            {
                bDescendOther = false;
            }

            if ( bDescendOther )
            {
                const AxisAlignedBox3d Bounds  = GetBoxEps( iBox, m_BoxEps );
                int                    oChild1 = OtherTree.IndexList[static_cast<size_t>( odx )];
                if ( oChild1 < 0 )
                {
                    oChild1 = ( -oChild1 ) - 1;
                    if ( MeshAABBTreeDetail::BoxIntersects( OtherTree.GetBox( oChild1 ), Bounds ) )
                    {
                        FindIntersections( iBox, OtherTree, oChild1, depth + 1, Result, IntersectFn, Options,
                                           OtherTreeOptions );
                    }
                }
                else
                {
                    oChild1 = oChild1 - 1;
                    if ( MeshAABBTreeDetail::BoxIntersects( OtherTree.GetBox( oChild1 ), Bounds ) )
                    {
                        FindIntersections( iBox, OtherTree, oChild1, depth + 1, Result, IntersectFn, Options,
                                           OtherTreeOptions );
                    }
                    const int oChild2 = OtherTree.IndexList[static_cast<size_t>( odx + 1 )] - 1;
                    if ( MeshAABBTreeDetail::BoxIntersects( OtherTree.GetBox( oChild2 ), Bounds ) )
                    {
                        FindIntersections( iBox, OtherTree, oChild2, depth + 1, Result, IntersectFn, Options,
                                           OtherTreeOptions );
                    }
                }
                return;
            }

            const AxisAlignedBox3d OBounds = OtherTree.GetBox( oBox );
            int                    iChild1 = IndexList[static_cast<size_t>( idx )];
            if ( iChild1 < 0 )
            {
                iChild1 = ( -iChild1 ) - 1;
                if ( BoxBoxIntersect( iChild1, OBounds ) )
                {
                    FindIntersections( iChild1, OtherTree, oBox, depth + 1, Result, IntersectFn, Options,
                                       OtherTreeOptions );
                }
            }
            else
            {
                iChild1 = iChild1 - 1;
                if ( BoxBoxIntersect( iChild1, OBounds ) )
                {
                    FindIntersections( iChild1, OtherTree, oBox, depth + 1, Result, IntersectFn, Options,
                                       OtherTreeOptions );
                }
                const int iChild2 = IndexList[static_cast<size_t>( idx + 1 )] - 1;
                if ( BoxBoxIntersect( iChild2, OBounds ) )
                {
                    FindIntersections( iChild2, OtherTree, oBox, depth + 1, Result, IntersectFn, Options,
                                       OtherTreeOptions );
                }
            }
        }
    };
} // namespace Desert::Geometry
