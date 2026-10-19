// Ported from UE 5.8 Engine/Source/Runtime/Renderer/Private/Nanite/TessellationTable.cpp:25-458 (pattern
// generator: Uniform, Remesh, ProcessEdge, AddPatch, SnapAtEdges) and
// Engine/Shaders/Private/Nanite/NaniteSplit.usf:228-330 + NaniteTessellation.ush:140-200 (recursive split, pattern
// rotation/flip), adapted: the table is generated at first use instead of loaded from
// Content/Renderer/TessellationTable.bin, only the regular table is built (the immediate table and its
// cache-window ordering serve UE's hardware rasteriser), and the split runs on the CPU over cooked clusters with
// per-cluster levels instead of per view on the GPU, so vertex and triangle counts are exact.
#include "DisplacementTessellation.hpp"

#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <tuple>
#include <unordered_map>

namespace Desert::Geometry
{
    namespace
    {
        // ------------------------------------------------------------------------------------------------------
        // Pattern generation (UE FTessellatedPatch / FTessellationTable).

        // UE Barycentric::LengthSquared: the squared length of the segment between two barycentric points of a
        // triangle whose edge i (corner i -> corner i+1) has squared length edgeLengthsSquared[i].
        float BarycentricLengthSquared( const glm::vec3& a, const glm::vec3& b,
                                        const glm::vec3& edgeLengthsSquared )
        {
            const glm::vec3 d  = b - a;
            const float     t0 = d.x * d.y * edgeLengthsSquared.x;
            const float     t1 = d.y * d.z * edgeLengthsSquared.y;
            const float     t2 = d.z * d.x * edgeLengthsSquared.z;
            return -( t0 + t1 + t2 );
        }

        uint32_t Max3Index( int a, int b, int c )
        {
            return ( a > b ) ? ( ( a > c ) ? 0u : 2u ) : ( ( b > c ) ? 1u : 2u );
        }

        int RoundToInt( double value )
        {
            return static_cast<int>( std::floor( value + 0.5 ) );
        }

        class PatchGenerator
        {
        public:
            explicit PatchGenerator( const glm::ivec3& tessFactors ) : m_TessFactors( tessFactors )
            {
                if ( m_TessFactors.x == m_TessFactors.z )
                    Uniform();
                else
                    Remesh();
            }

            DynamicMesh3 Mesh;

        private:
            void Uniform()
            {
                const uint32_t factor   = static_cast<uint32_t>( m_TessFactors.x );
                const uint32_t numVerts = ( ( factor + 1 ) * ( factor + 2 ) ) / 2;
                const uint32_t numTris  = factor * factor;
                // Starts from the top point; adds rows of verts and the corresponding rows of triangle strips.
                for ( uint32_t vertIndex = 0; vertIndex < numVerts; ++vertIndex )
                {
                    const uint32_t vertRow = static_cast<uint32_t>(
                         std::floor( std::sqrt( static_cast<float>( vertIndex ) * 8.0f + 1.0f ) * 0.5f - 0.5f ) );
                    const uint32_t vertCol = vertIndex - ( vertRow * ( vertRow + 1 ) ) / 2;
                    glm::dvec3     barycentrics( static_cast<double>( factor - vertRow ),
                                                 static_cast<double>( vertCol ),
                                                 static_cast<double>( vertRow - vertCol ) );
                    barycentrics /= static_cast<double>( factor );
                    Mesh.AppendVertex( barycentrics );
                }
                for ( uint32_t triIndex = 0; triIndex < numTris; ++triIndex )
                {
                    const uint32_t triRow =
                         static_cast<uint32_t>( std::floor( std::sqrt( static_cast<float>( triIndex ) ) ) );
                    const uint32_t triCol       = triIndex - triRow * triRow;
                    const uint32_t flipTri      = triCol & 1u;
                    const uint32_t vertCol      = triCol >> 1;
                    uint32_t       rowCol[3][2] = {
                         { triRow, vertCol }, { triRow + 1, vertCol + 1 }, { triRow + 1, vertCol } };
                    rowCol[1][0] -= flipTri;
                    rowCol[2][1] += flipTri;
                    int corners[3];
                    for ( int c = 0; c < 3; ++c )
                    {
                        const uint32_t row = rowCol[c][0];
                        const uint32_t col = rowCol[c][1];
                        corners[c]         = static_cast<int>( col + ( row * ( row + 1 ) ) / 2 );
                    }
                    Mesh.AppendTriangle( corners[0], corners[1], corners[2] );
                }
            }

            void Remesh()
            {
                const int v0 = Mesh.AppendVertex( glm::dvec3( 1, 0, 0 ) );
                const int v1 = Mesh.AppendVertex( glm::dvec3( 0, 1, 0 ) );
                const int v2 = Mesh.AppendVertex( glm::dvec3( 0, 0, 1 ) );
                Mesh.AppendTriangle( v0, v1, v2 );

                constexpr int kRemeshIterations = 64;
                constexpr int kModuloPrime      = 31337; // iterate edges in a scrambled order
                for ( int k = 0; k < kRemeshIterations; ++k )
                {
                    const int maxEdgeId  = Mesh.MaxEdgeID();
                    const int startIndex = k % maxEdgeId;
                    int       edgeIndex  = startIndex;
                    do
                    {
                        if ( Mesh.IsEdge( edgeIndex ) )
                            ProcessEdge( edgeIndex );
                        edgeIndex = ( edgeIndex + kModuloPrime ) % maxEdgeId;
                    } while ( edgeIndex != startIndex );

                    m_Relaxed.resize( static_cast<size_t>( Mesh.MaxVertexID() ) );
                    for ( const int vertexIndex : Mesh.VertexIndicesItr() )
                    {
                        glm::dvec3 v = Mesh.GetVertex( vertexIndex );
                        if ( !Mesh.IsBoundaryVertex( vertexIndex ) )
                            Mesh.GetVtxOneRingCentroid( vertexIndex, v );
                        m_Relaxed[static_cast<size_t>( vertexIndex )] = v;
                    }
                    for ( const int vertexIndex : Mesh.VertexIndicesItr() )
                        Mesh.SetVertex( vertexIndex, m_Relaxed[static_cast<size_t>( vertexIndex )] );
                }
            }

            void ProcessEdge( int edgeIndex )
            {
                // MinEdgeLength < MaxEdgeLength / 2
                constexpr float kMinEdgeLength = 0.66f;
                constexpr float kMaxEdgeLength = 1.33f;

                const DynamicMesh3::Edge edge = Mesh.GetEdge( edgeIndex );
                const glm::vec3          p0( Mesh.GetVertex( edge.Vert.A ) );
                const glm::vec3          p1( Mesh.GetVertex( edge.Vert.B ) );
                const glm::vec3          lengthsSquared( m_TessFactors * m_TessFactors );
                const float              edgeLengthSqr = BarycentricLengthSquared( p0, p1, lengthsSquared );

                const bool boundary0    = Mesh.IsBoundaryVertex( edge.Vert.A );
                const bool boundary1    = Mesh.IsBoundaryVertex( edge.Vert.B );
                const bool boundaryEdge = Mesh.IsBoundaryEdge( edgeIndex );

                if ( ( boundaryEdge || !boundary0 || !boundary1 ) &&
                     edgeLengthSqr < kMinEdgeLength * kMinEdgeLength )
                {
                    int    keep      = edge.Vert.A;
                    int    remove    = edge.Vert.B;
                    double collapseT = 0.5;
                    if ( boundary0 )
                    {
                        keep      = edge.Vert.A;
                        remove    = edge.Vert.B;
                        collapseT = 0;
                    }
                    if ( boundary1 )
                    {
                        keep      = edge.Vert.B;
                        remove    = edge.Vert.A;
                        collapseT = 0;
                    }
                    DynamicMesh3::EdgeCollapseInfo collapseInfo;
                    if ( Mesh.CollapseEdge( keep, remove, collapseT, collapseInfo ) == MeshResult::Ok )
                        return;
                }

                if ( !boundaryEdge )
                {
                    const Index2i   opposing = Mesh.GetEdgeOpposingV( edgeIndex );
                    const glm::vec3 o0( Mesh.GetVertex( opposing.A ) );
                    const glm::vec3 o1( Mesh.GetVertex( opposing.B ) );
                    const float     flipDistSqr = BarycentricLengthSquared( o0, o1, lengthsSquared );
                    if ( flipDistSqr < 0.9f * edgeLengthSqr )
                    {
                        DynamicMesh3::EdgeFlipInfo flipInfo;
                        if ( Mesh.FlipEdge( edgeIndex, flipInfo ) == MeshResult::Ok )
                            return;
                    }
                }

                if ( edgeLengthSqr > kMaxEdgeLength * kMaxEdgeLength )
                {
                    float splitT = 0.5f;
                    if ( boundaryEdge )
                    {
                        const float edgeLength    = std::sqrt( edgeLengthSqr );
                        const float numEdgeSplits = std::floor( edgeLength + 0.5f );
                        const float halfSplit     = std::floor( numEdgeSplits / 2.0f );
                        splitT                    = halfSplit / numEdgeSplits;
                    }
                    DynamicMesh3::EdgeSplitInfo splitInfo;
                    (void)Mesh.SplitEdge( edgeIndex, splitInfo, static_cast<double>( splitT ) );
                }
            }

            glm::ivec3              m_TessFactors;
            std::vector<glm::dvec3> m_Relaxed;
        };

        // UE FTessellationTable::SnapAtEdges: a vertex on edge i lands on a multiple of 1/TessFactor[i], snapped
        // toward the SMALLER barycentric so the two triangles sharing the edge mirror each other exactly.
        void SnapAtEdges( std::array<uint32_t, 3>& barycentrics, const glm::ivec3& tessFactors )
        {
            for ( uint32_t i = 0; i < 3; ++i )
            {
                const uint32_t e0 = i;
                const uint32_t e1 = ( 1u << e0 ) & 3u;
                if ( barycentrics[e0] + barycentrics[e1] != kBarycentricMax )
                    continue;
                const uint32_t minIndex = barycentrics[e0] < barycentrics[e1] ? e0 : e1;
                const uint32_t maxIndex = barycentrics[e0] >= barycentrics[e1] ? e0 : e1;
                const uint32_t factor   = static_cast<uint32_t>( tessFactors[static_cast<int>( i )] );
                const uint32_t snapped =
                     ( barycentrics[minIndex] * factor + ( kBarycentricMax / 2 ) - 1 ) & ~( kBarycentricMax - 1 );
                barycentrics[minIndex] = snapped / factor;
                barycentrics[maxIndex] = kBarycentricMax - barycentrics[minIndex];
            }
        }

        void AddPatch( TessellationTable& table, const glm::ivec3& tessFactors )
        {
            PatchGenerator patch( tessFactors );
            patch.Mesh.CompactInPlace();

            TessellationPattern& pattern = table.Patterns[TessellationTable::PatternIndex(
                 static_cast<uint32_t>( tessFactors.x ), static_cast<uint32_t>( tessFactors.y ),
                 static_cast<uint32_t>( tessFactors.z ) )];
            pattern.VertexOffset   = static_cast<uint32_t>( table.Verts.size() );
            pattern.TriangleOffset = static_cast<uint32_t>( table.Triangles.size() );

            for ( const int vertexIndex : patch.Mesh.VertexIndicesItr() )
            {
                const glm::dvec3   v = patch.Mesh.GetVertex( vertexIndex );
                std::array<int, 3> rounded{};
                for ( int c = 0; c < 3; ++c )
                    rounded[static_cast<size_t>( c )] =
                         RoundToInt( std::clamp( v[c], 0.0, 1.0 ) * static_cast<double>( kBarycentricMax ) );
                const uint32_t e0 = Max3Index( rounded[0], rounded[1], rounded[2] );
                const uint32_t e1 = ( 1u << e0 ) & 3u;
                const uint32_t e2 = ( 1u << e1 ) & 3u;
                rounded[e0]       = static_cast<int>( kBarycentricMax ) - rounded[e1] - rounded[e2];

                std::array<uint32_t, 3> barycentrics = { static_cast<uint32_t>( rounded[0] ),
                                                         static_cast<uint32_t>( rounded[1] ),
                                                         static_cast<uint32_t>( rounded[2] ) };
                SnapAtEdges( barycentrics, tessFactors );
                table.Verts.push_back( barycentrics[0] | ( barycentrics[1] << 16 ) );
            }
            for ( const int triangleId : patch.Mesh.TriangleIndicesItr() )
            {
                const Index3i tri = patch.Mesh.GetTriangle( triangleId );
                table.Triangles.push_back( { static_cast<uint16_t>( tri.A ), static_cast<uint16_t>( tri.B ),
                                             static_cast<uint16_t>( tri.C ) } );
            }
            pattern.Vertices  = static_cast<uint32_t>( table.Verts.size() ) - pattern.VertexOffset;
            pattern.Triangles = static_cast<uint32_t>( table.Triangles.size() ) - pattern.TriangleOffset;
        }

        TessellationTable BuildTable()
        {
            TessellationTable table;
            const size_t slots = static_cast<size_t>( kTessellationTablePo2Size ) * kTessellationTablePo2Size *
                                 kTessellationTablePo2Size;
            table.Patterns.resize( slots );
            // Factors in descending order (X >= Y >= Z): the table holds each unordered triple once.
            for ( uint32_t z = 1; z <= kMaxEdgeFactor; ++z )
                for ( uint32_t y = z; y <= kMaxEdgeFactor; ++y )
                    for ( uint32_t x = y; x <= kMaxEdgeFactor; ++x )
                        AddPatch( table, glm::ivec3( static_cast<int>( x ), static_cast<int>( y ),
                                                     static_cast<int>( z ) ) );
            return table;
        }

        // ------------------------------------------------------------------------------------------------------
        // Topology.

        struct WeldKey
        {
            uint32_t Group;
            uint32_t X, Y, Z;
            bool     operator==( const WeldKey& ) const = default;
        };
        struct WeldKeyHash
        {
            size_t operator()( const WeldKey& k ) const noexcept
            {
                uint64_t h = 1469598103934665603ull;
                for ( const uint32_t v : { k.Group, k.X, k.Y, k.Z } )
                {
                    h ^= v;
                    h *= 1099511628211ull;
                }
                return static_cast<size_t>( h );
            }
        };

        // Splits [begin, end) of @p tris at the centroid median of the longest axis until each piece fits a
        // cluster. Median split keeps clusters spatially compact and deterministic; UE partitions a triangle graph
        // with METIS (FGraphPartitioner), which is not vendored here - see REMAINDER-DISPLACE.
        void PartitionClusters( std::vector<uint32_t>& tris, size_t begin, size_t end,
                                const std::vector<glm::vec3>&           centroids,
                                std::vector<std::pair<size_t, size_t>>& out )
        {
            if ( end - begin <= kDisplacementClusterTriangles )
            {
                out.emplace_back( begin, end );
                return;
            }
            glm::vec3 lo( std::numeric_limits<float>::max() );
            glm::vec3 hi( std::numeric_limits<float>::lowest() );
            for ( size_t i = begin; i < end; ++i )
            {
                lo = glm::min( lo, centroids[tris[i]] );
                hi = glm::max( hi, centroids[tris[i]] );
            }
            const glm::vec3 extent = hi - lo;
            const int       axis =
                 extent.x >= extent.y ? ( extent.x >= extent.z ? 0 : 2 ) : ( extent.y >= extent.z ? 1 : 2 );
            const size_t middle = begin + ( end - begin ) / 2;
            std::nth_element( tris.begin() + static_cast<std::ptrdiff_t>( begin ),
                              tris.begin() + static_cast<std::ptrdiff_t>( middle ),
                              tris.begin() + static_cast<std::ptrdiff_t>( end ),
                              [&]( uint32_t a, uint32_t b )
                              {
                                  const float ca = centroids[a][axis];
                                  const float cb = centroids[b][axis];
                                  return ca != cb ? ca < cb : a < b;
                              } );
            PartitionClusters( tris, begin, middle, centroids, out );
            PartitionClusters( tris, middle, end, centroids, out );
        }

        uint32_t FloatBits( float v )
        {
            // -0 and +0 are one position.
            return v == 0.0f ? 0u : std::bit_cast<uint32_t>( v );
        }

        float TargetEdgeCm( const DisplacementTopology& topology, uint32_t level )
        {
            return std::ldexp( topology.BaseEdgeCm, -static_cast<int>( level ) );
        }

        // ------------------------------------------------------------------------------------------------------
        // Binary helpers.

        constexpr char     kTopologyMagic[8]      = { 'D', 'E', 'S', 'T', 'D', 'S', 'P', 'T' };
        constexpr uint32_t kTopologyFormatVersion = 1;

        template <typename T>
        void WritePod( std::vector<uint8_t>& out, const T& value )
        {
            const auto* p = reinterpret_cast<const uint8_t*>( &value );
            out.insert( out.end(), p, p + sizeof( T ) );
        }
        template <typename T>
        void WriteVector( std::vector<uint8_t>& out, const std::vector<T>& values )
        {
            WritePod( out, static_cast<uint64_t>( values.size() ) );
            const auto* p = reinterpret_cast<const uint8_t*>( values.data() );
            out.insert( out.end(), p, p + values.size() * sizeof( T ) );
        }

        struct Reader
        {
            std::span<const uint8_t> Data;
            size_t                   Cursor = 0;
            bool                     Failed = false;

            template <typename T>
            T Pod()
            {
                T value{};
                if ( Cursor + sizeof( T ) > Data.size() )
                {
                    Failed = true;
                    return value;
                }
                std::memcpy( &value, Data.data() + Cursor, sizeof( T ) );
                Cursor += sizeof( T );
                return value;
            }
            template <typename T>
            std::vector<T> Vector()
            {
                const uint64_t count = Pod<uint64_t>();
                std::vector<T> values;
                if ( Failed || count > ( Data.size() - Cursor ) / sizeof( T ) )
                {
                    Failed = true;
                    return values;
                }
                values.resize( static_cast<size_t>( count ) );
                std::memcpy( values.data(), Data.data() + Cursor, static_cast<size_t>( count ) * sizeof( T ) );
                Cursor += static_cast<size_t>( count ) * sizeof( T );
                return values;
            }
        };

        // ------------------------------------------------------------------------------------------------------
        // Cluster tessellation.

        using Barycentric = std::array<uint32_t, 3>; // fixed point in the SOURCE triangle's corner order

        class ClusterTessellator
        {
        public:
            ClusterTessellator( const DisplacementTopology& topology, uint32_t cluster,
                                std::span<const uint8_t> levels )
                 : m_Topology( topology ), m_Cluster( cluster ), m_Levels( levels ),
                   m_Table( GetTessellationTable() )
            {
            }

            Common::ResultStr<TessellatedCluster> Run()
            {
                const DisplacementCluster& cluster = m_Topology.Clusters[m_Cluster];
                for ( const uint32_t tri : cluster.Triangles )
                {
                    m_Triangle = tri;
                    m_TriangleVertices.clear();
                    const Barycentric c0 = { kBarycentricMax, 0, 0 };
                    const Barycentric c1 = { 0, kBarycentricMax, 0 };
                    const Barycentric c2 = { 0, 0, kBarycentricMax };
                    if ( auto split = Process( { c0, c1, c2 }, 0 ); !split.IsSuccess() )
                        return Common::MakeError<TessellatedCluster>( split.GetError() );
                }
                return Common::MakeSuccess( std::move( m_Out ) );
            }

        private:
            static constexpr uint32_t kMaxSplitDepth = 8; // 8^8 children per edge: beyond every reachable factor

            uint32_t Weld( uint32_t corner ) const
            {
                return m_Topology.CornerWeld[3 * m_Triangle + corner];
            }

            // Object-space position of a source barycentric, summed over welded vertices in ascending id order so
            // the two triangles of a welded edge compute bit-identical positions for its points.
            glm::vec3 Position( const Barycentric& b ) const
            {
                std::array<std::pair<uint32_t, uint32_t>, 3> terms = {
                     std::pair{ Weld( 0 ), b[0] }, std::pair{ Weld( 1 ), b[1] }, std::pair{ Weld( 2 ), b[2] } };
                std::sort( terms.begin(), terms.end() );
                glm::vec3 p( 0.0f );
                for ( const auto& [weld, weight] : terms )
                {
                    if ( weight == 0 )
                        continue;
                    const float     w    = static_cast<float>( weight ) / static_cast<float>( kBarycentricMax );
                    const glm::vec3 term = m_Topology.WeldPosition[weld] * w;
                    p                    = p + term;
                }
                return p;
            }

            // The level an edge between two patch corners dices at: a segment of source edge k (both corners have
            // a zero barycentric k) uses that welded edge's level, anything inside the triangle the cluster's.
            uint32_t EdgeLevel( const Barycentric& a, const Barycentric& b ) const
            {
                for ( uint32_t k = 0; k < 3; ++k )
                {
                    if ( a[k] != 0 || b[k] != 0 )
                        continue;
                    const uint32_t edge  = m_Topology.CornerEdge[3 * m_Triangle + ( k + 1 ) % 3];
                    uint32_t       level = m_Levels[m_Cluster];
                    for ( uint32_t i = m_Topology.EdgeTriangleOffset[edge];
                          i < m_Topology.EdgeTriangleOffset[edge + 1]; ++i )
                        level = std::max<uint32_t>(
                             level, m_Levels[m_Topology.TriangleCluster[m_Topology.EdgeTriangles[i]]] );
                    return level;
                }
                return m_Levels[m_Cluster];
            }

            uint32_t EdgeFactor( const Barycentric& a, const Barycentric& b ) const
            {
                // Measured from the corner with the lexicographically smaller canonical form so the factor is
                // symmetric in its endpoints (|a-b| == |b-a| holds in IEEE arithmetic; the order pins it anyway).
                const glm::vec3 pa     = Position( a );
                const glm::vec3 pb     = Position( b );
                const glm::vec3 d      = pa - pb;
                const float     length = std::sqrt( d.x * d.x + d.y * d.y + d.z * d.z );
                const float     target = TargetEdgeCm( m_Topology, EdgeLevel( a, b ) );
                const float     factor = std::ceil( length / target );
                return factor < 1.0f ? 1u : static_cast<uint32_t>( factor );
            }

            // Ties resolve toward the corner with the larger welded id: canonical, so siblings and neighbours
            // agree.
            void Normalise( Barycentric& b ) const
            {
                uint32_t largest = 0;
                for ( uint32_t k = 1; k < 3; ++k )
                    if ( b[k] > b[largest] || ( b[k] == b[largest] && Weld( k ) > Weld( largest ) ) )
                        largest = k;
                const uint32_t others = b[( largest + 1 ) % 3] + b[( largest + 2 ) % 3];
                b[largest]            = kBarycentricMax - others;
            }

            Barycentric Transform( uint32_t encoded, const std::array<Barycentric, 3>& corners ) const
            {
                const uint64_t w0 = encoded & 0xffffu;
                const uint64_t w1 = encoded >> 16;
                const uint64_t w2 = kBarycentricMax - w0 - w1;
                Barycentric    b{};
                for ( uint32_t k = 0; k < 3; ++k )
                {
                    const uint64_t sum = w0 * corners[0][k] + w1 * corners[1][k] + w2 * corners[2][k];
                    b[k]               = static_cast<uint32_t>( ( sum + kBarycentricMax / 2 ) / kBarycentricMax );
                }
                Normalise( b );
                return b;
            }

            Common::BoolResultStr Process( std::array<Barycentric, 3> corners, uint32_t depth )
            {
                if ( depth > kMaxSplitDepth )
                    return Common::MakeFormattedError<bool>(
                         "displacement: triangle {} of cluster {} still needs splitting after {} rounds",
                         m_Triangle, m_Cluster, kMaxSplitDepth );

                std::array<uint32_t, 3> factors = { EdgeFactor( corners[0], corners[1] ),
                                                    EdgeFactor( corners[1], corners[2] ),
                                                    EdgeFactor( corners[2], corners[0] ) };
                const bool needsSplitting = std::max( { factors[0], factors[1], factors[2] } ) > kMaxEdgeFactor;
                if ( needsSplitting )
                    for ( uint32_t& f : factors )
                        f = std::min( ( f + kMaxEdgeFactor - 1 ) / kMaxEdgeFactor, kMaxSplitFactor );

                // UE FTessellatedPatch::Init: rotate so factor x is the largest, then order y >= z (which flips
                // the winding, undone on the indices).
                const uint32_t maxFactor = std::max( { factors[0], factors[1], factors[2] } );
                if ( factors[1] == maxFactor )
                {
                    corners = { corners[1], corners[2], corners[0] };
                    factors = { factors[1], factors[2], factors[0] };
                }
                else if ( factors[2] == maxFactor )
                {
                    corners = { corners[2], corners[0], corners[1] };
                    factors = { factors[2], factors[0], factors[1] };
                }
                bool flipWinding = false;
                if ( factors[1] < factors[2] )
                {
                    std::swap( corners[0], corners[1] );
                    std::swap( factors[1], factors[2] );
                    flipWinding = true;
                }
                // Swapping corners 0 and 1 swaps edges 1 and 2, hence factors y and z (UE: VertData.xy, TessFactors.yz).

                const uint32_t patternIndex =
                     TessellationTable::PatternIndex( factors[0], factors[1], factors[2] );
                const TessellationPattern& pattern   = m_Table.Patterns[patternIndex];

                for ( uint32_t t = 0; t < pattern.Triangles; ++t )
                {
                    std::array<uint16_t, 3> local = m_Table.Triangles[pattern.TriangleOffset + t];
                    if ( flipWinding )
                        std::swap( local[1], local[2] );
                    std::array<Barycentric, 3> child{};
                    for ( uint32_t i = 0; i < 3; ++i )
                        child[i] = Transform( m_Table.Verts[pattern.VertexOffset + local[i]], corners );

                    if ( !needsSplitting )
                    {
                        for ( uint32_t i = 0; i < 3; ++i )
                            m_Out.Indices.push_back( VertexFor( child[i] ) );
                        continue;
                    }
                    // Do not recurse into degenerates or into the same patch again (NaniteSplit.usf RunChild).
                    if ( child[0] == child[1] || child[1] == child[2] || child[2] == child[0] ||
                         ( child[0] == corners[0] && child[1] == corners[1] && child[2] == corners[2] ) )
                        continue;
                    if ( auto r = Process( child, depth + 1 ); !r.IsSuccess() )
                        return r;
                }
                return Common::MakeSuccess( true );
            }

            uint32_t VertexFor( const Barycentric& b )
            {
                const uint32_t packed = b[0] | ( b[1] << 16 );
                if ( const auto it = m_TriangleVertices.find( packed ); it != m_TriangleVertices.end() )
                    return it->second;
                const uint32_t index = static_cast<uint32_t>( m_Out.Vertices.size() );
                m_Out.Vertices.push_back( { m_Triangle, packed, PointFor( b ) } );
                m_TriangleVertices.emplace( packed, index );
                return index;
            }

            uint32_t PointFor( const Barycentric& b )
            {
                // Canonical form: (weld, weight) pairs with weight > 0, welds merged and ascending.
                std::array<std::pair<uint32_t, uint32_t>, 3> terms{};
                uint32_t                                     count = 0;
                for ( uint32_t k = 0; k < 3; ++k )
                {
                    if ( b[k] == 0 )
                        continue;
                    bool merged = false;
                    for ( uint32_t i = 0; i < count; ++i )
                        if ( terms[i].first == Weld( k ) )
                        {
                            terms[i].second += b[k];
                            merged = true;
                        }
                    if ( !merged )
                        terms[count++] = { Weld( k ), b[k] };
                }
                std::sort( terms.begin(), terms.begin() + count );

                DisplacementPoint point;
                for ( uint32_t i = 0; i < 3; ++i )
                    point.Welds[i] = terms[std::min( i, count - 1 )].first;
                const uint32_t b0 = terms[0].second;
                const uint32_t b1 = count > 1 ? terms[1].second : 0u;
                point.Weights     = b0 | ( b1 << 16 );

                const auto key = std::make_tuple( point.Welds[0], point.Welds[1], point.Welds[2], point.Weights );
                if ( const auto it = m_Points.find( key ); it != m_Points.end() )
                    return it->second;

                point.SampleOffset = static_cast<uint32_t>( m_Out.Samples.size() );
                if ( count == 1 )
                    AddCornerSamples( point.Welds[0] );
                else if ( count == 2 )
                    AddEdgeSamples( terms[0], terms[1] );
                else
                    m_Out.Samples.push_back( { m_Triangle, b[0] | ( b[1] << 16 ) } );
                point.SampleCount = static_cast<uint32_t>( m_Out.Samples.size() ) - point.SampleOffset;
                point.Level       = PointLevel( b, count, point.Welds[0] );

                const uint32_t index = static_cast<uint32_t>( m_Out.Points.size() );
                m_Out.Points.push_back( point );
                m_Points.emplace( key, index );
                return index;
            }

            uint32_t PointLevel( const Barycentric& b, uint32_t count, uint32_t weld ) const
            {
                if ( count == 3 )
                    return m_Levels[m_Cluster];
                if ( count == 2 )
                {
                    // The two non-zero corners span the source edge opposite the zero one.
                    for ( uint32_t k = 0; k < 3; ++k )
                        if ( b[k] == 0 )
                        {
                            Barycentric a{}, c{};
                            a[( k + 1 ) % 3] = kBarycentricMax;
                            c[( k + 2 ) % 3] = kBarycentricMax;
                            return EdgeLevel( a, c );
                        }
                }
                uint32_t level = m_Levels[m_Cluster];
                for ( uint32_t i = m_Topology.WeldCornerOffset[weld]; i < m_Topology.WeldCornerOffset[weld + 1]; ++i )
                    level = std::max<uint32_t>( level, m_Levels[m_Topology.TriangleCluster[m_Topology.WeldCorners[i] / 3]] );
                return level;
            }

            static uint32_t Pack( const Barycentric& b )
            {
                return b[0] | ( b[1] << 16 );
            }

            // Every corner of every triangle welded to @p weld (ascending): one height for the whole vertex.
            void AddCornerSamples( uint32_t weld )
            {
                for ( uint32_t i = m_Topology.WeldCornerOffset[weld]; i < m_Topology.WeldCornerOffset[weld + 1];
                      ++i )
                {
                    const uint32_t corner = m_Topology.WeldCorners[i];
                    Barycentric    b{};
                    b[corner % 3] = kBarycentricMax;
                    m_Out.Samples.push_back( { corner / 3, Pack( b ) } );
                }
            }

            // Every triangle on the welded edge (ascending), at the same canonical point.
            void AddEdgeSamples( std::pair<uint32_t, uint32_t> a, std::pair<uint32_t, uint32_t> b )
            {
                uint32_t edge = 0;
                for ( uint32_t c = 0; c < 3; ++c )
                {
                    const uint32_t w0 = Weld( c );
                    const uint32_t w1 = Weld( ( c + 1 ) % 3 );
                    if ( ( w0 == a.first && w1 == b.first ) || ( w0 == b.first && w1 == a.first ) )
                        edge = m_Topology.CornerEdge[3 * m_Triangle + c];
                }
                for ( uint32_t i = m_Topology.EdgeTriangleOffset[edge];
                      i < m_Topology.EdgeTriangleOffset[edge + 1]; ++i )
                {
                    const uint32_t tri = m_Topology.EdgeTriangles[i];
                    Barycentric    bary{};
                    bool           placedA = false, placedB = false;
                    for ( uint32_t c = 0; c < 3; ++c )
                    {
                        const uint32_t w = m_Topology.CornerWeld[3 * tri + c];
                        if ( w == a.first && !placedA )
                        {
                            bary[c] = a.second;
                            placedA = true;
                        }
                        else if ( w == b.first && !placedB )
                        {
                            bary[c] = b.second;
                            placedB = true;
                        }
                    }
                    m_Out.Samples.push_back( { tri, Pack( bary ) } );
                }
            }

            const DisplacementTopology&                                            m_Topology;
            uint32_t                                                               m_Cluster;
            std::span<const uint8_t>                                               m_Levels;
            const TessellationTable&                                               m_Table;
            uint32_t                                                               m_Triangle = 0;
            TessellatedCluster                                                     m_Out;
            std::unordered_map<uint32_t, uint32_t>                                 m_TriangleVertices;
            std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, uint32_t> m_Points;
        };
    } // namespace

    uint32_t TessellationTable::PatternIndex( uint32_t x, uint32_t y, uint32_t z )
    {
        if ( x < y )
            std::swap( x, y );
        if ( x < z )
            std::swap( x, z );
        if ( y < z )
            std::swap( y, z );
        return ( x - 1 ) + ( y - 1 ) * kTessellationTablePo2Size +
               ( z - 1 ) * kTessellationTablePo2Size * kTessellationTablePo2Size;
    }

    const TessellationTable& GetTessellationTable()
    {
        static std::once_flag    once;
        static TessellationTable table;
        std::call_once( once, [] { table = BuildTable(); } );
        return table;
    }

    uint64_t DisplacementTopology::GetBytes() const
    {
        uint64_t bytes = 0;
        bytes += ( CornerVertex.size() + CornerWeld.size() + CornerEdge.size() + TriangleSubmesh.size() +
                   TriangleCluster.size() + WeldCornerOffset.size() + WeldCorners.size() +
                   EdgeTriangleOffset.size() + EdgeTriangles.size() ) *
                 sizeof( uint32_t );
        bytes += ( WeldPosition.size() + WeldDirection.size() ) * sizeof( glm::vec3 );
        bytes += EdgeWelds.size() * sizeof( std::array<uint32_t, 2> );
        for ( const DisplacementCluster& c : Clusters )
            bytes +=
                 sizeof( DisplacementCluster ) + ( c.Triangles.size() + c.Neighbours.size() ) * sizeof( uint32_t );
        return bytes;
    }

    Common::ResultStr<DisplacementTopology> BuildDisplacementTopology( std::span<const Vertex>  vertices,
                                                                       std::span<const Index>   indices,
                                                                       std::span<const Submesh> submeshes )
    {
        DisplacementTopology topology;

        // Positions are submesh-local: only submeshes drawn under the same Transform share a weld space.
        std::vector<uint32_t> transformGroup( submeshes.size() );
        for ( size_t s = 0; s < submeshes.size(); ++s )
        {
            transformGroup[s] = static_cast<uint32_t>( s );
            for ( size_t o = 0; o < s; ++o )
                if ( submeshes[o].Transform == submeshes[s].Transform )
                {
                    transformGroup[s] = transformGroup[o];
                    break;
                }
        }

        std::unordered_map<WeldKey, uint32_t, WeldKeyHash> welds;
        std::vector<uint32_t>                              vertexWeld( vertices.size(), UINT32_MAX );
        std::vector<glm::vec3>                             normalSum;
        for ( size_t s = 0; s < submeshes.size(); ++s )
        {
            const Submesh& sm          = submeshes[s];
            const uint32_t indexOffset = sm.LODs.empty() ? sm.IndexOffset : sm.LODs.front().IndexOffset;
            const uint32_t indexCount  = sm.LODs.empty() ? sm.IndexCount : sm.LODs.front().IndexCount;
            const uint32_t firstTri    = indexOffset / 3;
            const uint32_t triCount    = indexCount / 3;
            if ( firstTri + triCount > indices.size() )
                return Common::MakeFormattedError<DisplacementTopology>(
                     "displacement topology: submesh {} '{}' reads triangles {}..{} of {}", s, sm.Name, firstTri,
                     firstTri + triCount, indices.size() );
            for ( uint32_t t = firstTri; t < firstTri + triCount; ++t )
            {
                const std::array<uint32_t, 3> local = { indices[t].V1, indices[t].V2, indices[t].V3 };
                for ( const uint32_t l : local )
                {
                    const uint32_t v = sm.VertexOffset + l;
                    if ( l >= sm.VertexCount || v >= vertices.size() )
                        return Common::MakeFormattedError<DisplacementTopology>(
                             "displacement topology: submesh {} '{}' triangle {} reads vertex {} of {} (range "
                             "{}+{})",
                             s, sm.Name, t, l, vertices.size(), sm.VertexOffset, sm.VertexCount );
                    if ( vertexWeld[v] == UINT32_MAX )
                    {
                        const glm::vec3 p   = vertices[v].Position;
                        const WeldKey   key = { transformGroup[s], FloatBits( p.x ), FloatBits( p.y ),
                                                FloatBits( p.z ) };
                        auto [it, added] =
                             welds.try_emplace( key, static_cast<uint32_t>( topology.WeldPosition.size() ) );
                        if ( added )
                        {
                            topology.WeldPosition.push_back( p );
                            normalSum.emplace_back( 0.0f );
                        }
                        vertexWeld[v] = it->second;
                        // Each distinct mesh vertex (each copy) contributes its normal once.
                        normalSum[it->second] += vertices[v].Normal;
                    }
                    topology.CornerVertex.push_back( v );
                    topology.CornerWeld.push_back( vertexWeld[v] );
                }
                topology.TriangleSubmesh.push_back( static_cast<uint32_t>( s ) );
            }
        }
        const uint32_t triangleCount = static_cast<uint32_t>( topology.TriangleSubmesh.size() );
        if ( triangleCount == 0 )
            return Common::MakeError<DisplacementTopology>(
                 "displacement topology: the mesh has no LOD0 triangles" );

        topology.WeldDirection.resize( normalSum.size() );
        for ( size_t w = 0; w < normalSum.size(); ++w )
        {
            const float length = glm::length( normalSum[w] );
            // Copies whose normals cancel (a zero-thickness fold) have no direction to move in; displacing them
            // either way tears the surface, so the mesh is refused rather than displaced wrongly.
            if ( !( length > 1e-6f ) )
                return Common::MakeFormattedError<DisplacementTopology>(
                     "displacement topology: welded vertex {} at ({}, {}, {}) has copies whose normals sum to "
                     "zero",
                     w, topology.WeldPosition[w].x, topology.WeldPosition[w].y, topology.WeldPosition[w].z );
            topology.WeldDirection[w] = normalSum[w] / length;
        }

        // Welded edges.
        std::unordered_map<uint64_t, uint32_t> edges;
        topology.CornerEdge.resize( topology.CornerWeld.size() );
        for ( uint32_t t = 0; t < triangleCount; ++t )
            for ( uint32_t c = 0; c < 3; ++c )
            {
                uint32_t a = topology.CornerWeld[3 * t + c];
                uint32_t b = topology.CornerWeld[3 * t + ( c + 1 ) % 3];
                if ( b < a )
                    std::swap( a, b );
                const uint64_t key = ( static_cast<uint64_t>( a ) << 32 ) | b;
                auto [it, added]   = edges.try_emplace( key, static_cast<uint32_t>( topology.EdgeWelds.size() ) );
                if ( added )
                    topology.EdgeWelds.push_back( { a, b } );
                topology.CornerEdge[3 * t + c] = it->second;
            }

        // CSR: weld -> corners, edge -> triangles (ascending by construction: filled in triangle order).
        const auto buildCsr =
             []( size_t keys, const auto& forEach, std::vector<uint32_t>& offsets, std::vector<uint32_t>& values )
        {
            offsets.assign( keys + 1, 0 );
            forEach( [&]( uint32_t key, uint32_t ) { ++offsets[key + 1]; } );
            std::partial_sum( offsets.begin(), offsets.end(), offsets.begin() );
            values.resize( offsets.back() );
            std::vector<uint32_t> cursor( offsets.begin(), offsets.end() - 1 );
            forEach( [&]( uint32_t key, uint32_t value ) { values[cursor[key]++] = value; } );
        };
        buildCsr(
             topology.WeldPosition.size(),
             [&]( const auto& emit )
             {
                 for ( uint32_t corner = 0; corner < topology.CornerWeld.size(); ++corner )
                     emit( topology.CornerWeld[corner], corner );
             },
             topology.WeldCornerOffset, topology.WeldCorners );
        buildCsr(
             topology.EdgeWelds.size(),
             [&]( const auto& emit )
             {
                 for ( uint32_t t = 0; t < triangleCount; ++t )
                 {
                     std::array<uint32_t, 3> e = { topology.CornerEdge[3 * t], topology.CornerEdge[3 * t + 1],
                                                   topology.CornerEdge[3 * t + 2] };
                     std::sort( e.begin(), e.end() );
                     for ( uint32_t i = 0; i < 3; ++i )
                         if ( i == 0 || e[i] != e[i - 1] )
                             emit( e[i], t );
                 }
             },
             topology.EdgeTriangleOffset, topology.EdgeTriangles );

        // Level 0's target: the median welded edge length.
        std::vector<float> lengths( topology.EdgeWelds.size() );
        for ( size_t e = 0; e < lengths.size(); ++e )
            lengths[e] = glm::length( topology.WeldPosition[topology.EdgeWelds[e][1]] -
                                      topology.WeldPosition[topology.EdgeWelds[e][0]] );
        std::vector<float> sorted = lengths;
        std::nth_element( sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>( sorted.size() / 2 ),
                          sorted.end() );
        topology.BaseEdgeCm = sorted[sorted.size() / 2];
        if ( !( topology.BaseEdgeCm > 0.0f ) )
            return Common::MakeFormattedError<DisplacementTopology>(
                 "displacement topology: the median of {} welded edges has length {} cm", sorted.size(),
                 topology.BaseEdgeCm );

        // Clusters, per submesh.
        std::vector<glm::vec3> centroids( triangleCount );
        for ( uint32_t t = 0; t < triangleCount; ++t )
            centroids[t] = ( topology.WeldPosition[topology.CornerWeld[3 * t]] +
                             topology.WeldPosition[topology.CornerWeld[3 * t + 1]] +
                             topology.WeldPosition[topology.CornerWeld[3 * t + 2]] ) /
                           3.0f;
        topology.TriangleCluster.resize( triangleCount );
        uint32_t first = 0;
        while ( first < triangleCount )
        {
            const uint32_t submesh = topology.TriangleSubmesh[first];
            uint32_t       last    = first;
            while ( last < triangleCount && topology.TriangleSubmesh[last] == submesh )
                ++last;
            std::vector<uint32_t> tris( last - first );
            std::iota( tris.begin(), tris.end(), first );
            std::vector<std::pair<size_t, size_t>> ranges;
            PartitionClusters( tris, 0, tris.size(), centroids, ranges );
            for ( const auto& [b, e] : ranges )
            {
                DisplacementCluster cluster;
                cluster.Submesh = submesh;
                cluster.Triangles.assign( tris.begin() + static_cast<std::ptrdiff_t>( b ),
                                          tris.begin() + static_cast<std::ptrdiff_t>( e ) );
                std::sort( cluster.Triangles.begin(), cluster.Triangles.end() );
                cluster.BoundsMin = glm::vec3( std::numeric_limits<float>::max() );
                cluster.BoundsMax = glm::vec3( std::numeric_limits<float>::lowest() );
                for ( const uint32_t t : cluster.Triangles )
                {
                    topology.TriangleCluster[t] = static_cast<uint32_t>( topology.Clusters.size() );
                    for ( uint32_t c = 0; c < 3; ++c )
                    {
                        const glm::vec3& p = topology.WeldPosition[topology.CornerWeld[3 * t + c]];
                        cluster.BoundsMin  = glm::min( cluster.BoundsMin, p );
                        cluster.BoundsMax  = glm::max( cluster.BoundsMax, p );
                    }
                }
                topology.Clusters.push_back( std::move( cluster ) );
            }
            first = last;
        }

        // Neighbours (a shared welded edge) and the resolution limit per cluster.
        const float          maxFactor = static_cast<float>( kBarycentricMax / kMinBarycentricStep );
        std::vector<uint8_t> edgeMaxLevel( topology.EdgeWelds.size() );
        for ( size_t e = 0; e < lengths.size(); ++e )
        {
            // factor(L) = ceil(length * 2^L / base) must stay <= maxFactor.
            const float ratio = lengths[e] > 0.0f
                                     ? maxFactor * topology.BaseEdgeCm / lengths[e]
                                     : std::ldexp( 1.0f, static_cast<int>( kDisplacementLevelCount ) );
            const float level = std::floor( std::log2( std::max( ratio, 1.0f ) ) );
            edgeMaxLevel[e] =
                 static_cast<uint8_t>( std::min( level, static_cast<float>( kDisplacementLevelCount - 1 ) ) );
        }
        for ( size_t c = 0; c < topology.Clusters.size(); ++c )
        {
            DisplacementCluster& cluster = topology.Clusters[c];
            cluster.MaxLevel             = static_cast<uint8_t>( kDisplacementLevelCount - 1 );
            for ( const uint32_t t : cluster.Triangles )
                for ( uint32_t k = 0; k < 3; ++k )
                {
                    const uint32_t e = topology.CornerEdge[3 * t + k];
                    cluster.MaxLevel = std::min( cluster.MaxLevel, edgeMaxLevel[e] );
                    for ( uint32_t i = topology.EdgeTriangleOffset[e]; i < topology.EdgeTriangleOffset[e + 1];
                          ++i )
                    {
                        const uint32_t other = topology.TriangleCluster[topology.EdgeTriangles[i]];
                        if ( other != c )
                            cluster.Neighbours.push_back( other );
                    }
                }
            std::sort( cluster.Neighbours.begin(), cluster.Neighbours.end() );
            cluster.Neighbours.erase( std::unique( cluster.Neighbours.begin(), cluster.Neighbours.end() ),
                                      cluster.Neighbours.end() );
        }
        return Common::MakeSuccess( std::move( topology ) );
    }

    std::vector<uint8_t> EncodeDisplacementTopology( const DisplacementTopology& t )
    {
        std::vector<uint8_t> out;
        out.insert( out.end(), std::begin( kTopologyMagic ), std::end( kTopologyMagic ) );
        WritePod( out, kTopologyFormatVersion );
        WritePod( out, t.BaseEdgeCm );
        WriteVector( out, t.CornerVertex );
        WriteVector( out, t.CornerWeld );
        WriteVector( out, t.CornerEdge );
        WriteVector( out, t.TriangleSubmesh );
        WriteVector( out, t.TriangleCluster );
        WriteVector( out, t.WeldPosition );
        WriteVector( out, t.WeldDirection );
        WriteVector( out, t.WeldCornerOffset );
        WriteVector( out, t.WeldCorners );
        WriteVector( out, t.EdgeWelds );
        WriteVector( out, t.EdgeTriangleOffset );
        WriteVector( out, t.EdgeTriangles );
        WritePod( out, static_cast<uint64_t>( t.Clusters.size() ) );
        for ( const DisplacementCluster& c : t.Clusters )
        {
            WritePod( out, c.Submesh );
            WritePod( out, c.MaxLevel );
            WritePod( out, c.BoundsMin );
            WritePod( out, c.BoundsMax );
            WriteVector( out, c.Triangles );
            WriteVector( out, c.Neighbours );
        }
        return out;
    }

    Common::ResultStr<DisplacementTopology> DecodeDisplacementTopology( std::span<const uint8_t> data )
    {
        if ( data.size() < sizeof( kTopologyMagic ) ||
             std::memcmp( data.data(), kTopologyMagic, sizeof( kTopologyMagic ) ) != 0 )
            return Common::MakeError<DisplacementTopology>(
                 "not a displacement topology (magic is not DESTDSPT)" );
        Reader         r{ data, sizeof( kTopologyMagic ) };
        const uint32_t version = r.Pod<uint32_t>();
        if ( version != kTopologyFormatVersion )
            return Common::MakeFormattedError<DisplacementTopology>(
                 "displacement topology format {} is not the supported {}", version, kTopologyFormatVersion );
        DisplacementTopology t;
        t.BaseEdgeCm            = r.Pod<float>();
        t.CornerVertex          = r.Vector<uint32_t>();
        t.CornerWeld            = r.Vector<uint32_t>();
        t.CornerEdge            = r.Vector<uint32_t>();
        t.TriangleSubmesh       = r.Vector<uint32_t>();
        t.TriangleCluster       = r.Vector<uint32_t>();
        t.WeldPosition          = r.Vector<glm::vec3>();
        t.WeldDirection         = r.Vector<glm::vec3>();
        t.WeldCornerOffset      = r.Vector<uint32_t>();
        t.WeldCorners           = r.Vector<uint32_t>();
        t.EdgeWelds             = r.Vector<std::array<uint32_t, 2>>();
        t.EdgeTriangleOffset    = r.Vector<uint32_t>();
        t.EdgeTriangles         = r.Vector<uint32_t>();
        const uint64_t clusters = r.Pod<uint64_t>();
        for ( uint64_t i = 0; i < clusters && !r.Failed; ++i )
        {
            DisplacementCluster c;
            c.Submesh    = r.Pod<uint32_t>();
            c.MaxLevel   = r.Pod<uint8_t>();
            c.BoundsMin  = r.Pod<glm::vec3>();
            c.BoundsMax  = r.Pod<glm::vec3>();
            c.Triangles  = r.Vector<uint32_t>();
            c.Neighbours = r.Vector<uint32_t>();
            t.Clusters.push_back( std::move( c ) );
        }
        if ( r.Failed || r.Cursor != data.size() )
            return Common::MakeFormattedError<DisplacementTopology>(
                 "displacement topology: truncated or trailing data at byte {} of {}", r.Cursor, data.size() );
        return Common::MakeSuccess( std::move( t ) );
    }

    Common::ResultStr<TessellatedCluster> TessellateCluster( const DisplacementTopology& topology,
                                                             uint32_t                    cluster,
                                                             std::span<const uint8_t>    clusterLevels )
    {
        if ( cluster >= topology.Clusters.size() )
            return Common::MakeFormattedError<TessellatedCluster>( "displacement: cluster {} of {}", cluster,
                                                                   topology.Clusters.size() );
        if ( clusterLevels.size() != topology.Clusters.size() )
            return Common::MakeFormattedError<TessellatedCluster>(
                 "displacement: {} levels for {} clusters", clusterLevels.size(), topology.Clusters.size() );
        for ( size_t c = 0; c < clusterLevels.size(); ++c )
            if ( clusterLevels[c] > topology.Clusters[c].MaxLevel )
                return Common::MakeFormattedError<TessellatedCluster>(
                     "displacement: cluster {} asks level {} above its resolution limit {}", c, clusterLevels[c],
                     topology.Clusters[c].MaxLevel );
        return ClusterTessellator( topology, cluster, clusterLevels ).Run();
    }

    uint8_t DisplacementLevelForDensity( const DisplacementTopology& topology, uint32_t cluster, float pixelsPerCm,
                                         float dicingRatePixels, bool* clamped )
    {
        // Level L targets BaseEdgeCm / 2^L cm per diced edge; the view wants dicingRatePixels / pixelsPerCm.
        const float wantedCm = dicingRatePixels / std::max( pixelsPerCm, 1e-12f );
        const float level    = std::ceil( std::log2( std::max( topology.BaseEdgeCm / wantedCm, 1.0f ) ) );
        const float top      = static_cast<float>( topology.Clusters[cluster].MaxLevel );
        if ( clamped )
            *clamped = level > top;
        return static_cast<uint8_t>( std::min( level, top ) );
    }
} // namespace Desert::Geometry
