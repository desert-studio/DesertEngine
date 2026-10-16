#pragma once

// A BOUNDING-VOLUME HIERARCHY OVER A TRIANGLE SOUP, for the offline mesh builders that UE runs against an Embree
// scene: the distance field's sign rays (Assets/MeshDistanceFieldBuild.cpp) and the card build's surfel rays
// (Assets/MeshCardBuild.cpp). Median split on the widest centroid axis, up to kLeafTriangles per leaf; closest
// hit only, either side of a triangle (the caller decides what a back face means, as UE does with the hit
// normal). Double precision: the builders' inputs are centimetres over meshes up to kilometres long.
//
// One home: lifted out of the distance-field builder (GLOBAL-SDF) when the card build needed the same rays with
// UE's two extra Embree arguments - a ray start distance (tnear) and one primitive to skip (SkipPrimId, the
// triangle the previous hit of a ray walking through the mesh stopped on).

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace Desert::Geometry
{
    class TriangleBvh
    {
    public:
        static constexpr int kLeafTriangles = 4;

        struct Hit
        {
            int    Triangle = -1; // -1: no hit
            double T        = 0.0;
        };

        // Per triangle: vertex a, b - a, c - a and the unnormalised normal (b - a) x (c - a) (front = this side).
        std::vector<glm::dvec3> A, E1, E2, Normal;

        [[nodiscard]] static TriangleBvh Build( const std::vector<glm::dvec3>& positions,
                                                const std::vector<glm::uvec3>& triangles )
        {
            TriangleBvh  bvh;
            const size_t n = triangles.size();
            bvh.A.resize( n );
            bvh.E1.resize( n );
            bvh.E2.resize( n );
            bvh.Normal.resize( n );
            bvh.m_Order.resize( n );
            std::vector<glm::dvec3> centroids( n );
            for ( size_t t = 0; t < n; ++t )
            {
                const glm::uvec3& tri = triangles[t];
                bvh.A[t]              = positions[tri.x];
                bvh.E1[t]             = positions[tri.y] - bvh.A[t];
                bvh.E2[t]             = positions[tri.z] - bvh.A[t];
                bvh.Normal[t]         = glm::cross( bvh.E1[t], bvh.E2[t] );
                centroids[t]          = bvh.A[t] + ( bvh.E1[t] + bvh.E2[t] ) / 3.0;
                bvh.m_Order[t]        = static_cast<int>( t );
            }
            bvh.m_Nodes.reserve( 2 * n / kLeafTriangles + 2 );
            bvh.m_Nodes.resize( 1 );
            if ( n > 0 )
                bvh.Split( centroids, 0, 0, static_cast<int>( n ) );
            bvh.m_Packed.resize( n * 9 );
            for ( size_t i = 0; i < n; ++i )
            {
                const int t = bvh.m_Order[i];
                for ( int c = 0; c < 3; ++c )
                {
                    bvh.m_Packed[i * 9 + c]     = bvh.A[t][c];
                    bvh.m_Packed[i * 9 + 3 + c] = bvh.E1[t][c];
                    bvh.m_Packed[i * 9 + 6 + c] = bvh.E2[t][c];
                }
            }
            return bvh;
        }

        [[nodiscard]] size_t TriangleCount() const
        {
            return A.size();
        }

        // The closest triangle the ray o + t d, t in [tNear, tMax], crosses from either side (Moller-Trumbore),
        // skipping triangle @p skip (-1: none).
        [[nodiscard]] Hit ClosestHit( const double o[3], const double d[3], const double tNear, const double tMax,
                                      const int skip = -1 ) const
        {
            Hit hit;
            if ( A.empty() )
                return hit;
            const double inv[3] = { 1.0 / d[0], 1.0 / d[1], 1.0 / d[2] };
            double       best   = tMax;
            int          stack[64];
            int          top = 0;
            if ( BoxEntry( m_Nodes[0], o, inv, best ) < 0.0 )
                return hit;
            stack[top++] = 0;
            while ( top > 0 )
            {
                const Node& node = m_Nodes[stack[--top]];
                if ( node.Count > 0 )
                {
                    const double* tri = m_Packed.data() + static_cast<size_t>( node.First ) * 9;
                    for ( int i = 0; i < node.Count; ++i, tri += 9 )
                    {
                        const int index = m_Order[node.First + i];
                        if ( index == skip )
                            continue;
                        const double* a  = tri;
                        const double* e1 = tri + 3;
                        const double* e2 = tri + 6;
                        // p = d x e2, det = e1 . p
                        const double px  = d[1] * e2[2] - d[2] * e2[1];
                        const double py  = d[2] * e2[0] - d[0] * e2[2];
                        const double pz  = d[0] * e2[1] - d[1] * e2[0];
                        const double det = e1[0] * px + e1[1] * py + e1[2] * pz;
                        if ( det == 0.0 )
                            continue;
                        const double invDet = 1.0 / det;
                        const double sx = o[0] - a[0], sy = o[1] - a[1], sz = o[2] - a[2];
                        const double u = ( sx * px + sy * py + sz * pz ) * invDet;
                        if ( u < 0.0 || u > 1.0 )
                            continue;
                        // q = s x e1
                        const double qx = sy * e1[2] - sz * e1[1];
                        const double qy = sz * e1[0] - sx * e1[2];
                        const double qz = sx * e1[1] - sy * e1[0];
                        const double v  = ( d[0] * qx + d[1] * qy + d[2] * qz ) * invDet;
                        if ( v < 0.0 || u + v > 1.0 )
                            continue;
                        const double tHit = ( e2[0] * qx + e2[1] * qy + e2[2] * qz ) * invDet;
                        if ( tHit >= tNear && tHit <= best )
                        {
                            best         = tHit;
                            hit.Triangle = index;
                            hit.T        = tHit;
                        }
                    }
                    continue;
                }
                const double l = BoxEntry( m_Nodes[node.First], o, inv, best );
                const double r = BoxEntry( m_Nodes[node.First + 1], o, inv, best );
                // Nearer child on top of the stack, so a close hit shrinks the farther child's test.
                if ( l >= 0.0 && r >= 0.0 )
                {
                    stack[top++] = l <= r ? node.First + 1 : node.First;
                    stack[top++] = l <= r ? node.First : node.First + 1;
                }
                else if ( l >= 0.0 )
                    stack[top++] = node.First;
                else if ( r >= 0.0 )
                    stack[top++] = node.First + 1;
            }
            return hit;
        }

    private:
        struct Node
        {
            double Lo[3];
            double Hi[3];
            int    First = 0; // leaf: first entry in m_Order; inner: index of the left child (right = left + 1)
            int    Count = 0; // > 0: leaf
        };

        std::vector<Node> m_Nodes;
        std::vector<int>  m_Order;
        // The leaves' triangles in leaf order, 9 doubles each (a, b - a, c - a): the ray loop reads them as plain
        // scalars - it runs ~100 times per sample and the editor's builds are unoptimised.
        std::vector<double> m_Packed;

        void Split( const std::vector<glm::dvec3>& centroids, const int node, const int first, const int count )
        {
            glm::dvec3 lo( std::numeric_limits<double>::max() ), hi( std::numeric_limits<double>::lowest() );
            glm::dvec3 clo = lo, chi = hi;
            for ( int i = first; i < first + count; ++i )
            {
                const int t = m_Order[i];
                for ( const glm::dvec3& p : { A[t], A[t] + E1[t], A[t] + E2[t] } )
                {
                    lo = glm::min( lo, p );
                    hi = glm::max( hi, p );
                }
                clo = glm::min( clo, centroids[t] );
                chi = glm::max( chi, centroids[t] );
            }
            for ( int c = 0; c < 3; ++c )
            {
                m_Nodes[node].Lo[c] = lo[c];
                m_Nodes[node].Hi[c] = hi[c];
            }
            const glm::dvec3 spread = chi - clo;
            const int axis = spread.x >= spread.y && spread.x >= spread.z ? 0 : ( spread.y >= spread.z ? 1 : 2 );
            if ( count <= kLeafTriangles || spread[axis] <= 0.0 )
            {
                m_Nodes[node].First = first;
                m_Nodes[node].Count = count;
                return;
            }
            const int half = count / 2;
            std::nth_element( m_Order.begin() + first, m_Order.begin() + first + half,
                              m_Order.begin() + first + count, [&]( const int l, const int r )
                              { return centroids[l][axis] < centroids[r][axis]; } );
            const int left = static_cast<int>( m_Nodes.size() );
            m_Nodes.resize( m_Nodes.size() + 2 );
            m_Nodes[node].First = left;
            m_Nodes[node].Count = 0;
            Split( centroids, left, first, half );
            Split( centroids, left + 1, first + half, count - half );
        }

        // Slab test: the entry distance of the ray into the node's box, or a negative value when it misses within
        // [0, tMax].
        static double BoxEntry( const Node& node, const double o[3], const double inv[3], const double tMax )
        {
            double t0 = 0.0, t1 = tMax;
            for ( int c = 0; c < 3; ++c )
            {
                const double l = ( node.Lo[c] - o[c] ) * inv[c];
                const double h = ( node.Hi[c] - o[c] ) * inv[c];
                const double n = l < h ? l : h;
                const double f = l < h ? h : l;
                t0             = n > t0 ? n : t0;
                t1             = f < t1 ? f : t1;
                if ( t0 > t1 )
                    return -1.0;
            }
            return t0;
        }
    };
} // namespace Desert::Geometry
