// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/MeshBoundaryLoops.cpp (see the header for the line
// ranges and what was left out) and Public/VectorUtil.h:290-303 (PlaneAngleSignedD).
#include "Engine/Geometry/UECore/MeshBoundaryLoops.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Desert::Geometry;

namespace
{
    // UE VectorUtil::PlaneAngleSignedD: signed angle in degrees between the projections of From and To onto the
    // plane with normal PlaneN.
    double PlaneAngleSignedD( const glm::dvec3& VFrom, const glm::dvec3& VTo, const glm::dvec3& PlaneN )
    {
        glm::dvec3 From = VFrom - glm::dot( VFrom, PlaneN ) * PlaneN;
        glm::dvec3 To   = VTo - glm::dot( VTo, PlaneN ) * PlaneN;
        Normalize( From );
        Normalize( To );
        const glm::dvec3 C = glm::cross( From, To );
        if ( glm::length2( C ) < FMathd::ZeroTolerance )
            return glm::dot( From, To ) < 0 ? 180.0 : 0.0;
        const double Sign = glm::dot( C, PlaneN ) < 0 ? -1.0 : 1.0;
        const double Angle =
             std::acos( std::clamp( glm::dot( From, To ), -1.0, 1.0 ) ) * ( 180.0 / 3.14159265358979323846 );
        return Sign * Angle;
    }

    void ReverseInPlace( std::vector<int>& Values )
    {
        for ( int i = 0, j = static_cast<int32_t>( Values.size() ) - 1; i < j; ++i, --j )
            std::swap( Values[i], Values[j] );
    }
} // namespace

int FMeshBoundaryLoops::FindLoopContainingVertex( int VertexID ) const
{
    for ( int li = 0; li < static_cast<int32_t>( Loops.size() ); ++li )
        if ( ( std::find( Loops[li].Vertices.begin(), Loops[li].Vertices.end(), VertexID ) !=
               Loops[li].Vertices.end() ) )
            return li;
    return -1;
}

int FMeshBoundaryLoops::FindLoopContainingEdge( int EdgeID ) const
{
    for ( int li = 0; li < static_cast<int32_t>( Loops.size() ); ++li )
        if ( ( std::find( Loops[li].Edges.begin(), Loops[li].Edges.end(), EdgeID ) != Loops[li].Edges.end() ) )
            return li;
    return -1;
}

bool FMeshBoundaryLoops::Compute()
{
    // Triangles are assumed consistently oriented, so a closed boundary loop is followed by walking edges in
    // order.
    Loops.clear();
    Spans.clear();
    bSawOpenSpans = bFellBackToSpansOnFailure = false;
    if ( Mesh->IsClosed() )
        return true;

    const int    NE = Mesh->MaxEdgeID();
    std::vector<bool> UsedEdge;
    UsedEdge.assign( NE, false );
    std::vector<int> LoopEdges;
    std::vector<int> LoopVerts;
    std::vector<int> Bowties;
    std::vector<int> AllE;

    for ( int Eid = 0; Eid < NE; ++Eid )
    {
        if ( !Mesh->IsEdge( Eid ) || UsedEdge[Eid] || !Mesh->IsBoundaryEdge( Eid ) )
            continue;

        const int EStart = Eid;
        UsedEdge[EStart] = true;
        LoopEdges.push_back( EStart );
        int  ECur        = Eid;
        bool bClosed     = false;
        bool bIsOpenSpan = false;
        while ( !bClosed )
        {
            const FIndex2i Ev    = Mesh->GetOrientedBoundaryEdgeV( ECur );
            int            CureB = Ev.B;
            if ( bIsOpenSpan )
                CureB = Ev.A;
            else
                LoopVerts.push_back( Ev.A );

            int       E0       = -1;
            int       E1       = 1;
            const int BdryNbrs = Mesh->GetVtxBoundaryEdges( CureB, E0, E1 );
            if ( BdryNbrs < 2 )
            {
                // an endpoint vertex: continue the span from the other end of the chain
                if ( bIsOpenSpan )
                {
                    bClosed = true;
                    continue;
                }
                bIsOpenSpan = true;
                ECur        = LoopEdges[0];
                ReverseInPlace( LoopEdges );
                continue;
            }

            int ENext = -1;
            if ( BdryNbrs > 2 )
            {
                // a bowtie vertex
                if ( CureB == LoopVerts[0] )
                {
                    ENext = -2; // the loop closes at its start vertex
                }
                else
                {
                    AllE.clear();
                    const int NumBe = Mesh->GetAllVtxBoundaryEdges( CureB, AllE );
                    ENext           = FindLeftTurnEdge( ECur, CureB, AllE, NumBe, UsedEdge );
                    if ( ENext == -1 )
                    {
                        // stuck: keep what was walked as a span
                        bIsOpenSpan = true;
                        bClosed     = true;
                        continue;
                    }
                }
                if ( !( std::find( Bowties.begin(), Bowties.end(), CureB ) != Bowties.end() ) )
                    Bowties.push_back( CureB );
            }
            else
            {
                UE_CHECK( E0 == ECur || E1 == ECur );
                ENext = ( E0 == ECur ) ? E1 : E0;
            }

            if ( ENext == -2 || ENext == EStart )
            {
                bClosed = true;
            }
            else if ( UsedEdge[ENext] )
            {
                // the next edge belongs to another chain: keep this one as a span
                bIsOpenSpan = true;
                bClosed     = true;
            }
            else
            {
                LoopEdges.push_back( ENext );
                UsedEdge[ENext] = true;
                ECur            = ENext;
            }
        }

        if ( bIsOpenSpan )
        {
            bSawOpenSpans = true;
            ReverseInPlace( LoopEdges );
            Spans.push_back( FEdgeSpan{} );
            Spans[static_cast<int32_t>( Spans.size() ) - 1].InitializeFromEdges( *Mesh, LoopEdges );
        }
        else if ( !Bowties.empty() )
        {
            FSubloops Subloops;
            if ( !ExtractSubloops( LoopVerts, LoopEdges, Bowties, Subloops ) )
            {
                if ( !Subloops.Spans.empty() )
                {
                    bFellBackToSpansOnFailure = true;
                    for ( const FEdgeSpan& Span : Subloops.Spans )
                        Spans.push_back( Span );
                }
            }
            else
            {
                for ( const FEdgeLoop& Loop : Subloops.Loops )
                    Loops.push_back( Loop );
            }
        }
        else
        {
            Loops.push_back( FEdgeLoop{} );
            Loops[static_cast<int32_t>( Loops.size() ) - 1].Initialize( LoopVerts, LoopEdges );
        }
        LoopEdges.clear();
        LoopVerts.clear();
        Bowties.clear();
    }
    return true;
}

glm::dvec3 FMeshBoundaryLoops::GetVertexNormal( int Vid ) const
{
    glm::dvec3 N = glm::dvec3( 0 );
    for ( int Ti : Mesh->VtxTrianglesItr( Vid ) )
        N += Mesh->GetTriNormal( Ti );
    Normalize( N );
    return N;
}

int FMeshBoundaryLoops::FindLeftTurnEdge( int IncomingE, int BowtieV, const std::vector<int>& BdryEdges,
                                          int BdryEdgesCount, const std::vector<bool>& UsedEdges ) const
{
    // the normal at the bowtie vertex is the plane the turn angles are measured in
    const glm::dvec3 N      = GetVertexNormal( BowtieV );
    const FIndex2i  Ev     = Mesh->GetEdgeV( IncomingE );
    const int       OtherV = ( Ev.A == BowtieV ) ? Ev.B : Ev.A;
    const glm::dvec3 Ab     = Mesh->GetVertex( BowtieV ) - Mesh->GetVertex( OtherV );

    int    BestE     = -1;
    double BestAngle = std::numeric_limits<double>::max();
    for ( int i = 0; i < BdryEdgesCount; ++i )
    {
        const int BdryEid = BdryEdges[i];
        if ( UsedEdges[BdryEid] )
            continue;
        const FIndex2i BdryEv = Mesh->GetOrientedBoundaryEdgeV( BdryEid );
        if ( BdryEv.A != BowtieV )
            continue; // must chain onto the end of the current edge, orientation-wise
        const glm::dvec3 Bc     = Mesh->GetVertex( BdryEv.B ) - Mesh->GetVertex( BowtieV );
        const double    AngleS = -PlaneAngleSignedD( Ab, Bc, N );
        if ( BestAngle == std::numeric_limits<double>::max() || AngleS < BestAngle )
        {
            BestAngle = AngleS;
            BestE     = BdryEid;
        }
    }
    return BestE;
}

bool FMeshBoundaryLoops::ExtractSubloops( std::vector<int>& LoopV, std::vector<int>& LoopE,
                                          std::vector<int>& Bowties, FSubloops& SubloopsOut )
{
    FSubloops& Subs = SubloopsOut;

    // only the bowties the loop passes more than once split it
    std::vector<int> Dupes;
    for ( int Bv : Bowties )
        if ( CountInList( LoopV, Bv ) > 1 )
            Dupes.push_back( Bv );

    if ( Dupes.empty() )
    {
        Subs.Loops.push_back( FEdgeLoop{} );
        Subs.Loops[static_cast<int32_t>( Subs.Loops.size() ) - 1].Initialize( LoopV, LoopE, &Bowties );
        return true;
    }

    while ( !Dupes.empty() )
    {
        int Bv         = 0;
        int StartI     = -1;
        int EndI       = -1;
        int BvShortest = -1;
        int Shortest   = std::numeric_limits<int>::max();
        for ( int Bi = 0; Bi < static_cast<int32_t>( Dupes.size() ); ++Bi )
        {
            Bv = Dupes[Bi];
            if ( IsSimpleBowtieLoop( LoopV, Dupes, Bv, StartI, EndI ) )
            {
                const int Len = CountSpan( LoopV, StartI, EndI );
                if ( Len < Shortest )
                {
                    BvShortest = Bv;
                    Shortest   = Len;
                }
            }
        }
        if ( BvShortest == -1 )
        {
            // no simple sub-loop: what is left becomes a span
            VerticesTemp.clear();
            for ( int i = 0; i < static_cast<int32_t>( LoopV.size() ); ++i )
                if ( LoopV[i] != -1 )
                    VerticesTemp.push_back( LoopV[i] );
            Subs.Spans.push_back( FEdgeSpan{} );
            FEdgeSpan& NewSpan = Subs.Spans[static_cast<int32_t>( Subs.Spans.size() ) - 1];
            NewSpan.InitializeFromVertices( *Mesh, VerticesTemp );
            NewSpan.BowtieVertices = Bowties;
            return false;
        }
        if ( Bv != BvShortest )
        {
            Bv = BvShortest;
            IsSimpleBowtieLoop( LoopV, Dupes, Bv, StartI, EndI );
        }
        UE_CHECK( LoopV[StartI] == Bv && LoopV[EndI] == Bv );

        VerticesTemp.clear();
        ExtractSpan( LoopV, StartI, EndI, true, VerticesTemp );
        Subs.Loops.push_back( FEdgeLoop{} );
        FEdgeLoop& NewLoop = Subs.Loops[static_cast<int32_t>( Subs.Loops.size() ) - 1];
        NewLoop.InitializeFromVertices( *Mesh, VerticesTemp );
        NewLoop.BowtieVertices = Bowties;

        if ( CountInList( LoopV, Bv ) < 2 )
            std::erase( Dupes, Bv );
    }

    VerticesTemp.clear();
    for ( int i = 0; i < static_cast<int32_t>( LoopV.size() ); ++i )
        if ( LoopV[i] != -1 )
            VerticesTemp.push_back( LoopV[i] );
    if ( !VerticesTemp.empty() )
    {
        Subs.Loops.push_back( FEdgeLoop{} );
        FEdgeLoop& NewLoop = Subs.Loops[static_cast<int32_t>( Subs.Loops.size() ) - 1];
        NewLoop.InitializeFromVertices( *Mesh, VerticesTemp );
        NewLoop.BowtieVertices = Bowties;
    }
    return true;
}

bool FMeshBoundaryLoops::IsSimpleBowtieLoop( const std::vector<int>& LoopVerts,
                                             const std::vector<int>& BowtieVerts, int BowtieVertex, int& StartI,
                                             int& EndI )
{
    StartI = FindIndex( LoopVerts, 0, BowtieVertex );
    EndI   = FindIndex( LoopVerts, StartI + 1, BowtieVertex );
    if ( IsSimplePath( LoopVerts, BowtieVerts, BowtieVertex, StartI, EndI ) )
        return true;
    if ( IsSimplePath( LoopVerts, BowtieVerts, BowtieVertex, EndI, StartI ) )
    {
        std::swap( StartI, EndI );
        return true;
    }
    return false;
}

bool FMeshBoundaryLoops::IsSimplePath( const std::vector<int>& LoopVerts, const std::vector<int>& BowtieVerts,
                                       int BowtieVertex, int I1, int I2 )
{
    const int N = static_cast<int32_t>( LoopVerts.size() );
    for ( int i = I1; i != I2; i = ( i + 1 ) % N )
    {
        const int Vi = LoopVerts[i];
        if ( Vi == -1 )
            continue; // removed
        if ( Vi != BowtieVertex &&
             ( std::find( BowtieVerts.begin(), BowtieVerts.end(), Vi ) != BowtieVerts.end() ) )
            return false;
    }
    return true;
}

void FMeshBoundaryLoops::ExtractSpan( std::vector<int>& Loop, int I0, int I1, bool bMarkInvalid,
                                      std::vector<int>& OutSpan )
{
    OutSpan.resize( CountSpan( Loop, I0, I1 ) );
    int       Ai = 0;
    const int N  = static_cast<int32_t>( Loop.size() );
    for ( int i = I0; i != I1; i = ( i + 1 ) % N )
    {
        if ( Loop[i] != -1 )
        {
            OutSpan[Ai++] = Loop[i];
            if ( bMarkInvalid )
                Loop[i] = -1;
        }
    }
}

int FMeshBoundaryLoops::CountSpan( const std::vector<int>& Loop, int I0, int I1 )
{
    int       C = 0;
    const int N = static_cast<int32_t>( Loop.size() );
    for ( int i = I0; i != I1; i = ( i + 1 ) % N )
        if ( Loop[i] != -1 )
            ++C;
    return C;
}

int FMeshBoundaryLoops::FindIndex( const std::vector<int>& Loop, int Start, int Item )
{
    for ( int i = Start; i < static_cast<int32_t>( Loop.size() ); ++i )
        if ( Loop[i] == Item )
            return i;
    return -1;
}

int FMeshBoundaryLoops::CountInList( const std::vector<int>& Loop, int Item )
{
    int C = 0;
    for ( int i = 0; i < static_cast<int32_t>( Loop.size() ); ++i )
        if ( Loop[i] == Item )
            ++C;
    return C;
}
