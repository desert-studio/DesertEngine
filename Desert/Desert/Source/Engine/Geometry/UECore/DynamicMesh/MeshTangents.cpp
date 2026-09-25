// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/MeshTangents.cpp:186-235
// (ComputeFaceTangent), 295-345 (CopyToOverlays, PlaneProjectionNormalized), 356-391
// (ComputeSeparatePerTriangleTangents), adapted: UE Core via UECore.hpp, namespace Desert::Geometry, ParallelFor
// is the UECore.hpp serial shim, FVector2f -> FVector2d by components (UECore's TVector2 has no converting
// constructor), instantiated for double only.
#include "Engine/Geometry/UECore/DynamicMesh/MeshTangents.hpp"

#include "Engine/Geometry/UECore/VectorUtil.hpp"

using namespace Desert::Geometry;

namespace
{
    // Tangent / bitangent of one triangle from its positions and UVs.
    void ComputeFaceTangent( const FVector3d TriVertices[3], const FVector2f TriUVs[3], FVector3d& TangentOut,
                             FVector3d& BitangentOut, FVector2d& MagnitudesOut, double& OrientationSignOut,
                             bool& bIsDegenerateOut )
    {
        const FVector2d UVEdge1( (double)TriUVs[1].X - TriUVs[0].X, (double)TriUVs[1].Y - TriUVs[0].Y );
        const FVector2d UVEdge2( (double)TriUVs[2].X - TriUVs[0].X, (double)TriUVs[2].Y - TriUVs[0].Y );
        const FVector3d TriEdge1 = TriVertices[1] - TriVertices[0];
        const FVector3d TriEdge2 = TriVertices[2] - TriVertices[0];

        const FVector3d TriTangent   = ( UVEdge2.Y * TriEdge1 ) - ( UVEdge1.Y * TriEdge2 );
        const FVector3d TriBitangent = ( -UVEdge2.X * TriEdge1 ) + ( UVEdge1.X * TriEdge2 );

        double     UVArea               = ( UVEdge1.X * UVEdge2.Y ) - ( UVEdge1.Y * UVEdge2.X );
        const bool bPreserveOrientation = ( UVArea >= 0 );

        UVArea = FMathd::Abs( UVArea );

        // if a triangle is zero-UV-area due to one edge being collapsed, we still have a
        // valid direction on the other edge. We are going to keep those
        const double TriTangentLength   = TriTangent.Length();
        const double TriBitangentLength = TriBitangent.Length();
        TangentOut    = ( TriTangentLength > 0 ) ? ( TriTangent / TriTangentLength ) : FVector3d::Zero();
        BitangentOut  = ( TriBitangentLength > 0 ) ? ( TriBitangent / TriBitangentLength ) : FVector3d::Zero();
        MagnitudesOut = FVector2d( TriTangentLength / UVArea, TriBitangentLength / UVArea );

        if ( bPreserveOrientation )
        {
            OrientationSignOut = 1.0;
        }
        else
        {
            OrientationSignOut = -1.0;
            TangentOut         = -TangentOut;
            BitangentOut       = -BitangentOut;
        }

        bIsDegenerateOut = ( UVArea < FMathd::ZeroTolerance );
    }

    FVector3d PlaneProjectionNormalized( const FVector3d& Vector, const FVector3d& PlaneNormal )
    {
        return Normalized( Vector - Vector.Dot( PlaneNormal ) * PlaneNormal );
    }
} // namespace

namespace Desert::Geometry
{
    template <typename RealType>
    bool TMeshTangents<RealType>::CopyToOverlays( FDynamicMesh3& MeshToSet ) const
    {
        if ( !MeshToSet.HasAttributes() || MeshToSet.Attributes()->NumNormalLayers() != 3 )
        {
            return false;
        }

        // Set aliases to make iterating over tangents and bitangents easier
        FDynamicMeshNormalOverlay*       TangentOverlays[2] = { MeshToSet.Attributes()->PrimaryTangents(),
                                                                MeshToSet.Attributes()->PrimaryBiTangents() };
        const TArray<TVector<RealType>>* TangentValues[2]   = { &Tangents, &Bitangents };

        for ( int Idx = 0; Idx < 2; Idx++ )
        {
            // Create overlay topology
            const TArray<TVector<RealType>>& TV = *TangentValues[Idx];
            TangentOverlays[Idx]->CreateFromPredicate(
                 [&MeshToSet, &TV]( int ParentVertexIdx, int TriIDA, int TriIDB ) -> bool
                 {
                     const FIndex3i TriA = MeshToSet.GetTriangle( TriIDA );
                     const FIndex3i TriB = MeshToSet.GetTriangle( TriIDB );
                     const int      SubA = TriA.IndexOf( ParentVertexIdx );
                     const int      SubB = TriB.IndexOf( ParentVertexIdx );
                     UE_CHECK_SLOW( SubA > -1 && SubB > -1 );
                     const TVector<RealType>& A = TV[TriIDA * 3 + SubA];
                     const TVector<RealType>& B = TV[TriIDB * 3 + SubB];
                     return DistanceSquared( A, B ) < TMathUtil<RealType>::ZeroTolerance;
                 },
                 0.0f );

            // Write tangent values out for each wedge value
            // Note: shared elements will be written to multiple times, and the last value written will be used
            for ( int TID : MeshToSet.TriangleIndicesItr() )
            {
                const FIndex3i ElTri = TangentOverlays[Idx]->GetTriangle( TID );
                for ( int SubIdx = 0; SubIdx < 3; SubIdx++ )
                {
                    TangentOverlays[Idx]->SetElement( ElTri[SubIdx], (FVector3f)TV[TID * 3 + SubIdx] );
                }
            }
        }
        return true;
    }

    template <typename RealType>
    void
    TMeshTangents<RealType>::ComputeSeparatePerTriangleTangents( const FDynamicMeshNormalOverlay* NormalOverlay,
                                                                 const FDynamicMeshUVOverlay*     UVOverlay )
    {
        const int32 MaxTriangleID = Mesh->MaxTriangleID();
        InitializeTriVertexTangents( false );

        // compute per-triangle tangent and bitangent
        ParallelFor(
             MaxTriangleID,
             [&]( int32 TriangleID )
             {
                 if ( Mesh->IsTriangle( TriangleID ) == false || UVOverlay->IsSetTriangle( TriangleID ) == false )
                 {
                     return;
                 }

                 FVector3d TriVertices[3];
                 Mesh->GetTriVertices( TriangleID, TriVertices[0], TriVertices[1], TriVertices[2] );
                 FVector2f TriUVs[3];
                 UVOverlay->GetTriElements( TriangleID, TriUVs[0], TriUVs[1], TriUVs[2] );
                 FVector3f TriNormals[3];
                 NormalOverlay->GetTriElements( TriangleID, TriNormals[0], TriNormals[1], TriNormals[2] );

                 FVector3d Tangent, Bitangent;
                 FVector2d Magnitudes;
                 double    OrientationSign;
                 bool      bIsDegenerate;
                 ComputeFaceTangent( TriVertices, TriUVs, Tangent, Bitangent, Magnitudes, OrientationSign,
                                     bIsDegenerate );

                 for ( int32 j = 0; j < 3; ++j )
                 {
                     const FVector3d VtxNormal        = (FVector3d)TriNormals[j];
                     const FVector3d ProjectedTangent = PlaneProjectionNormalized( Tangent, VtxNormal );

                     const double BitangentSign =
                          VectorUtil::BitangentSign( VtxNormal, ProjectedTangent, Bitangent );
                     const FVector3d ReconsBitangent =
                          VectorUtil::Bitangent( VtxNormal, ProjectedTangent, BitangentSign );

                     SetPerTriangleTangent( TriangleID, j, Normalized( (TVector<RealType>)ProjectedTangent ),
                                            Normalized( (TVector<RealType>)ReconsBitangent ) );
                 }
             } );
    }

    template class TMeshTangents<double>;
} // namespace Desert::Geometry
