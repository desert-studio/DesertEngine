// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/MeshBoundaryLoops.h:1-205 and
// Private/MeshBoundaryLoops.cpp:62-90 (FindLoopContainingVertex/Edge), 92-366 (Compute), 368-449 (GetVertexNormal,
// FindLeftTurnEdge), 450-686 (ExtractSubloops and its span helpers), adapted: namespace Desert::Geometry, UE Core
// via UECore.hpp, FEdgeLoop carries no mesh pointer. Not ported: EdgeFilterFunc, SpanBehavior, FailureBehavior and
// bOnlyComputeSpans - no caller sets them, so Compute runs UE's defaults (open spans are computed, a failed walk
// is kept as a span, never an abort); the loop-index queries nothing here calls (GetMaxVerticesLoopIndex,
// GetLongestLoopIndex, FindVertexInLoop, FindLoop*Hint, FindSpanContainingEdge).
#pragma once

#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"

namespace Desert::Geometry
{
    /** Extracts every boundary loop of a mesh, oriented as its boundary edges are. */
    class FMeshBoundaryLoops
    {
    public:
        const FDynamicMesh3* Mesh = nullptr;
        TArray<FEdgeLoop>    Loops;
        TArray<FEdgeSpan>    Spans;
        /** At least one open span was found (a failed walk becomes one). */
        bool bSawOpenSpans = false;
        /** A loop with bowties could not be split into simple loops and was kept as a span. */
        bool bFellBackToSpansOnFailure = false;

        explicit FMeshBoundaryLoops( const FDynamicMesh3* MeshIn, bool bAutoCompute = true ) : Mesh( MeshIn )
        {
            if ( bAutoCompute )
                Compute();
        }

        bool Compute();

        int GetLoopCount() const
        {
            return Loops.Num();
        }
        int FindLoopContainingVertex( int VertexID ) const;
        int FindLoopContainingEdge( int EdgeID ) const;

    private:
        struct FSubloops
        {
            TArray<FEdgeLoop> Loops;
            TArray<FEdgeSpan> Spans;
        };
        TArray<int> VerticesTemp;

        glm::dvec3 GetVertexNormal( int Vid ) const;
        int       FindLeftTurnEdge( int IncomingE, int BowtieV, const TArray<int>& BdryEdges, int BdryEdgesCount,
                                    const TArray<bool>& UsedEdges ) const;
        bool      ExtractSubloops( TArray<int>& LoopV, TArray<int>& LoopE, TArray<int>& Bowties,
                                   FSubloops& SubloopsOut );

        static bool IsSimpleBowtieLoop( const TArray<int>& LoopVerts, const TArray<int>& BowtieVerts,
                                        int BowtieVertex, int& StartI, int& EndI );
        static bool IsSimplePath( const TArray<int>& LoopVerts, const TArray<int>& BowtieVerts, int BowtieVertex,
                                  int I1, int I2 );
        static void ExtractSpan( TArray<int>& Loop, int I0, int I1, bool bMarkInvalid, TArray<int>& OutSpan );
        static int  CountSpan( const TArray<int>& Loop, int I0, int I1 );
        static int  FindIndex( const TArray<int>& Loop, int Start, int Item );
        static int  CountInList( const TArray<int>& Loop, int Item );
    };
} // namespace Desert::Geometry
