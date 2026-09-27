// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Spatial/FastWinding.h:30-82,222-259,315-652
// (+ Util/MeshCaches.h:32-44 FMeshTriInfoCache), adapted: glm, namespace Desert::Geometry, DynamicMesh3 only,
// ComputeCoeffsSerial only (UE's parallel ComputeCoeffs reduces to the same sums), the triangle info cache built
// serially from DynamicMesh3::GetTriInfo, TOptional<TArray> as std::optional<std::vector>, FMatrix3d(u, v) as the
// outer product Order2[i][j] = u[i] * v[j] and InnerProduct as sum of A[i][j] * B[i][j] (both over glm::dmat3
// indexed [row][col] consistently; the Hessian is symmetric, so the layout cannot change the result). Left out:
// the point-set (TFastWindingTree over points) variant and the non-const FastWindingNumber that rebuilds on demand
// - the Boolean builds once and queries through the const path.

#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/Spatial/MeshAABBTree3.hpp"
#include "Engine/Geometry/MeshCore/VectorUtil.hpp"

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>

namespace Desert::Geometry
{
    namespace FastTriWinding
    {
        inline constexpr double kFourPi = 4.0 * std::numbers::pi;

        // Per-triangle normal, area and centroid, indexed by triangle ID (UE FMeshTriInfoCache).
        struct MeshTriInfoCache
        {
            std::vector<glm::dvec3> Normals;
            std::vector<double>     Areas;
            std::vector<glm::dvec3> Centroids;

            static MeshTriInfoCache Build( const DynamicMesh3& Mesh )
            {
                MeshTriInfoCache Cache;
                const auto       NT = static_cast<size_t>( Mesh.MaxTriangleID() );
                Cache.Normals.resize( NT );
                Cache.Areas.resize( NT );
                Cache.Centroids.resize( NT );
                for ( int tid = 0; tid < Mesh.MaxTriangleID(); ++tid )
                {
                    if ( Mesh.IsTriangle( tid ) )
                    {
                        const auto u = static_cast<size_t>( tid );
                        Mesh.GetTriInfo( tid, Cache.Normals[u], Cache.Areas[u], Cache.Centroids[u] );
                    }
                }
                return Cache;
            }
        };

        // Dipole expansion of a triangle set about its area-weighted centroid P: R bounds the set's reach from P,
        // Order1 = sum a*n, Order2 = sum a * (c - P) (x) n.
        inline void ComputeCoeffs( const DynamicMesh3& Mesh, const std::vector<int>& Triangles,
                                   const MeshTriInfoCache& TriCache, glm::dvec3& P, double& R, glm::dvec3& Order1,
                                   glm::dmat3& Order2 )
        {
            P      = glm::dvec3( 0 );
            Order1 = glm::dvec3( 0 );
            Order2 = glm::dmat3( 0.0 );
            R      = 0;

            double SumArea = 0;
            for ( const int tid : Triangles )
            {
                const double Area = TriCache.Areas[static_cast<size_t>( tid )];
                SumArea += Area;
                P += Area * TriCache.Centroids[static_cast<size_t>( tid )];
            }
            P /= SumArea;

            double     RSq = 0;
            glm::dvec3 P0;
            glm::dvec3 P1;
            glm::dvec3 P2;
            for ( const int tid : Triangles )
            {
                const auto        u = static_cast<size_t>( tid );
                const glm::dvec3& n = TriCache.Normals[u];
                const double      a = TriCache.Areas[u];
                Mesh.GetTriVertices( tid, P0, P1, P2 );
                Order1 += a * n;
                const glm::dvec3 dcp = TriCache.Centroids[u] - P;
                for ( int i = 0; i < 3; ++i )
                {
                    for ( int j = 0; j < 3; ++j )
                    {
                        Order2[i][j] += a * dcp[i] * n[j];
                    }
                }
                const auto DistSq = []( const glm::dvec3& A, const glm::dvec3& B )
                { return glm::dot( A - B, A - B ); };
                RSq = std::max( RSq, std::max( { DistSq( P0, P ), DistSq( P1, P ), DistSq( P2, P ) } ) );
            }
            R = std::sqrt( RSq );
        }

        // First-order (dipole) far-field winding number of the set at Q.
        inline double EvaluateOrder1Approx( const glm::dvec3& Center, const glm::dvec3& Order1Coeff,
                                            const glm::dvec3& Q )
        {
            const glm::dvec3 dpq = Center - Q;
            const double     len = glm::length( dpq );
            return ( 1.0 / kFourPi ) * glm::dot( Order1Coeff, dpq / ( len * len * len ) );
        }

        // Second-order far-field winding number: the dipole term plus the Hessian contracted with Order2.
        inline double EvaluateOrder2Approx( const glm::dvec3& Center, const glm::dvec3& Order1Coeff,
                                            const glm::dmat3& Order2Coeff, const glm::dvec3& Q )
        {
            const glm::dvec3 dpq         = Center - Q;
            const double     len         = glm::length( dpq );
            const double     len3        = len * len * len;
            const double     fourPi_len3 = 1.0 / ( kFourPi * len3 );
            const double     Order1      = fourPi_len3 * glm::dot( Order1Coeff, dpq );

            const double c      = -3.0 / ( kFourPi * len3 * len * len );
            double       Order2 = 0;
            for ( int i = 0; i < 3; ++i )
            {
                for ( int j = 0; j < 3; ++j )
                {
                    const double Hessian = ( i == j ? fourPi_len3 : 0.0 ) + c * dpq[i] * dpq[j];
                    Order2 += Order2Coeff[i][j] * Hessian;
                }
            }
            return Order1 + Order2;
        }
    } // namespace FastTriWinding

    // Fast winding number (Barill et al. 2018) over a DynamicMeshAABBTree3: leaves sum exact solid angles, a
    // subtree far enough from the query (distance > FWNBeta * its radius) is replaced by its cached expansion.
    class FastWindingTree
    {
    public:
        // Far-field acceptance: the cached expansion is used when the query is farther than FWNBeta * R.
        double FWNBeta = 2.0;
        // 1 = dipole only, 2 = dipole + second-order term.
        int FWNApproxOrder = 2;

        // The tree must outlive this object; bAutoBuild builds the cache (and the tree, if stale) right away.
        explicit FastWindingTree( DynamicMeshAABBTree3* TreeToRef, bool bAutoBuild = true )
        {
            SetTree( TreeToRef, bAutoBuild );
        }

        void SetTree( DynamicMeshAABBTree3* TreeToRef, bool bAutoBuild = true )
        {
            m_Tree = TreeToRef;
            if ( bAutoBuild )
            {
                Build( true );
            }
        }

        DynamicMeshAABBTree3* GetTree() const
        {
            return m_Tree;
        }

        void Build( bool bForceRebuild = true )
        {
            assert( m_Tree != nullptr );
            if ( !m_Tree->IsValid() )
            {
                m_Tree->Build();
            }
            if ( bForceRebuild || m_CacheMeshChangeStamp != m_Tree->GetMeshChangeStamp() )
            {
                BuildFastWindingCache();
                m_CacheMeshChangeStamp = m_Tree->GetMeshChangeStamp();
            }
        }

        bool IsBuilt() const
        {
            return m_Tree->IsValid() && m_CacheMeshChangeStamp == m_Tree->GetMeshChangeStamp();
        }

        double FastWindingNumber( const glm::dvec3& P ) const
        {
            assert( IsBuilt() );
            return BranchFastWindingNum( m_Tree->RootIndex, P );
        }

        bool IsInside( const glm::dvec3& P, double WindingIsoThreshold = 0.5 ) const
        {
            return FastWindingNumber( P ) > WindingIsoThreshold;
        }

    private:
        struct WNInfo
        {
            glm::dvec3 Center{};
            double     R = 0;
            glm::dvec3 Order1Vec{};
            glm::dmat3 Order2Mat{ 0.0 };
            bool       bValid = false;
        };

        DynamicMeshAABBTree3* m_Tree = nullptr;
        // Only internal boxes carry an expansion, and they are stored after the leaves: index = box - offset.
        std::vector<WNInfo> m_Cache;
        int                 m_CacheOffset          = 0;
        uint64_t            m_CacheMeshChangeStamp = 0;

        double BranchFastWindingNum( int IBox, const glm::dvec3& P ) const
        {
            const DynamicMeshAABBTree3& Tree      = *m_Tree;
            double                      BranchSum = 0;
            const int                   idx       = Tree.BoxToIndex[static_cast<size_t>( IBox )];
            if ( idx < Tree.TrianglesEnd )
            {
                const int NumTris = Tree.IndexList[static_cast<size_t>( idx )];
                for ( int i = 1; i <= NumTris; ++i )
                {
                    glm::dvec3 a;
                    glm::dvec3 b;
                    glm::dvec3 c;
                    Tree.GetMesh()->GetTriVertices( Tree.IndexList[static_cast<size_t>( idx + i )], a, b, c );
                    BranchSum += VectorUtil::TriSolidAngle( a, b, c, P );
                }
                return BranchSum / FastTriWinding::kFourPi;
            }

            const auto AddChild = [&]( int iChild )
            {
                if ( !Tree.BoxContains( iChild, P ) && CanUseFastWindingCache( iChild, P ) )
                {
                    BranchSum += EvaluateBoxFastWindingCache( iChild, P );
                }
                else
                {
                    BranchSum += BranchFastWindingNum( iChild, P );
                }
            };
            const int iChild1 = Tree.IndexList[static_cast<size_t>( idx )];
            if ( iChild1 < 0 )
            {
                AddChild( ( -iChild1 ) - 1 );
            }
            else
            {
                AddChild( iChild1 - 1 );
                AddChild( Tree.IndexList[static_cast<size_t>( idx + 1 )] - 1 );
            }
            return BranchSum;
        }

        void BuildFastWindingCache()
        {
            // A box needs more than this many triangles below it to get an expansion.
            constexpr int kWindingCacheThresh = 1;

            const FastTriWinding::MeshTriInfoCache TriCache =
                 FastTriWinding::MeshTriInfoCache::Build( *m_Tree->GetMesh() );

            const int NumBoxes = static_cast<int>( m_Tree->BoxToIndex.size() );
            for ( m_CacheOffset = 0; m_CacheOffset < NumBoxes; ++m_CacheOffset )
            {
                if ( m_Tree->BoxToIndex[static_cast<size_t>( m_CacheOffset )] >= m_Tree->TrianglesEnd )
                {
                    break;
                }
            }
            m_Cache.assign( static_cast<size_t>( NumBoxes - m_CacheOffset ), WNInfo{} );

            std::optional<std::vector<int>> RootTris;
            BuildFastWindingCache( m_Tree->RootIndex, 0, kWindingCacheThresh, RootTris, TriCache );
        }

        // Returns the triangle count under IBox. TriArray comes back filled with those triangles whenever a box at
        // or below IBox needed them for its expansion, so each level collects only what its children did not.
        int BuildFastWindingCache( int IBox, int Depth, int TriCountThresh,
                                   std::optional<std::vector<int>>&        TriArray,
                                   const FastTriWinding::MeshTriInfoCache& TriCache )
        {
            TriArray.reset();
            const DynamicMeshAABBTree3& Tree = *m_Tree;
            const int                   idx  = Tree.BoxToIndex[static_cast<size_t>( IBox )];
            if ( idx < Tree.TrianglesEnd )
            {
                return Tree.IndexList[static_cast<size_t>( idx )];
            }

            int iChild1 = Tree.IndexList[static_cast<size_t>( idx )];
            if ( iChild1 < 0 )
            {
                return BuildFastWindingCache( ( -iChild1 ) - 1, Depth + 1, TriCountThresh, TriArray, TriCache );
            }

            iChild1                                 = iChild1 - 1;
            const int                       iChild2 = Tree.IndexList[static_cast<size_t>( idx + 1 )] - 1;
            std::optional<std::vector<int>> Child2Array;
            const int NumTris1 = BuildFastWindingCache( iChild1, Depth + 1, TriCountThresh, TriArray, TriCache );
            const int NumTris2 =
                 BuildFastWindingCache( iChild2, Depth + 1, TriCountThresh, Child2Array, TriCache );
            const bool bBuildCache = NumTris1 + NumTris2 > TriCountThresh;

            // The root is never approximated: every query point is "near" the whole mesh's box.
            if ( Depth == 0 )
            {
                return NumTris1 + NumTris2;
            }

            if ( TriArray.has_value() || Child2Array.has_value() || bBuildCache )
            {
                if ( !TriArray.has_value() && Child2Array.has_value() )
                {
                    CollectTriangles( iChild1, *Child2Array );
                    TriArray = std::move( Child2Array );
                }
                else
                {
                    if ( !TriArray.has_value() )
                    {
                        TriArray.emplace();
                        CollectTriangles( iChild1, *TriArray );
                    }
                    if ( !Child2Array.has_value() )
                    {
                        CollectTriangles( iChild2, *TriArray );
                    }
                    else
                    {
                        TriArray->insert( TriArray->end(), Child2Array->begin(), Child2Array->end() );
                    }
                }
            }
            if ( bBuildCache )
            {
                MakeBoxFastWindingCache( IBox, *TriArray, TriCache );
            }
            return NumTris1 + NumTris2;
        }

        bool CanUseFastWindingCache( int IBox, const glm::dvec3& Q ) const
        {
            const int CacheIdx = IBox - m_CacheOffset;
            if ( CacheIdx < 0 )
            {
                return false;
            }
            const WNInfo& Info = m_Cache[static_cast<size_t>( CacheIdx )];
            if ( !Info.bValid )
            {
                return false;
            }
            const glm::dvec3 d             = Info.Center - Q;
            const double     DistThreshold = FWNBeta * Info.R;
            return glm::dot( d, d ) > DistThreshold * DistThreshold;
        }

        void MakeBoxFastWindingCache( int IBox, const std::vector<int>& Triangles,
                                      const FastTriWinding::MeshTriInfoCache& TriCache )
        {
            WNInfo& Info = m_Cache[static_cast<size_t>( IBox - m_CacheOffset )];
            assert( !Info.bValid );
            FastTriWinding::ComputeCoeffs( *m_Tree->GetMesh(), Triangles, TriCache, Info.Center, Info.R,
                                           Info.Order1Vec, Info.Order2Mat );
            Info.bValid = true;
        }

        double EvaluateBoxFastWindingCache( int IBox, const glm::dvec3& Q ) const
        {
            const WNInfo& Info = m_Cache[static_cast<size_t>( IBox - m_CacheOffset )];
            if ( FWNApproxOrder == 2 )
            {
                return FastTriWinding::EvaluateOrder2Approx( Info.Center, Info.Order1Vec, Info.Order2Mat, Q );
            }
            return FastTriWinding::EvaluateOrder1Approx( Info.Center, Info.Order1Vec, Q );
        }

        void CollectTriangles( int IBox, std::vector<int>& Triangles ) const
        {
            const DynamicMeshAABBTree3& Tree = *m_Tree;
            const int                   idx  = Tree.BoxToIndex[static_cast<size_t>( IBox )];
            if ( idx < Tree.TrianglesEnd )
            {
                const int NumTris = Tree.IndexList[static_cast<size_t>( idx )];
                for ( int i = 1; i <= NumTris; ++i )
                {
                    Triangles.push_back( Tree.IndexList[static_cast<size_t>( idx + i )] );
                }
                return;
            }
            const int iChild1 = Tree.IndexList[static_cast<size_t>( idx )];
            if ( iChild1 < 0 )
            {
                CollectTriangles( ( -iChild1 ) - 1, Triangles );
            }
            else
            {
                CollectTriangles( iChild1 - 1, Triangles );
                CollectTriangles( Tree.IndexList[static_cast<size_t>( idx + 1 )] - 1, Triangles );
            }
        }
    };
} // namespace Desert::Geometry
