// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Selections/QuadGridPatch.h and
// Private/Selections/QuadGridPatch.cpp:86-174,269-296, adapted: UE Core via UECore.hpp, namespace
// Desert::Geometry. Only what the multi-segment MeshBevel reads is ported: InitializeFromQuadPatch,
// GetVertexColumn, FindColumnIndex and the size queries. The strip, sub-patch, split, append, reverse and triangle
// list helpers have no caller here and are not carried.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

namespace Desert::Geometry
{
    /**
     * A grid of quads in a mesh, each quad a pair of triangles: NumVertexRowsV rows of NumVertexColsU vertex IDs
     * and (NumVertexRowsV-1) rows of (NumVertexColsU-1) quads between them.
     */
    class QuadGridPatch
    {
    public:
        int m_NumVertexColsU = 0;
        int m_NumVertexRowsV = 0;

        /** NumVertexRowsV rows of NumVertexColsU VertexIDs, may contain repeated element if the patch forms a loop
         */
        std::vector<std::vector<int32_t>> m_VertexSpans;

        /** Quads stored as pairs of triangle indices, (NumVertexRowsV-1) rows of (NumVertexColsU-1) */
        std::vector<std::vector<Index2i>> m_QuadTriangles;

        [[nodiscard]] int NumVertexCols() const
        {
            return m_NumVertexColsU;
        }
        [[nodiscard]] int NumVertexRows() const
        {
            return m_NumVertexRowsV;
        }
        [[nodiscard]] bool IsEmpty() const
        {
            return m_NumVertexColsU == 0 || m_NumVertexRowsV == 0;
        }

        /**
         * Initialize from rows of quads and the rows of vertices around them. Each quad must be exactly the four
         * vertices at its grid corners; its first triangle is swapped to be the one on the lower row's edge. On a
         * mismatch the patch is left empty and false is returned.
         */
        bool InitializeFromQuadPatch( const DynamicMesh3&                      Mesh,
                                      const std::vector<std::vector<Index2i>>& QuadRowsIn,
                                      const std::vector<std::vector<int32_t>>& VertexSpansIn );

        /** The vertices of column ColumnIndex, one per row; false for an index outside the patch. */
        bool GetVertexColumn( int32_t ColumnIndex, std::vector<int32_t>& VerticesOut ) const;

        /** Find column that contains vertex, or InvalidID */
        [[nodiscard]] int32_t FindColumnIndex( int32_t VertexID ) const;
    };
} // namespace Desert::Geometry
