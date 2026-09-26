// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/Operations/SplitAttributeWelder.cpp
// (see the header for the adaptations).
#include "Engine/Geometry/UECore/DynamicMesh/Operations/SplitAttributeWelder.hpp"

#include <cmath>

namespace Desert::Geometry
{
    namespace
    {
        template <typename OverlayType, typename ShouldWeldFunctorType>
        void WeldSplits( const FDynamicMesh3* ParentMesh, const int32_t ParentVID, OverlayType& Overlay,
                         ShouldWeldFunctorType& ShouldWeld )
        {
            if ( !ParentMesh || !ParentMesh->IsVertex( ParentVID ) )
                return;

            std::vector<int> ElementIDs;
            Overlay.GetVertexElements( ParentVID, ElementIDs );

            // the number of elements at one vertex is small: simple O(n^2), as UE
            const int32_t    NumElements = static_cast<int32_t>( ElementIDs.size() );
            std::vector<int> ConsumedMask;
            ConsumedMask.resize( NumElements );
            for ( int32_t i = 0; i < NumElements; ++i )
            {
                if ( ConsumedMask[i] == 1 )
                    continue;
                const int32_t eid = ElementIDs[i];
                for ( int32_t j = i + 1; j < NumElements; ++j )
                {
                    if ( ConsumedMask[j] == 1 )
                        continue;
                    const int32_t oeid = ElementIDs[j];
                    if ( !ShouldWeld( eid, oeid ) )
                        continue;
                    ConsumedMask[j] = 1;
                    for ( int TID : ParentMesh->VtxTrianglesItr( ParentVID ) )
                    {
                        if ( !Overlay.IsSetTriangle( TID ) )
                            continue;
                        FIndex3i TriElements     = Overlay.GetTriangle( TID );
                        bool     bUpdateTriangle = false;
                        for ( int c = 0; c < 3; ++c )
                        {
                            if ( TriElements[c] == oeid )
                            {
                                TriElements[c]  = eid;
                                bUpdateTriangle = true;
                            }
                        }
                        if ( bUpdateTriangle )
                            Overlay.SetTriangle( TID, TriElements, true /* allow element freeing */ );
                    }
                }
            }
        }
    } // namespace

    void FSplitAttributeWelder::WeldSplitElements( FDynamicMesh3& ParentMesh, const int32_t ParentVID )
    {
        FDynamicMeshAttributeSet* Attributes = ParentMesh.Attributes();
        if ( !Attributes || !ParentMesh.IsVertex( ParentVID ) )
            return;
        for ( int32_t i = 0, I = Attributes->NumUVLayers(); i < I; ++i )
            WeldSplitUVs( ParentVID, *Attributes->GetUVLayer( i ), UVDistSqrdThreshold );
        for ( int32_t i = 0, I = Attributes->NumNormalLayers(); i < I; ++i )
        {
            const float DotThreshold = ( i == 0 ) ? NormalVecDotThreshold : TangentVecDotThreshold;
            WeldSplitUnitVectors( ParentVID, *Attributes->GetNormalLayer( i ), DotThreshold );
        }
        if ( FDynamicMeshColorOverlay* Overlay = Attributes->PrimaryColors() )
            WeldSplitColors( ParentVID, *Overlay, ColorDistSqrdThreshold );
    }

    void FSplitAttributeWelder::WeldSplitElements( FDynamicMesh3& ParentMesh )
    {
        for ( int vid : ParentMesh.VertexIndicesItr() )
            WeldSplitElements( ParentMesh, vid );
    }

    void FSplitAttributeWelder::WeldSplitUVs( const int32_t ParentVID, FDynamicMeshUVOverlay& Overlay,
                                              float UVDistSqrdThreshold )
    {
        const FDynamicMesh3* ParentMesh = Overlay.GetParentMesh();
        const float          Threshold  = std::max( UVDistSqrdThreshold, 0.f );
        auto                 ShouldWeld = [&Overlay, Threshold]( const int32_t eid, const int32_t oeid ) -> bool
        {
            const glm::vec2 UV = Overlay.GetElement( eid ), otherUV = Overlay.GetElement( oeid );
            const float     dx = UV.x - otherUV.x;
            const float     dy = UV.y - otherUV.y;
            return dx * dx + dy * dy <= Threshold;
        };
        WeldSplits( ParentMesh, ParentVID, Overlay, ShouldWeld );
    }

    void FSplitAttributeWelder::WeldSplitUnitVectors( const int32_t ParentVID, FDynamicMeshNormalOverlay& Overlay,
                                                      float DotThreshold, bool bMergeZeroVectors )
    {
        const FDynamicMesh3* ParentMesh = Overlay.GetParentMesh();
        auto                 ShouldWeld = [&Overlay, DotThreshold, bMergeZeroVectors]( const int32_t eid,
                                                                       const int32_t oeid ) -> bool
        {
            // UE's FVector3f::Normalize: false (vector untouched) when the squared length is below
            // SMALL_NUMBER (1e-8)
            const auto Unit = []( glm::vec3 V, bool& bOk )
            {
                const float LenSq = V.x * V.x + V.y * V.y + V.z * V.z;
                bOk               = LenSq > 1e-8f;
                if ( bOk )
                {
                    const float Inv = 1.0f / std::sqrt( LenSq );
                    V.x *= Inv;
                    V.y *= Inv;
                    V.z *= Inv;
                }
                return V;
            };
            bool            bVecNormalized = false, bOtherVecNormalized = false;
            const glm::vec3 Vec      = Unit( Overlay.GetElement( eid ), bVecNormalized );
            const glm::vec3 otherVec = Unit( Overlay.GetElement( oeid ), bOtherVecNormalized );
            if ( bVecNormalized && bOtherVecNormalized )
            {
                const float CosAngle = Vec.x * otherVec.x + Vec.y * otherVec.y + Vec.z * otherVec.z;
                return std::abs( 1.f - CosAngle ) <= DotThreshold;
            }
            return bMergeZeroVectors && !bVecNormalized && !bOtherVecNormalized;
        };
        WeldSplits( ParentMesh, ParentVID, Overlay, ShouldWeld );
    }

    void FSplitAttributeWelder::WeldSplitColors( const int32_t ParentVID, FDynamicMeshColorOverlay& Overlay,
                                                 float ColorDistSqrdThreshold )
    {
        const FDynamicMesh3* ParentMesh = Overlay.GetParentMesh();
        const float          Threshold  = std::max( ColorDistSqrdThreshold, 0.f );
        auto                 ShouldWeld = [&Overlay, Threshold]( const int32_t eid, const int32_t oeid ) -> bool
        {
            const glm::vec4 A = Overlay.GetElement( eid ), B = Overlay.GetElement( oeid );
            const float     d[4] = { A.x - B.x, A.y - B.y, A.z - B.z, A.w - B.w };
            return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + d[3] * d[3] <= Threshold;
        };
        WeldSplits( ParentMesh, ParentVID, Overlay, ShouldWeld );
    }
} // namespace Desert::Geometry
