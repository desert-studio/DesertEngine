// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/MeshBoundaryLoops.h:1-205 and
// Private/MeshBoundaryLoops.cpp:62-90 (FindLoopContainingVertex/Edge), 92-366 (Compute), 368-449 (GetVertexNormal,
// FindLeftTurnEdge), 450-686 (ExtractSubloops and its span helpers), adapted: namespace Desert::Geometry, UE Core
// via UECore.hpp, EdgeLoop carries no mesh pointer. Not ported: EdgeFilterFunc, SpanBehavior, FailureBehavior and
// bOnlyComputeSpans - no caller sets them, so Compute runs UE's defaults (open spans are computed, a failed walk
// is kept as a span, never an abort); the loop-index queries nothing here calls (GetMaxVerticesLoopIndex,
// GetLongestLoopIndex, FindVertexInLoop, FindLoop*Hint, FindSpanContainingEdge).
#pragma once

#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"

namespace Desert::Geometry
{
    /** Extracts every boundary loop of a mesh, oriented as its boundary edges are. */
    class MeshBoundaryLoops
    {
    public:
        const DynamicMesh3*   m_Mesh = nullptr;
        std::vector<EdgeLoop> m_Loops;
        std::vector<EdgeSpan> m_Spans;
        /** At least one open span was found (a failed walk becomes one). */
        bool m_bSawOpenSpans = false;
        /** A loop with bowties could not be split into simple loops and was kept as a span. */
        bool m_bFellBackToSpansOnFailure = false;

        explicit MeshBoundaryLoops( const DynamicMesh3* MeshIn, bool bAutoCompute = true ) : m_Mesh( MeshIn )
        {
            if ( bAutoCompute )
                Compute();
        }

        bool Compute();

        [[nodiscard]] int GetLoopCount() const
        {
            return static_cast<int32_t>( m_Loops.size() );
        }
        [[nodiscard]] int FindLoopContainingVertex( int VertexID ) const;
        [[nodiscard]] int FindLoopContainingEdge( int EdgeID ) const;

    private:
        struct Subloops
        {
            std::vector<EdgeLoop> Loops;
            std::vector<EdgeSpan> Spans;
        };
        std::vector<int> m_VerticesTemp;

        [[nodiscard]] glm::dvec3 GetVertexNormal( int Vid ) const;
        [[nodiscard]] int        FindLeftTurnEdge( int IncomingE, int BowtieV, const std::vector<int>& BdryEdges,
                                                   int BdryEdgesCount, const std::vector<bool>& UsedEdges ) const;
        bool ExtractSubloops( std::vector<int>& LoopV, std::vector<int>& LoopE, std::vector<int>& Bowties,
                              Subloops& SubloopsOut );

        static bool IsSimpleBowtieLoop( const std::vector<int>& LoopVerts, const std::vector<int>& BowtieVerts,
                                        int BowtieVertex, int& StartI, int& EndI );
        static bool IsSimplePath( const std::vector<int>& LoopVerts, const std::vector<int>& BowtieVerts,
                                  int BowtieVertex, int I1, int I2 );
        static void ExtractSpan( std::vector<int>& Loop, int I0, int I1, bool bMarkInvalid,
                                 std::vector<int>& OutSpan );
        static int  CountSpan( const std::vector<int>& Loop, int I0, int I1 );
        static int  FindIndex( const std::vector<int>& Loop, int Start, int Item );
        static int  CountInList( const std::vector<int>& Loop, int Item );
    };
} // namespace Desert::Geometry
