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
    double PlaneAngleSignedD( const FVector3d& VFrom, const FVector3d& VTo, const FVector3d& PlaneN )
    {
        FVector3d From = VFrom - VFrom.Dot( PlaneN ) * PlaneN;
        FVector3d To   = VTo - VTo.Dot( PlaneN ) * PlaneN;
        Normalize( From );
        Normalize( To );
        const FVector3d C = From.Cross( To );
        if ( C.SquaredLength() < FMathd::ZeroTolerance )
            return From.Dot( To ) < 0 ? 180.0 : 0.0;
        const double Sign = C.Dot( PlaneN ) < 0 ? -1.0 : 1.0;
        const double Angle =
             std::acos( std::clamp( From.Dot( To ), -1.0, 1.0 ) ) * ( 180.0 / 3.14159265358979323846 );
        return Sign * Angle;
    }

    void ReverseInPlace( TArray<int>& Values )
    {
        for ( int i = 0, j = Values.Num() - 1; i < j; ++i, --j )
            std::swap( Values[i], Values[j] );
    }
} // namespace

int FMeshBoundaryLoops::FindLoopContainingVertex( int VertexID ) const
{
    for ( int li = 0; li < Loops.Num(); ++li )
        if ( Loops[li].Vertices.Contains( VertexID ) )
            return li;
    return -1;
}

int FMeshBoundaryLoops::FindLoopContainingEdge( int EdgeID ) const
{
    for ( int li = 0; li < Loops.Num(); ++li )
        if ( Loops[li].Edges.Contains( EdgeID ) )
            return li;
    return -1;
}

bool FMeshBoundaryLoops::Compute()
{
    // Triangles are assumed consistently oriented, so a closed boundary loop is followed by walking edges in
    // order.
    Loops.Reset();
    Spans.Reset();
    bSawOpenSpans = bFellBackToSpansOnFailure = false;
    if ( Mesh->IsClosed() )
        return true;

    const int    NE = Mesh->MaxEdgeID();
    TArray<bool> UsedEdge;
    UsedEdge.Init( false, NE );
    TArray<int> LoopEdges;
    TArray<int> LoopVerts;
    TArray<int> Bowties;
    TArray<int> AllE;

    for ( int Eid = 0; Eid < NE; ++Eid )
    {
        if ( !Mesh->IsEdge( Eid ) || UsedEdge[Eid] || !Mesh->IsBoundaryEdge( Eid ) )
            continue;

        const int EStart = Eid;
        UsedEdge[EStart] = true;
        LoopEdges.Add( EStart );
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
                LoopVerts.Add( Ev.A );

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
                    AllE.Reset();
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
                if ( !Bowties.Contains( CureB ) )
                    Bowties.Add( CureB );
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
                LoopEdges.Add( ENext );
                UsedEdge[ENext] = true;
                ECur            = ENext;
            }
        }

        if ( bIsOpenSpan )
        {
            bSawOpenSpans = true;
            ReverseInPlace( LoopEdges );
            Spans.Add( FEdgeSpan{} );
            Spans[Spans.Num() - 1].InitializeFromEdges( *Mesh, LoopEdges );
        }
        else if ( Bowties.Num() > 0 )
        {
            FSubloops Subloops;
            if ( !ExtractSubloops( LoopVerts, LoopEdges, Bowties, Subloops ) )
            {
                if ( Subloops.Spans.Num() > 0 )
                {
                    bFellBackToSpansOnFailure = true;
                    for ( const FEdgeSpan& Span : Subloops.Spans )
                        Spans.Add( Span );
                }
            }
            else
            {
                for ( const FEdgeLoop& Loop : Subloops.Loops )
                    Loops.Add( Loop );
            }
        }
        else
        {
            Loops.Add( FEdgeLoop{} );
            Loops[Loops.Num() - 1].Initialize( LoopVerts, LoopEdges );
        }
        LoopEdges.Reset();
        LoopVerts.Reset();
        Bowties.Reset();
    }
    return true;
}

FVector3d FMeshBoundaryLoops::GetVertexNormal( int Vid ) const
{
    FVector3d N = FVector3d::Zero();
    for ( int Ti : Mesh->VtxTrianglesItr( Vid ) )
        N += Mesh->GetTriNormal( Ti );
    Normalize( N );
    return N;
}

int FMeshBoundaryLoops::FindLeftTurnEdge( int IncomingE, int BowtieV, const TArray<int>& BdryEdges,
                                          int BdryEdgesCount, const TArray<bool>& UsedEdges ) const
{
    // the normal at the bowtie vertex is the plane the turn angles are measured in
    const FVector3d N      = GetVertexNormal( BowtieV );
    const FIndex2i  Ev     = Mesh->GetEdgeV( IncomingE );
    const int       OtherV = ( Ev.A == BowtieV ) ? Ev.B : Ev.A;
    const FVector3d Ab     = Mesh->GetVertex( BowtieV ) - Mesh->GetVertex( OtherV );

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
        const FVector3d Bc     = Mesh->GetVertex( BdryEv.B ) - Mesh->GetVertex( BowtieV );
        const double    AngleS = -PlaneAngleSignedD( Ab, Bc, N );
        if ( BestAngle == std::numeric_limits<double>::max() || AngleS < BestAngle )
        {
            BestAngle = AngleS;
            BestE     = BdryEid;
        }
    }
    return BestE;
}

bool FMeshBoundaryLoops::ExtractSubloops( TArray<int>& LoopV, TArray<int>& LoopE, TArray<int>& Bowties,
                                          FSubloops& SubloopsOut )
{
    FSubloops& Subs = SubloopsOut;

    // only the bowties the loop passes more than once split it
    TArray<int> Dupes;
    for ( int Bv : Bowties )
        if ( CountInList( LoopV, Bv ) > 1 )
            Dupes.Add( Bv );

    if ( Dupes.Num() == 0 )
    {
        Subs.Loops.Add( FEdgeLoop{} );
        Subs.Loops[Subs.Loops.Num() - 1].Initialize( LoopV, LoopE, &Bowties );
        return true;
    }

    while ( Dupes.Num() > 0 )
    {
        int Bv         = 0;
        int StartI     = -1;
        int EndI       = -1;
        int BvShortest = -1;
        int Shortest   = std::numeric_limits<int>::max();
        for ( int Bi = 0; Bi < Dupes.Num(); ++Bi )
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
            VerticesTemp.Reset();
            for ( int i = 0; i < LoopV.Num(); ++i )
                if ( LoopV[i] != -1 )
                    VerticesTemp.Add( LoopV[i] );
            Subs.Spans.Add( FEdgeSpan{} );
            FEdgeSpan& NewSpan = Subs.Spans[Subs.Spans.Num() - 1];
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

        VerticesTemp.Reset();
        ExtractSpan( LoopV, StartI, EndI, true, VerticesTemp );
        Subs.Loops.Add( FEdgeLoop{} );
        FEdgeLoop& NewLoop = Subs.Loops[Subs.Loops.Num() - 1];
        NewLoop.InitializeFromVertices( *Mesh, VerticesTemp );
        NewLoop.BowtieVertices = Bowties;

        if ( CountInList( LoopV, Bv ) < 2 )
            Dupes.Remove( Bv );
    }

    VerticesTemp.Reset();
    for ( int i = 0; i < LoopV.Num(); ++i )
        if ( LoopV[i] != -1 )
            VerticesTemp.Add( LoopV[i] );
    if ( VerticesTemp.Num() > 0 )
    {
        Subs.Loops.Add( FEdgeLoop{} );
        FEdgeLoop& NewLoop = Subs.Loops[Subs.Loops.Num() - 1];
        NewLoop.InitializeFromVertices( *Mesh, VerticesTemp );
        NewLoop.BowtieVertices = Bowties;
    }
    return true;
}

bool FMeshBoundaryLoops::IsSimpleBowtieLoop( const TArray<int>& LoopVerts, const TArray<int>& BowtieVerts,
                                             int BowtieVertex, int& StartI, int& EndI )
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

bool FMeshBoundaryLoops::IsSimplePath( const TArray<int>& LoopVerts, const TArray<int>& BowtieVerts,
                                       int BowtieVertex, int I1, int I2 )
{
    const int N = LoopVerts.Num();
    for ( int i = I1; i != I2; i = ( i + 1 ) % N )
    {
        const int Vi = LoopVerts[i];
        if ( Vi == -1 )
            continue; // removed
        if ( Vi != BowtieVertex && BowtieVerts.Contains( Vi ) )
            return false;
    }
    return true;
}

void FMeshBoundaryLoops::ExtractSpan( TArray<int>& Loop, int I0, int I1, bool bMarkInvalid, TArray<int>& OutSpan )
{
    OutSpan.SetNum( CountSpan( Loop, I0, I1 ) );
    int       Ai = 0;
    const int N  = Loop.Num();
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

int FMeshBoundaryLoops::CountSpan( const TArray<int>& Loop, int I0, int I1 )
{
    int       C = 0;
    const int N = Loop.Num();
    for ( int i = I0; i != I1; i = ( i + 1 ) % N )
        if ( Loop[i] != -1 )
            ++C;
    return C;
}

int FMeshBoundaryLoops::FindIndex( const TArray<int>& Loop, int Start, int Item )
{
    for ( int i = Start; i < Loop.Num(); ++i )
        if ( Loop[i] == Item )
            return i;
    return -1;
}

int FMeshBoundaryLoops::CountInList( const TArray<int>& Loop, int Item )
{
    int C = 0;
    for ( int i = 0; i < Loop.Num(); ++i )
        if ( Loop[i] == Item )
            ++C;
    return C;
}
