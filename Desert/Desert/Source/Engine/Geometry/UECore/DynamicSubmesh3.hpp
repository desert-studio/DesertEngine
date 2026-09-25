// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicSubmesh3.h (Compute, MapVertexToBaseMesh,
// MapTriangleToBaseMesh), adapted: geometry only - UE's WantComponents / bAttributes / border-edge sets and
// FDynamicMeshEditor::AppendTriangles are not ported, the only caller (the ExpMap UV editor) asks for
// EMeshComponents::None without attributes. The maps are flat arrays: submesh IDs are appended densely.
#pragma once

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

namespace Desert::Geometry
{
    class FDynamicSubmesh3
    {
    public:
        FDynamicSubmesh3( const FDynamicMesh3* BaseMeshIn, const TArray<int32>& Triangles )
             : BaseMesh( BaseMeshIn )
        {
            TMap<int32, int32> BaseToSubV;
            for ( const int32 BaseTID : Triangles )
            {
                const FIndex3i BaseTri = BaseMesh->GetTriangle( BaseTID );
                FIndex3i       SubTri;
                for ( int32 j = 0; j < 3; ++j )
                {
                    if ( const int32* Found = BaseToSubV.Find( BaseTri[j] ) )
                    {
                        SubTri[j] = *Found;
                        continue;
                    }
                    SubTri[j] = Submesh.AppendVertex( BaseMesh->GetVertex( BaseTri[j] ) );
                    BaseToSubV.Add( BaseTri[j], SubTri[j] );
                    SubToBaseV.Add( BaseTri[j] );
                }
                const int32 SubTID = Submesh.AppendTriangle( SubTri );
                if ( SubTID < 0 )
                {
                    FailedTriangles.Add( BaseTID );
                    continue;
                }
                SubToBaseT.Add( BaseTID );
            }
        }
        FDynamicMesh3& GetSubmesh()
        {
            return Submesh;
        }
        int32 MapVertexToBaseMesh( int32 SubVID ) const
        {
            return SubToBaseV[SubVID];
        }
        int32 MapTriangleToBaseMesh( int32 SubTID ) const
        {
            return SubToBaseT[SubTID];
        }
        /** Base triangles AppendTriangle refused (non-manifold within the set); UE check()s this away. */
        const TArray<int32>& GetFailedTriangles() const
        {
            return FailedTriangles;
        }

    private:
        const FDynamicMesh3* BaseMesh;
        FDynamicMesh3        Submesh;
        TArray<int32>        SubToBaseV;
        TArray<int32>        SubToBaseT;
        TArray<int32>        FailedTriangles;
    };
} // namespace Desert::Geometry
