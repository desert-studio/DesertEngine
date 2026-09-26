// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicSubmesh3.h (Compute, MapVertexToBaseMesh,
// MapTriangleToBaseMesh), adapted: geometry only - UE's WantComponents / bAttributes / border-edge sets and
// DynamicMeshEditor::AppendTriangles are not ported, the only caller (the ExpMap UV editor) asks for
// MeshComponents::None without attributes. The maps are flat arrays: submesh IDs are appended densely.
#pragma once

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/MapLookup.hpp"

namespace Desert::Geometry
{
    class DynamicSubmesh3
    {
    public:
        DynamicSubmesh3( const DynamicMesh3* BaseMeshIn, const std::vector<int32_t>& Triangles )
             : BaseMesh( BaseMeshIn )
        {
            std::unordered_map<int32_t, int32_t> BaseToSubV;
            for ( const int32_t BaseTID : Triangles )
            {
                const Index3i BaseTri = BaseMesh->GetTriangle( BaseTID );
                Index3i       SubTri;
                for ( int32_t j = 0; j < 3; ++j )
                {
                    if ( const int32_t* Found = FindValue( BaseToSubV, BaseTri[j] ) )
                    {
                        SubTri[j] = *Found;
                        continue;
                    }
                    SubTri[j] = Submesh.AppendVertex( BaseMesh->GetVertex( BaseTri[j] ) );
                    BaseToSubV.insert_or_assign( BaseTri[j], SubTri[j] );
                    SubToBaseV.push_back( BaseTri[j] );
                }
                const int32_t SubTID = Submesh.AppendTriangle( SubTri );
                if ( SubTID < 0 )
                {
                    FailedTriangles.push_back( BaseTID );
                    continue;
                }
                SubToBaseT.push_back( BaseTID );
            }
        }
        DynamicMesh3& GetSubmesh()
        {
            return Submesh;
        }
        int32_t MapVertexToBaseMesh( int32_t SubVID ) const
        {
            return SubToBaseV[SubVID];
        }
        int32_t MapTriangleToBaseMesh( int32_t SubTID ) const
        {
            return SubToBaseT[SubTID];
        }
        /** Base triangles AppendTriangle refused (non-manifold within the set); UE check()s this away. */
        const std::vector<int32_t>& GetFailedTriangles() const
        {
            return FailedTriangles;
        }

    private:
        const DynamicMesh3*  BaseMesh;
        DynamicMesh3         Submesh;
        std::vector<int32_t> SubToBaseV;
        std::vector<int32_t> SubToBaseT;
        std::vector<int32_t> FailedTriangles;
    };
} // namespace Desert::Geometry
