// Ported from UE 5.8 .../GeometryCore/Private/CompGeom/Delaunay2.cpp (see the header for the line ranges and
// what was adapted).
#include "Engine/Geometry/MeshCore/CompGeom/Delaunay2.hpp"

#include "Engine/Geometry/MeshCore/CompGeom/ExactPredicates.hpp"

#include <algorithm>
#include <numeric>
#include <unordered_map>

namespace Desert::Geometry
{
    // Triangle connectivity for Delaunay triangulation: a half-edge -> opposite-vertex map, with one 'ghost
    // vertex' joined to every hull edge, and an optional vertex -> one-ring-neighbour cache.
    struct Delaunay2Connectivity
    {
        static constexpr int GhostIndex   = -1;
        static constexpr int InvalidIndex = -2;

        explicit Delaunay2Connectivity( bool bTrackDuplicateVerticesIn )
             : m_bTrackDuplicateVertices( bTrackDuplicateVerticesIn )
        {
        }

        void Empty( int ExpectedMaxVertices )
        {
            m_bHasDuplicates = false;
            m_EdgeToVert.clear();
            m_EdgeToVert.reserve( static_cast<size_t>( ExpectedMaxVertices ) * 6 );
            DisableVertexAdjacency();
        }

        void EnableVertexAdjacency( int NumVertices )
        {
            if ( !m_bUseAdjCache ||
                 static_cast<int>( m_VertexAdjCache.size() ) < VertexIDToAdjIndex( NumVertices ) )
            {
                m_bUseAdjCache = true;
                m_VertexAdjCache.assign( static_cast<size_t>( VertexIDToAdjIndex( NumVertices ) ), -1 );
                for ( const auto& [Edge, Vert] : m_EdgeToVert )
                    UpdateAdjCache( Edge );
            }
        }

        void DisableVertexAdjacency()
        {
            m_bUseAdjCache = false;
            m_VertexAdjCache.clear();
        }

        bool HasEdge( const Index2i& Edge ) const
        {
            return m_EdgeToVert.contains( Edge );
        }
        int NumTriangles() const
        {
            return static_cast<int>( m_EdgeToVert.size() / 3 );
        }
        int NumHalfEdges() const
        {
            return static_cast<int>( m_EdgeToVert.size() );
        }

        void GetTrianglesAndAdjacency( std::vector<Index3i>& Triangles, std::vector<Index3i>& Adjacency ) const
        {
            Triangles.clear();
            for ( const auto& [Edge, Vert] : m_EdgeToVert )
                if ( !IsGhost( Edge, Vert ) && Vert < Edge.A && Vert < Edge.B )
                    Triangles.emplace_back( Edge.A, Edge.B, Vert );

            // the edge map knows nothing of triangle indices; build adjacency from a fresh map
            Adjacency.assign( Triangles.size(), Index3i::Invalid() );
            std::unordered_map<Index2i, int> EdgeToTri;
            for ( int TriIdx = 0; TriIdx < static_cast<int>( Triangles.size() ); ++TriIdx )
            {
                const Index3i& Tri = Triangles[TriIdx];
                for ( int Sub0 = 2, Sub1 = 0; Sub1 < 3; Sub0 = Sub1++ )
                {
                    const Index2i RevEdge( Tri[Sub1], Tri[Sub0] );
                    const auto    Found = EdgeToTri.find( RevEdge );
                    if ( Found != EdgeToTri.end() )
                    {
                        const int      NbrTriIdx     = Found->second;
                        const Index3i& AdjTri        = Triangles[NbrTriIdx];
                        const int      AdjSub        = AdjTri.IndexOf( RevEdge.A );
                        Adjacency[NbrTriIdx][AdjSub] = TriIdx;
                        Adjacency[TriIdx][Sub0]      = NbrTriIdx;
                        EdgeToTri.erase( Found ); // only need the match once
                    }
                    else
                        EdgeToTri.emplace( Index2i( RevEdge.B, RevEdge.A ), TriIdx );
                }
            }
        }

        static bool IsGhost( const Index2i& Edge, int Vertex )
        {
            return Edge.A == GhostIndex || Edge.B == GhostIndex || Vertex == GhostIndex;
        }
        static bool IsGhost( const Index2i& Edge )
        {
            return Edge.A == GhostIndex || Edge.B == GhostIndex;
        }

        void AddTriangle( const Index3i& Tri )
        {
            m_EdgeToVert.insert_or_assign( Index2i( Tri.A, Tri.B ), Tri.C );
            m_EdgeToVert.insert_or_assign( Index2i( Tri.B, Tri.C ), Tri.A );
            m_EdgeToVert.insert_or_assign( Index2i( Tri.C, Tri.A ), Tri.B );
            if ( m_bUseAdjCache )
            {
                const Index3i TriAdjI( VertexIDToAdjIndex( Tri.A ), VertexIDToAdjIndex( Tri.B ),
                                       VertexIDToAdjIndex( Tri.C ) );
                m_VertexAdjCache[TriAdjI.A] = TriAdjI.B;
                m_VertexAdjCache[TriAdjI.B] = TriAdjI.C;
                m_VertexAdjCache[TriAdjI.C] = TriAdjI.A;
            }
        }

        // a first triangle surrounded by ghost triangles
        void InitWithGhosts( const Index3i& Tri )
        {
            AddTriangle( Tri );
            AddTriangle( Index3i( Tri.B, Tri.A, GhostIndex ) );
            AddTriangle( Index3i( Tri.C, Tri.B, GhostIndex ) );
            AddTriangle( Index3i( Tri.A, Tri.C, GhostIndex ) );
        }

        void DeleteTriangle( const Index3i& Tri )
        {
            m_EdgeToVert.erase( Index2i( Tri.A, Tri.B ) );
            m_EdgeToVert.erase( Index2i( Tri.B, Tri.C ) );
            m_EdgeToVert.erase( Index2i( Tri.C, Tri.A ) );
            if ( m_bUseAdjCache )
            {
                const Index3i TriAdjI( VertexIDToAdjIndex( Tri.A ), VertexIDToAdjIndex( Tri.B ),
                                       VertexIDToAdjIndex( Tri.C ) );
                const int     InvalidAdj = VertexIDToAdjIndex( InvalidIndex );
                if ( m_VertexAdjCache[TriAdjI.A] == TriAdjI.B )
                    m_VertexAdjCache[TriAdjI.A] = InvalidAdj;
                if ( m_VertexAdjCache[TriAdjI.B] == TriAdjI.C )
                    m_VertexAdjCache[TriAdjI.B] = InvalidAdj;
                if ( m_VertexAdjCache[TriAdjI.C] == TriAdjI.A )
                    m_VertexAdjCache[TriAdjI.C] = InvalidAdj;
            }
        }

        int GetVertex( const Index2i& Edge ) const
        {
            const auto Found = m_EdgeToVert.find( Edge );
            return Found == m_EdgeToVert.end() ? InvalidIndex : Found->second;
        }

        void MarkDuplicateVertex( int Orig, int Duplicate )
        {
            m_DuplicateVertices.insert_or_assign( Duplicate, Orig );
        }
        bool HasDuplicateTracking() const
        {
            return m_bTrackDuplicateVertices;
        }
        void SetHasDuplicates()
        {
            m_bHasDuplicates = true;
        }
        bool HasDuplicates() const
        {
            return m_bHasDuplicates;
        }

        void FixDuplicatesOnEdge( Index2i& Edge ) const
        {
            if ( const auto OrigA = m_DuplicateVertices.find( Edge.A ); OrigA != m_DuplicateVertices.end() )
                Edge.A = OrigA->second;
            if ( const auto OrigB = m_DuplicateVertices.find( Edge.B ); OrigB != m_DuplicateVertices.end() )
                Edge.B = OrigB->second;
        }

        // any edge BC opposite Vertex such that triangle (Vertex, B, C) is in the mesh, or an InvalidIndex edge
        Index2i GetEdge( int Vertex )
        {
            if ( m_bUseAdjCache )
            {
                const int AdjVertex = AdjIndexToVertexID( m_VertexAdjCache[VertexIDToAdjIndex( Vertex )] );
                if ( AdjVertex != InvalidIndex )
                {
                    const int LastVertex = GetVertex( Index2i( Vertex, AdjVertex ) );
                    if ( LastVertex != InvalidIndex )
                        return Index2i( AdjVertex, LastVertex );
                }
            }
            for ( const auto& [Edge, Vert] : m_EdgeToVert )
            {
                if ( m_bUseAdjCache )
                    UpdateAdjCache( Edge );
                if ( Vertex == Edge.A )
                    return Index2i( Edge.B, Vert );
            }
            return Index2i( InvalidIndex, InvalidIndex );
        }

    private:
        std::unordered_map<Index2i, int> m_EdgeToVert;
        std::vector<int>                 m_VertexAdjCache;
        bool                             m_bUseAdjCache            = false;
        bool                             m_bHasDuplicates          = false;
        bool                             m_bTrackDuplicateVertices = false;
        std::unordered_map<int, int>     m_DuplicateVertices;

        // offset by 1 so the GhostIndex has slot 0
        static int VertexIDToAdjIndex( int VertexID )
        {
            return VertexID + 1;
        }
        static int AdjIndexToVertexID( int AdjIndex )
        {
            return AdjIndex - 1;
        }
        void UpdateAdjCache( const Index2i& Edge )
        {
            m_VertexAdjCache[VertexIDToAdjIndex( Edge.A )] = VertexIDToAdjIndex( Edge.B );
        }
    };
} // namespace Desert::Geometry

using namespace Desert::Geometry;

namespace
{
    using Connectivity   = Delaunay2Connectivity;
    constexpr int GhostV = Connectivity::GhostIndex;
    using Vertices2      = std::vector<glm::dvec2>;

    // true if Vertex is inside the circumcircle of Tri; for a ghost triangle: on its one solid edge or inside that
    // edge's half-space
    bool InTriCircle( const Vertices2& Vertices, const Index3i& Tri, int Vertex )
    {
        if ( Tri.IndexOf( GhostV ) == -1 )
            return ExactPredicates::InCircle2( Vertices[Tri.A], Vertices[Tri.B], Vertices[Tri.C],
                                               Vertices[Vertex] ) > 0;
        const Index3i GhostFirst = Tri.GetCycled( GhostV );
        const double  Pred =
             ExactPredicates::Orient2( Vertices[GhostFirst.B], Vertices[GhostFirst.C], Vertices[Vertex] );
        if ( Pred > 0 )
            return true;
        if ( Pred < 0 )
            return false;
        // collinear: is Vertex *on* the edge?
        const glm::dvec2& P = Vertices[GhostFirst.B];
        const glm::dvec2& Q = Vertices[GhostFirst.C];
        const glm::dvec2& V = Vertices[Vertex];
        if ( P.x != Q.x )
            return V.x >= std::min( P.x, Q.x ) && V.x <= std::max( P.x, Q.x );
        return V.y >= std::min( P.y, Q.y ) && V.y <= std::max( P.y, Q.y );
    }

    Index3i WalkToContainingTri( std::mt19937& Random, Connectivity& Conn, const Vertices2& Vertices,
                                 Index3i StartTri, int Vertex, bool bAssumeDelaunay, int& IsDuplicateOfOut )
    {
        IsDuplicateOfOut = Connectivity::InvalidIndex;

        auto ChooseCross = [&]( const Index3i& Tri, bool bSkipFirst ) -> int
        {
            auto CrossesEdge = [&]( int A, int B, bool bOnGhostTri ) -> bool
            {
                if ( !bOnGhostTri || ( A != GhostV && B != GhostV ) )
                {
                    const double Orient = ExactPredicates::Orient2( Vertices[A], Vertices[B], Vertices[Vertex] );
                    if ( Orient < 0 || ( bOnGhostTri && Orient == 0 ) )
                        return true;
                }
                return false;
            };

            int        Choose[2]{ -1, -1 };
            int        Chosen   = 0;
            const bool bIsGhost = Tri.Contains( GhostV );
            const int  NextSub[3]{ 1, 2, 0 };
            for ( int EdgeSub = static_cast<int>( bSkipFirst ); EdgeSub < 3; ++EdgeSub )
                if ( CrossesEdge( Tri[EdgeSub], Tri[NextSub[EdgeSub]], bIsGhost ) )
                {
                    // on a Delaunay mesh the first edge with the target behind it can always be crossed
                    if ( bAssumeDelaunay )
                        return EdgeSub;
                    // otherwise choose randomly between candidates, which avoids a cycle
                    Choose[Chosen++] = EdgeSub;
                }
            if ( Chosen == 0 )
                return -1; // on this tri
            if ( Chosen == 1 )
                return Choose[0];
            return Choose[std::uniform_int_distribution<int>( 0, 1 )( Random )];
        };

        Index3i   WalkTri  = StartTri;
        int       Cross    = ChooseCross( WalkTri, false );
        int       NumSteps = 0;
        const int NextIdx[3]{ 1, 2, 0 };
        while ( Cross != -1 )
        {
            if ( NumSteps++ >= Conn.NumTriangles() * 100 )
                return Index3i( Connectivity::InvalidIndex, Connectivity::InvalidIndex,
                                Connectivity::InvalidIndex );
            const Index2i OppEdge( WalkTri[NextIdx[Cross]], WalkTri[Cross] );
            const int     OppVert = Conn.GetVertex( OppEdge );
            WalkTri               = Index3i( OppEdge.A, OppEdge.B, OppVert );
            Cross                 = ChooseCross( WalkTri, true );
        }

        if ( WalkTri.A >= 0 && Vertices[Vertex] == Vertices[WalkTri.A] )
            IsDuplicateOfOut = WalkTri.A;
        else if ( WalkTri.B >= 0 && Vertices[Vertex] == Vertices[WalkTri.B] )
            IsDuplicateOfOut = WalkTri.B;
        else if ( WalkTri.C >= 0 && Vertices[Vertex] == Vertices[WalkTri.C] )
            IsDuplicateOfOut = WalkTri.C;
        return WalkTri;
    }

    // Bowyer-Watson: delete the connected triangles whose circumcircles contain the vertex, then fan the new
    // vertex out to the border of the deletion. @return one inserted triangle containing ToInsertV
    Index3i Insert( Connectivity& Conn, const Vertices2& Vertices, const Index3i& OnTri, int ToInsertV )
    {
        std::vector<Index2i> ToConsider;
        auto                 AddIfValid = [&]( const Index2i& Edge )
        {
            if ( Conn.HasEdge( Edge ) )
                ToConsider.push_back( Edge );
        };
        auto DeleteTri = [&]( const Index3i& Tri )
        {
            Conn.DeleteTriangle( Tri );
            AddIfValid( Index2i( Tri.B, Tri.A ) );
            AddIfValid( Index2i( Tri.C, Tri.B ) );
            AddIfValid( Index2i( Tri.A, Tri.C ) );
        };
        DeleteTri( OnTri );

        std::vector<Index2i> Border;
        while ( !ToConsider.empty() )
        {
            const Index2i Edge = ToConsider.back();
            ToConsider.pop_back();
            const int TriV = Conn.GetVertex( Edge );
            if ( TriV == Connectivity::InvalidIndex ) // already deleted by an earlier step
                continue;
            const Index3i ConsiderTri( Edge.A, Edge.B, TriV );
            if ( InTriCircle( Vertices, ConsiderTri, ToInsertV ) )
                DeleteTri( ConsiderTri );
            else
                Border.push_back( Edge );
        }

        for ( const Index2i& BorderEdge : Border )
            Conn.AddTriangle( Index3i( BorderEdge.B, BorderEdge.A, ToInsertV ) );
        return Border.empty() ? Index3i::Invalid() : Index3i( Border[0].B, Border[0].A, ToInsertV );
    }

    bool GetFirstCrossingEdge( Connectivity& Conn, const Vertices2& Vertices, const Index2i& EdgeToConnect,
                               Index2i& CrossingEdgeOut )
    {
        if ( Conn.HasEdge( EdgeToConnect ) )
            return false; // nothing to dig
        const Index2i StartWalk = Conn.GetEdge( EdgeToConnect.A );
        if ( StartWalk.A == Connectivity::InvalidIndex )
            return false; // the edge starts at a vertex not in the triangulation (e.g. a rejected duplicate)

        const glm::dvec2 VA = Vertices[EdgeToConnect.A];
        const glm::dvec2 VB = Vertices[EdgeToConnect.B];

        auto IsCrossingEdgeOnA = [&]( const Index2i& Edge, double OrientA, double& OrientBOut ) -> bool
        {
            if ( Edge.B >= 0 )
                OrientBOut = ExactPredicates::Orient2( VA, VB, Vertices[Edge.B] );
            if ( Connectivity::IsGhost( Edge ) )
                return false;
            if ( OrientA >= 0 )
                return false;
            // an edge crossing AB, on a triangle that includes A, must go from the negative to the positive side
            return OrientBOut > 0;
        };

        Index2i WalkEdge = StartWalk;
        double  OrientA  = 0;
        if ( WalkEdge.A != GhostV )
            OrientA = ExactPredicates::Orient2( VA, VB, Vertices[WalkEdge.A] );
        double OrientB     = 0; // set by the crossing test
        bool   bIsCrossing = IsCrossingEdgeOnA( WalkEdge, OrientA, OrientB );
        int    EdgesWalked = 0;
        while ( !bIsCrossing )
        {
            if ( EdgesWalked++ > Conn.NumHalfEdges() )
                return false;
            const int NextVertex = Conn.GetVertex( Index2i( EdgeToConnect.A, WalkEdge.B ) );
            if ( NextVertex == Connectivity::InvalidIndex ) // a hole in the mesh at this stage: unrecoverable
                return false;
            WalkEdge = Index2i( WalkEdge.B, NextVertex );
            // a full cycle with no crossing: the edge is blocked (e.g. by a vertex exactly on it)
            if ( WalkEdge == StartWalk )
                return false;
            OrientA     = OrientB;
            bIsCrossing = IsCrossingEdgeOnA( WalkEdge, OrientA, OrientB );
        }
        CrossingEdgeOut = WalkEdge;
        return true;
    }

    // @return the vertex to fill to: EdgeToConnect.A - no fill needed; EdgeToConnect.B - a normal
    // re-triangulation; another vertex - digging stopped at a collinear vertex and the cavity is filled up to it
    int DigCavity( Connectivity& Conn, const Vertices2& Vertices, const Index2i& EdgeToConnect,
                   std::vector<int>& CavityLOut, std::vector<int>& CavityROut )
    {
        CavityLOut.clear();
        CavityROut.clear();

        Index2i FirstCross;
        if ( !GetFirstCrossingEdge( Conn, Vertices, EdgeToConnect, FirstCross ) )
            return EdgeToConnect.A;

        Conn.DeleteTriangle( Index3i( EdgeToConnect.A, FirstCross.A, FirstCross.B ) );
        CavityROut.push_back( EdgeToConnect.A );
        CavityLOut.push_back( EdgeToConnect.A );
        CavityROut.push_back( FirstCross.A );
        CavityLOut.push_back( FirstCross.B );

        const glm::dvec2 VA = Vertices[EdgeToConnect.A];
        const glm::dvec2 VB = Vertices[EdgeToConnect.B];

        // by convention WalkCross always crosses from left to right
        Index2i WalkCross( FirstCross.B, FirstCross.A );
        while ( true ) // cannot loop forever: it deletes triangles as it walks
        {
            const int NextV = Conn.GetVertex( WalkCross );
            if ( NextV == Connectivity::InvalidIndex )
                return EdgeToConnect.A; // walked off the triangulation: unrecoverably broken
            Conn.DeleteTriangle( Index3i( WalkCross.A, WalkCross.B, NextV ) );

            if ( NextV == EdgeToConnect.B )
            {
                CavityROut.push_back( EdgeToConnect.B );
                CavityLOut.push_back( EdgeToConnect.B );
                return EdgeToConnect.B;
            }

            const double OrientNextV = ExactPredicates::Orient2( VA, VB, Vertices[NextV] );
            if ( OrientNextV == 0 )
            {
                // the target cannot be reached past this collinear vertex; stop here
                CavityROut.push_back( NextV );
                CavityLOut.push_back( NextV );
                return NextV;
            }
            if ( OrientNextV < 0 )
            {
                WalkCross = Index2i( WalkCross.A, NextV );
                CavityROut.push_back( NextV );
            }
            else
            {
                WalkCross = Index2i( NextV, WalkCross.B );
                CavityLOut.push_back( NextV );
            }
        }
    }

    // Anglada (1997) cavity retriangulation (UE: faster than Shewchuk-Brown below ~30-85 cavity vertices).
    void FillCavity( Connectivity& Conn, const Vertices2& Vertices, const std::vector<int>& Cavity )
    {
        std::vector<Index2i> Ranges;
        Ranges.reserve( Cavity.size() );
        Ranges.emplace_back( 0, static_cast<int>( Cavity.size() ) - 1 );
        while ( !Ranges.empty() )
        {
            const Index2i Range = Ranges.back();
            Ranges.pop_back();
            int Mid = Range.A + 1;
            if ( Range.A + 2 == Range.B )
            {
                Conn.AddTriangle( Index3i( Cavity[Range.A], Cavity[Mid], Cavity[Range.B] ) );
                continue;
            }
            for ( int Cand = Mid + 1; Cand < Range.B; ++Cand )
                if ( 0 < ExactPredicates::InCircle2( Vertices[Cavity[Range.A]], Vertices[Cavity[Mid]],
                                                     Vertices[Cavity[Range.B]], Vertices[Cavity[Cand]] ) )
                    Mid = Cand;
            Conn.AddTriangle( Index3i( Cavity[Range.A], Cavity[Mid], Cavity[Range.B] ) );
            if ( Mid - Range.A > 1 )
                Ranges.emplace_back( Range.A, Mid );
            if ( Range.B - Mid > 1 )
                Ranges.emplace_back( Mid, Range.B );
        }
    }

    bool ConstrainEdgesImpl( Connectivity& Conn, const Vertices2& Vertices, const std::vector<Index2i>& Edges,
                             bool bKeepFastEdgeAdjacencyData )
    {
        constexpr size_t NeedFasterEdgeLookupThreshold = 4;
        if ( bKeepFastEdgeAdjacencyData || Edges.size() > NeedFasterEdgeLookupThreshold )
            Conn.EnableVertexAdjacency( static_cast<int>( Vertices.size() ) );

        // UE shuffles the edge order through GetShuffledOrder, whose loop starts at the unset EndIn (-1) and so
        // never swaps: the edges go in input order, as here.
        std::vector<int> CavityVerts[2];
        for ( Index2i Edge : Edges )
        {
            if ( Conn.HasDuplicateTracking() )
                Conn.FixDuplicatesOnEdge( Edge );
            if ( Edge.A == Edge.B )
                continue;
            const int DigTo = DigCavity( Conn, Vertices, Edge, CavityVerts[0], CavityVerts[1] );
            if ( DigTo != Edge.A )
            {
                // fill the dug cavity (which may end short of the target, at a collinear vertex)
                std::reverse( CavityVerts[0].begin(), CavityVerts[0].end() );
                FillCavity( Conn, Vertices, CavityVerts[0] );
                FillCavity( Conn, Vertices, CavityVerts[1] );
            }
        }
        if ( !bKeepFastEdgeAdjacencyData )
            Conn.DisableVertexAdjacency();
        return true;
    }

    void Update( std::mt19937& Random, Connectivity& Conn, const Vertices2& Vertices,
                 const std::vector<int>& Order, Index3i SearchTri, bool bAssumeDelaunay )
    {
        for ( const int Vertex : Order )
        {
            int           DuplicateOf = -1;
            const Index3i ContainingTri =
                 WalkToContainingTri( Random, Conn, Vertices, SearchTri, Vertex, bAssumeDelaunay, DuplicateOf );
            if ( DuplicateOf >= 0 )
            {
                Conn.SetHasDuplicates();
                if ( Conn.HasDuplicateTracking() )
                    Conn.MarkDuplicateVertex( DuplicateOf, Vertex );
                continue;
            }
            if ( ContainingTri.A == Connectivity::InvalidIndex )
                continue;
            SearchTri = Insert( Conn, Vertices, ContainingTri, Vertex );
        }
    }
} // namespace

Delaunay2::Delaunay2()  = default;
Delaunay2::~Delaunay2() = default;

bool Delaunay2::Triangulate( const std::vector<glm::dvec2>& Vertices, const std::vector<Index2i>& Edges )
{
    m_Connectivity = std::make_unique<Delaunay2Connectivity>( m_bAutomaticallyFixEdgesToDuplicateVertices );
    Delaunay2Connectivity& Conn = *m_Connectivity;
    Conn.Empty( static_cast<int>( Vertices.size() ) );
    if ( Vertices.size() < 3 )
        return false;

    std::vector<int> Order( Vertices.size() );
    std::iota( Order.begin(), Order.end(), 0 );
    std::shuffle( Order.begin(), Order.end(), m_Random );

    // bootstrap triangle from the end of the order: two distinct points and a third off their line
    int        BootstrapIndices[3]{ static_cast<int>( Order.size() ) - 1, -1, -1 };
    glm::dvec2 Pts[3];
    Pts[0]                 = Vertices[Order[BootstrapIndices[0]]];
    double BootstrapOrient = 0;
    for ( int SecondIdx = static_cast<int>( Order.size() ) - 2; SecondIdx >= 0; --SecondIdx )
        if ( Pts[0] != Vertices[Order[SecondIdx]] )
        {
            Pts[1]              = Vertices[Order[SecondIdx]];
            BootstrapIndices[1] = SecondIdx;
            break;
        }
    if ( BootstrapIndices[1] == -1 )
        return false; // all points identical
    for ( int ThirdIdx = BootstrapIndices[1] - 1; ThirdIdx >= 0; --ThirdIdx )
    {
        const double Orient = ExactPredicates::Orient2( Pts[0], Pts[1], Vertices[Order[ThirdIdx]] );
        if ( Orient != 0 )
        {
            BootstrapIndices[2] = ThirdIdx;
            BootstrapOrient     = Orient;
            break;
        }
    }
    if ( BootstrapIndices[2] == -1 )
        return false; // all points collinear

    Index3i FirstTri( Order[BootstrapIndices[0]], Order[BootstrapIndices[1]], Order[BootstrapIndices[2]] );
    if ( BootstrapOrient < 0 )
        std::swap( FirstTri.B, FirstTri.C );
    Conn.InitWithGhosts( FirstTri );
    // indices are descending, so each erase leaves the next index valid
    Order.erase( Order.begin() + BootstrapIndices[0] );
    Order.erase( Order.begin() + BootstrapIndices[1] );
    Order.erase( Order.begin() + BootstrapIndices[2] );

    Update( m_Random, Conn, Vertices, Order, FirstTri, true /* before constraints the mesh is Delaunay */ );
    ConstrainEdgesImpl( Conn, Vertices, Edges, m_bKeepFastEdgeAdjacencyData );
    return ValidateEdgesResult( Edges );
}

bool Delaunay2::ConstrainEdges( const std::vector<glm::dvec2>& Vertices, const std::vector<Index2i>& Edges )
{
    ConstrainEdgesImpl( *m_Connectivity, Vertices, Edges, m_bKeepFastEdgeAdjacencyData );
    return ValidateEdgesResult( Edges );
}

bool Delaunay2::ValidateEdgesResult( const std::vector<Index2i>& Edges ) const
{
    if ( !m_bValidateEdges )
        return true;
    for ( const Index2i& Edge : Edges )
        if ( !HasEdge( Edge, m_bAutomaticallyFixEdgesToDuplicateVertices ) )
            return false;
    return true;
}

void Delaunay2::GetTrianglesAndAdjacency( std::vector<Index3i>& Triangles, std::vector<Index3i>& Adjacency ) const
{
    Triangles.clear();
    Adjacency.clear();
    if ( m_Connectivity )
        m_Connectivity->GetTrianglesAndAdjacency( Triangles, Adjacency );
}

void Delaunay2::FixDuplicatesOnEdge( Index2i& Edge ) const
{
    m_Connectivity->FixDuplicatesOnEdge( Edge );
}

bool Delaunay2::HasEdge( const Index2i& Edge, bool bRemapDuplicates ) const
{
    Index2i Remapped = Edge;
    if ( bRemapDuplicates )
        m_Connectivity->FixDuplicatesOnEdge( Remapped );
    return m_Connectivity->HasEdge( Remapped );
}

bool Delaunay2::HasDuplicates() const
{
    return m_Connectivity->HasDuplicates();
}
