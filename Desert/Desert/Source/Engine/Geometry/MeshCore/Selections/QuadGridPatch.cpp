// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Selections/QuadGridPatch.cpp:
// 86-174,269-296, adapted: see QuadGridPatch.hpp; UE's ensure(false) on a bad input is the false return alone.
#include "Engine/Geometry/MeshCore/Selections/QuadGridPatch.hpp"

#include <utility>

namespace Desert::Geometry
{
    bool QuadGridPatch::InitializeFromQuadPatch( const DynamicMesh3&                      Mesh,
                                                 const std::vector<std::vector<Index2i>>& QuadRowsIn,
                                                 const std::vector<std::vector<int32_t>>& VertexSpansIn )
    {
        if ( static_cast<int32_t>( VertexSpansIn.size() ) < 2 ||
             static_cast<int32_t>( QuadRowsIn.size() ) != static_cast<int32_t>( VertexSpansIn.size() ) - 1 )
            return false;
        const auto NumV = static_cast<int32_t>( VertexSpansIn[0].size() );
        const auto NumQ = static_cast<int32_t>( QuadRowsIn[0].size() );
        if ( NumQ != NumV - 1 )
            return false;
        for ( int32_t j = 1; j < static_cast<int32_t>( VertexSpansIn.size() ); ++j )
        {
            if ( static_cast<int32_t>( VertexSpansIn[j].size() ) != NumV )
                return false;
        }
        const auto NumQuadRows = static_cast<int32_t>( QuadRowsIn.size() );
        for ( int32_t j = 1; j < NumQuadRows; ++j )
        {
            if ( static_cast<int32_t>( QuadRowsIn[j].size() ) != NumQ )
                return false;
        }
        m_NumVertexColsU = NumV;
        m_NumVertexRowsV = static_cast<int32_t>( VertexSpansIn.size() );
        m_VertexSpans    = VertexSpansIn;
        m_QuadTriangles  = QuadRowsIn;

        bool bAllOK = true;
        for ( int32_t j = 0; j < NumQuadRows && bAllOK; ++j )
        {
            for ( int32_t k = 0; k < NumQ && bAllOK; ++k )
            {
                // these should be the four vertices of the quad
                const int32_t VertexA  = m_VertexSpans[j][k];
                const int32_t VertexB  = m_VertexSpans[j][k + 1];
                const int32_t VertexC  = m_VertexSpans[j + 1][k + 1];
                const int32_t VertexD  = m_VertexSpans[j + 1][k];
                Index2i&      QuadTris = m_QuadTriangles[j][k];
                if ( !Mesh.IsTriangle( QuadTris.A ) || !Mesh.IsTriangle( QuadTris.B ) )
                {
                    bAllOK = false;
                    break;
                }
                const Index3i        TriA = Mesh.GetTriangle( QuadTris.A );
                const Index3i        TriB = Mesh.GetTriangle( QuadTris.B );
                std::vector<int32_t> TriVerts;
                TriVerts.push_back( TriA.A );
                TriVerts.push_back( TriA.B );
                TriVerts.push_back( TriA.C );
                if ( std::find( TriVerts.begin(), TriVerts.end(), TriB.A ) == TriVerts.end() )
                {
                    TriVerts.push_back( TriB.A );
                }
                if ( std::find( TriVerts.begin(), TriVerts.end(), TriB.B ) == TriVerts.end() )
                {
                    TriVerts.push_back( TriB.B );
                }
                if ( std::find( TriVerts.begin(), TriVerts.end(), TriB.C ) == TriVerts.end() )
                {
                    TriVerts.push_back( TriB.C );
                }
                if ( static_cast<int32_t>( TriVerts.size() ) != 4 ||
                     !( std::find( TriVerts.begin(), TriVerts.end(), VertexA ) != TriVerts.end() ) ||
                     !( std::find( TriVerts.begin(), TriVerts.end(), VertexB ) != TriVerts.end() ) ||
                     !( std::find( TriVerts.begin(), TriVerts.end(), VertexC ) != TriVerts.end() ) ||
                     !( std::find( TriVerts.begin(), TriVerts.end(), VertexD ) != TriVerts.end() ) )
                {
                    bAllOK = false;
                    break;
                }
                // we want TriA to be the one containing the edge (Vertex0,Vertex1), so if it's TriB, swap the quad
                if ( TriB.Contains( VertexA ) && TriB.Contains( VertexB ) )
                    std::swap( QuadTris.A, QuadTris.B );
            }
        }
        if ( !bAllOK )
        {
            m_VertexSpans.clear();
            m_QuadTriangles.clear();
            m_NumVertexColsU = 0;
            m_NumVertexRowsV = 0;
            return false;
        }
        return true;
    }

    bool QuadGridPatch::GetVertexColumn( int32_t ColumnIndex, std::vector<int32_t>& VerticesOut ) const
    {
        if ( m_VertexSpans.empty() || ColumnIndex < 0 ||
             ColumnIndex >= static_cast<int32_t>( m_VertexSpans[0].size() ) )
            return false;
        VerticesOut.clear();
        for ( const auto& m_VertexSpan : m_VertexSpans )
            VerticesOut.push_back( m_VertexSpan[ColumnIndex] );
        return true;
    }

    int32_t QuadGridPatch::FindColumnIndex( int32_t VertexID ) const
    {
        for ( const std::vector<int32_t>& Span : m_VertexSpans )
        {
            for ( int32_t Index = 0; Index < static_cast<int32_t>( Span.size() ); ++Index )
            {
                if ( Span[Index] == VertexID )
                    return Index;
            }
        }
        return IndexConstants::InvalidID;
    }
} // namespace Desert::Geometry
