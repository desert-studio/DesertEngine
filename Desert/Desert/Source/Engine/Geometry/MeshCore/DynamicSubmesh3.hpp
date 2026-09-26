// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicSubmesh3.h (Compute, MapVertexToBaseMesh,
// MapTriangleToBaseMesh), adapted: geometry only - UE's WantComponents / bAttributes / border-edge sets and
// DynamicMeshEditor::AppendTriangles are not ported, the only caller (the ExpMap UV editor) asks for
// MeshComponents::None without attributes. The maps are flat arrays: submesh IDs are appended densely.
#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/MapLookup.hpp"

#include <unordered_map>

namespace Desert::Geometry
{
    class DynamicSubmesh3
    {
    public:
        DynamicSubmesh3( const DynamicMesh3* BaseMeshIn, const std::vector<int32_t>& Triangles )
             : m_BaseMesh( BaseMeshIn )
        {
            std::unordered_map<int32_t, int32_t> BaseToSubV;
            for ( const int32_t BaseTID : Triangles )
            {
                const Index3i BaseTri = m_BaseMesh->GetTriangle( BaseTID );
                Index3i       SubTri;
                for ( int32_t j = 0; j < 3; ++j )
                {
                    if ( const int32_t* Found = FindValue( BaseToSubV, BaseTri[j] ) )
                    {
                        SubTri[j] = *Found;
                        continue;
                    }
                    SubTri[j] = m_Submesh.AppendVertex( m_BaseMesh->GetVertex( BaseTri[j] ) );
                    BaseToSubV.insert_or_assign( BaseTri[j], SubTri[j] );
                    m_SubToBaseV.push_back( BaseTri[j] );
                }
                const int32_t SubTID = m_Submesh.AppendTriangle( SubTri );
                if ( SubTID < 0 )
                {
                    m_FailedTriangles.push_back( BaseTID );
                    continue;
                }
                m_SubToBaseT.push_back( BaseTID );
            }
        }
        DynamicMesh3& GetSubmesh()
        {
            return m_Submesh;
        }
        int32_t MapVertexToBaseMesh( int32_t SubVID ) const
        {
            return m_SubToBaseV[SubVID];
        }
        int32_t MapTriangleToBaseMesh( int32_t SubTID ) const
        {
            return m_SubToBaseT[SubTID];
        }
        /** Base triangles AppendTriangle refused (non-manifold within the set); UE check()s this away. */
        const std::vector<int32_t>& GetFailedTriangles() const
        {
            return m_FailedTriangles;
        }

    private:
        const DynamicMesh3*  m_BaseMesh;
        DynamicMesh3         m_Submesh;
        std::vector<int32_t> m_SubToBaseV;
        std::vector<int32_t> m_SubToBaseT;
        std::vector<int32_t> m_FailedTriangles;
    };
} // namespace Desert::Geometry
