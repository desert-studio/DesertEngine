// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/MeshTangents.cpp:186-235
// (ComputeFaceTangent), 295-345 (CopyToOverlays, PlaneProjectionNormalized), 356-391
// (ComputeSeparatePerTriangleTangents), adapted: UE Core as std/glm, namespace Desert::Geometry, ParallelFor
// is a serial for loop, FVector2f -> FVector2d by components (MeshCore's TVector2 has no converting
// constructor), instantiated for double only.
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshTangents.hpp"

#include <cmath>

#include "Engine/Geometry/MeshCore/VectorUtil.hpp"

using namespace Desert::Geometry;

namespace
{
    // Tangent / bitangent of one triangle from its positions and UVs.
    void ComputeFaceTangent( const glm::dvec3 TriVertices[3], const glm::vec2 TriUVs[3], glm::dvec3& TangentOut,
                             glm::dvec3& BitangentOut, glm::dvec2& MagnitudesOut, double& OrientationSignOut,
                             bool& bIsDegenerateOut )
    {
        const glm::dvec2 UVEdge1( static_cast<double>( TriUVs[1].x ) - TriUVs[0].x,
                                  static_cast<double>( TriUVs[1].y ) - TriUVs[0].y );
        const glm::dvec2 UVEdge2( static_cast<double>( TriUVs[2].x ) - TriUVs[0].x,
                                  static_cast<double>( TriUVs[2].y ) - TriUVs[0].y );
        const glm::dvec3 TriEdge1 = TriVertices[1] - TriVertices[0];
        const glm::dvec3 TriEdge2 = TriVertices[2] - TriVertices[0];

        const glm::dvec3 TriTangent   = ( UVEdge2.y * TriEdge1 ) - ( UVEdge1.y * TriEdge2 );
        const glm::dvec3 TriBitangent = ( -UVEdge2.x * TriEdge1 ) + ( UVEdge1.x * TriEdge2 );

        double     UVArea               = ( UVEdge1.x * UVEdge2.y ) - ( UVEdge1.y * UVEdge2.x );
        const bool bPreserveOrientation = ( UVArea >= 0 );

        UVArea = std::abs( UVArea );

        // if a triangle is zero-UV-area due to one edge being collapsed, we still have a
        // valid direction on the other edge. We are going to keep those
        const double TriTangentLength   = glm::length( TriTangent );
        const double TriBitangentLength = glm::length( TriBitangent );
        TangentOut    = ( TriTangentLength > 0 ) ? ( TriTangent / TriTangentLength ) : glm::dvec3( 0 );
        BitangentOut  = ( TriBitangentLength > 0 ) ? ( TriBitangent / TriBitangentLength ) : glm::dvec3( 0 );
        MagnitudesOut = glm::dvec2( TriTangentLength / UVArea, TriBitangentLength / UVArea );

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

        bIsDegenerateOut = ( UVArea < ZeroTolerance<double> );
    }

    glm::dvec3 PlaneProjectionNormalized( const glm::dvec3& Vector, const glm::dvec3& PlaneNormal )
    {
        return Normalized( Vector - glm::dot( Vector, PlaneNormal ) * PlaneNormal );
    }
} // namespace

namespace Desert::Geometry
{
    template <typename RealType>
    bool MeshTangents<RealType>::CopyToOverlays( DynamicMesh3& MeshToSet ) const
    {
        if ( !MeshToSet.HasAttributes() || MeshToSet.Attributes()->NumNormalLayers() != 3 )
        {
            return false;
        }

        // Set aliases to make iterating over tangents and bitangents easier
        DynamicMeshNormalOverlay*                 TangentOverlays[2] = { MeshToSet.Attributes()->PrimaryTangents(),
                                                                         MeshToSet.Attributes()->PrimaryBiTangents() };
        const std::vector<glm::vec<3, RealType>>* TangentValues[2]   = { &m_Tangents, &m_Bitangents };

        for ( int Idx = 0; Idx < 2; Idx++ )
        {
            // Create overlay topology
            const std::vector<glm::vec<3, RealType>>& TV = *TangentValues[Idx];
            TangentOverlays[Idx]->CreateFromPredicate(
                 [&MeshToSet, &TV]( int ParentVertexIdx, int TriIDA, int TriIDB ) -> bool
                 {
                     const Index3i  TriA = MeshToSet.GetTriangle( TriIDA );
                     const Index3i  TriB = MeshToSet.GetTriangle( TriIDB );
                     const int      SubA = TriA.IndexOf( ParentVertexIdx );
                     const int      SubB = TriB.IndexOf( ParentVertexIdx );
                     assert( SubA > -1 && SubB > -1 );
                     const glm::vec<3, RealType>& A = TV[TriIDA * 3 + SubA];
                     const glm::vec<3, RealType>& B = TV[TriIDB * 3 + SubB];
                     return DistanceSquared( A, B ) < ZeroTolerance<RealType>;
                 },
                 0.0f );

            // Write tangent values out for each wedge value
            // Note: shared elements will be written to multiple times, and the last value written will be used
            for ( const int TID : MeshToSet.TriangleIndicesItr() )
            {
                const Index3i ElTri = TangentOverlays[Idx]->GetTriangle( TID );
                for ( int SubIdx = 0; SubIdx < 3; SubIdx++ )
                {
                    TangentOverlays[Idx]->SetElement( ElTri[SubIdx],
                                                      static_cast<glm::vec3>( TV[TID * 3 + SubIdx] ) );
                }
            }
        }
        return true;
    }

    template <typename RealType>
    void MeshTangents<RealType>::ComputeSeparatePerTriangleTangents( const DynamicMeshNormalOverlay* NormalOverlay,
                                                                     const DynamicMeshUVOverlay*     UVOverlay )
    {
        const int32_t MaxTriangleID = m_Mesh->MaxTriangleID();
        InitializeTriVertexTangents( false );

        // compute per-triangle tangent and bitangent
        for ( int32_t TriangleID = 0; TriangleID < MaxTriangleID; ++TriangleID )
        {
            if ( !m_Mesh->IsTriangle( TriangleID ) || !UVOverlay->IsSetTriangle( TriangleID ) )
            {
                continue;
            }

            glm::dvec3 TriVertices[3]{};
            m_Mesh->GetTriVertices( TriangleID, TriVertices[0], TriVertices[1], TriVertices[2] );
            glm::vec2 TriUVs[3]{};
            UVOverlay->GetTriElements( TriangleID, TriUVs[0], TriUVs[1], TriUVs[2] );
            glm::vec3 TriNormals[3]{};
            NormalOverlay->GetTriElements( TriangleID, TriNormals[0], TriNormals[1], TriNormals[2] );

            glm::dvec3 Tangent{};
            glm::dvec3 Bitangent{};
            glm::dvec2 Magnitudes{};
            double     OrientationSign = NAN;
            bool       bIsDegenerate   = false;
            ComputeFaceTangent( TriVertices, TriUVs, Tangent, Bitangent, Magnitudes, OrientationSign,
                                bIsDegenerate );

            for ( int32_t j = 0; j < 3; ++j )
            {
                const glm::dvec3 VtxNormal        = glm::dvec3( TriNormals[j] );
                const glm::dvec3 ProjectedTangent = PlaneProjectionNormalized( Tangent, VtxNormal );

                const double BitangentSign = VectorUtil::BitangentSign( VtxNormal, ProjectedTangent, Bitangent );
                const glm::dvec3 ReconsBitangent =
                     VectorUtil::Bitangent( VtxNormal, ProjectedTangent, BitangentSign );

                SetPerTriangleTangent( TriangleID, j,
                                       Normalized( static_cast<glm::vec<3, RealType>>( ProjectedTangent ) ),
                                       Normalized( static_cast<glm::vec<3, RealType>>( ReconsBitangent ) ) );
            }
        }
    }

    template class MeshTangents<double>;
} // namespace Desert::Geometry
