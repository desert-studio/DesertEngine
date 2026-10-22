// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/MeshTangents.cpp:186-235
// (ComputeFaceTangent), 295-345 (CopyToOverlays, PlaneProjectionNormalized), 356-391
// (ComputeSeparatePerTriangleTangents), adapted: UE Core as std/glm, namespace Desert::Geometry, ParallelFor
// is a serial for loop, FVector2f -> FVector2d by components (MeshCore's TVector2 has no converting
// constructor), instantiated for double only.
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshTangents.hpp"
#include "Engine/Geometry/MeshCore/IndexUtil.hpp"

#include <algorithm>
#include <cmath>

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
                     const Index3i TriA = MeshToSet.GetTriangle( TriIDA );
                     const Index3i TriB = MeshToSet.GetTriangle( TriIDB );
                     const int     SubA = TriA.IndexOf( ParentVertexIdx );
                     const int     SubB = TriB.IndexOf( ParentVertexIdx );
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

    namespace
    {
        // UE MeshTangents.cpp ComputeFaceTangent.
        void ComputeFaceTangentWeighted( const glm::dvec3 TriVertices[3], const glm::dvec2 TriUVs[3],
                                         glm::dvec3& TangentOut, glm::dvec3& BitangentOut, glm::dvec2& MagnitudesOut,
                                         bool& bIsDegenerateOut )
        {
            const glm::dvec2 UVEdge1      = TriUVs[1] - TriUVs[0];
            const glm::dvec2 UVEdge2      = TriUVs[2] - TriUVs[0];
            const glm::dvec3 TriEdge1     = TriVertices[1] - TriVertices[0];
            const glm::dvec3 TriEdge2     = TriVertices[2] - TriVertices[0];
            const glm::dvec3 TriTangent   = ( UVEdge2.y * TriEdge1 ) - ( UVEdge1.y * TriEdge2 );
            const glm::dvec3 TriBitangent = ( -UVEdge2.x * TriEdge1 ) + ( UVEdge1.x * TriEdge2 );
            double           UVArea       = ( UVEdge1.x * UVEdge2.y ) - ( UVEdge1.y * UVEdge2.x );
            const bool       bPreserve    = UVArea >= 0;
            UVArea                        = std::abs( UVArea );
            const double TL               = glm::length( TriTangent );
            const double BL               = glm::length( TriBitangent );
            TangentOut                    = TL > 0 ? TriTangent / TL : glm::dvec3( 0.0 );
            BitangentOut                  = BL > 0 ? TriBitangent / BL : glm::dvec3( 0.0 );
            MagnitudesOut                 = glm::dvec2( TL / UVArea, BL / UVArea );
            if ( !bPreserve )
            {
                TangentOut   = -TangentOut;
                BitangentOut = -BitangentOut;
            }
            bIsDegenerateOut = UVArea < 1e-8; // FMathd::ZeroTolerance
        }

        glm::dvec3 TriNormalOf( const DynamicMesh3& Mesh, int TriangleID )
        {
            const Index3i    T = Mesh.GetTriangle( TriangleID );
            const glm::dvec3 A = Mesh.GetVertex( T.A ), B = Mesh.GetVertex( T.B ), Cv = Mesh.GetVertex( T.C );
            const glm::dvec3 N = glm::cross( B - A, Cv - A );
            const double     L = glm::length( N );
            return L > 0 ? N / L : glm::dvec3( 0.0 );
        }

        glm::dvec3 SafeNormalized( const glm::dvec3& V )
        {
            const double L = glm::length( V );
            return L > 0 ? V / L : glm::dvec3( 0.0 );
        }
    } // namespace

    template <typename RealType>
    void MeshTangents<RealType>::ComputeTriVertexTangents( const DynamicMeshNormalOverlay* NormalOverlay,
                                                           const DynamicMeshUVOverlay*     UVOverlay,
                                                           const ComputeTangentsOptions&   Options )
    {
        if ( Options.bAveraged )
            ComputeMikkTStyleTangents( NormalOverlay, UVOverlay, Options );
        else
            ComputeSeparatePerTriangleTangents( NormalOverlay, UVOverlay );
    }

    // UE MeshTangents.cpp:395-676 ComputeMikkTStyleTangents (ParallelFor as serial loops).
    template <typename RealType>
    void MeshTangents<RealType>::ComputeMikkTStyleTangents( const DynamicMeshNormalOverlay* NormalOverlay,
                                                            const DynamicMeshUVOverlay*     UVOverlay,
                                                            const ComputeTangentsOptions&   Options )
    {
        constexpr double HardcodedSplitDotThresh = -0.99;
        const int        MaxTriangleID           = m_Mesh->MaxTriangleID();
        InitializeTriVertexTangents( true );
        std::vector<glm::dvec3> TriTangents( MaxTriangleID ), TriBitangents( MaxTriangleID ),
             TriMagnitudes( MaxTriangleID );
        std::vector<int> DegenerateTris;
        for ( int TriangleID = 0; TriangleID < MaxTriangleID; ++TriangleID )
        {
            if ( !m_Mesh->IsTriangle( TriangleID ) || !UVOverlay->IsSetTriangle( TriangleID ) )
                continue;
            const Index3i    T     = m_Mesh->GetTriangle( TriangleID );
            const glm::dvec3 V[3]  = { m_Mesh->GetVertex( T.A ), m_Mesh->GetVertex( T.B ), m_Mesh->GetVertex( T.C ) };
            const Index3i    UT    = UVOverlay->GetTriangle( TriangleID );
            const glm::dvec2 UV[3] = { glm::dvec2( UVOverlay->GetElement( UT.A ) ),
                                       glm::dvec2( UVOverlay->GetElement( UT.B ) ),
                                       glm::dvec2( UVOverlay->GetElement( UT.C ) ) };
            glm::dvec3       Tangent, Bitangent;
            glm::dvec2       Magnitudes;
            bool             bIsDegenerate = false;
            ComputeFaceTangentWeighted( V, UV, Tangent, Bitangent, Magnitudes, bIsDegenerate );
            TriTangents[TriangleID]   = Tangent;
            TriBitangents[TriangleID] = Bitangent;
            TriMagnitudes[TriangleID] = bIsDegenerate ? glm::dvec3( 0.0 ) : glm::dvec3( Magnitudes, 0.0 );
            if ( bIsDegenerate )
                DegenerateTris.push_back( TriangleID );
        }

        // Average triangle tangents across vertex "wedges": one-ring triangles sharing UV and normal elements.
        for ( int VertexID = 0; VertexID < m_Mesh->MaxVertexID(); ++VertexID )
        {
            if ( !m_Mesh->IsVertex( VertexID ) )
                continue;
            std::vector<Index3i>    Uniques, TriSet;
            std::vector<glm::dvec3> AccumTangent, AccumBitangent, AccumMagnitudeAngle;
            m_Mesh->EnumerateVertexTriangles(
                 VertexID,
                 [&]( int32_t TriangleID )
                 {
                     if ( !UVOverlay->IsSetTriangle( TriangleID ) )
                         return;
                     glm::dvec3    Magnitudes   = TriMagnitudes[TriangleID];
                     const Index3i Triangle     = m_Mesh->GetTriangle( TriangleID );
                     const int     Idx          = IndexUtil::FindTriIndex( VertexID, Triangle );
                     const int     UVElementID  = UVOverlay->GetTriangle( TriangleID )[Idx];
                     const bool    NormalSet    = NormalOverlay->IsSetTriangle( TriangleID );
                     const int     NormalElemID = NormalSet ? NormalOverlay->GetTriangle( TriangleID )[Idx] : -1;
                     const glm::dvec3 Normal    = NormalSet ? glm::dvec3( NormalOverlay->GetElement( NormalElemID ) )
                                                            : TriNormalOf( *m_Mesh, TriangleID );
                     glm::dvec3 ProjTangent     = PlaneProjectionNormalized( TriTangents[TriangleID], Normal );
                     glm::dvec3 ProjBitangent   = PlaneProjectionNormalized( TriBitangents[TriangleID], Normal );
                     double     Angle           = 1.0;
                     if ( Options.bAngleWeighted )
                     {
                         const glm::dvec3 P0 = m_Mesh->GetVertex( Triangle[Idx] );
                         const glm::dvec3 E1 = SafeNormalized( m_Mesh->GetVertex( Triangle[( Idx + 1 ) % 3] ) - P0 );
                         const glm::dvec3 E2 = SafeNormalized( m_Mesh->GetVertex( Triangle[( Idx + 2 ) % 3] ) - P0 );
                         Angle               = std::acos( std::clamp( glm::dot( E1, E2 ), -1.0, 1.0 ) );
                         ProjTangent *= Angle;
                         ProjBitangent *= Angle;
                         Magnitudes.x *= Angle;
                         Magnitudes.y *= Angle;
                     }
                     const glm::dvec3 MagnitudeAngle( Magnitudes.x, Magnitudes.y, Angle );
                     const Index3i    Key( UVElementID, NormalElemID, Magnitudes.z < 0 ? -1 : 1 );
                     int              Found = -1;
                     for ( size_t k = 0; k < Uniques.size(); ++k )
                         if ( Uniques[k] == Key )
                         {
                             Found = static_cast<int>( k );
                             break;
                         }
                     if ( Found >= 0 &&
                          ( glm::dot( SafeNormalized( AccumTangent[Found] ), ProjTangent ) <= HardcodedSplitDotThresh ||
                            glm::dot( SafeNormalized( AccumBitangent[Found] ), ProjBitangent ) <=
                                 HardcodedSplitDotThresh ) )
                         Found = -1; // too large an angle: a new (single-triangle) group, as UE
                     if ( Found < 0 )
                     {
                         Found = static_cast<int>( Uniques.size() );
                         Uniques.push_back( Key );
                         AccumTangent.push_back( ProjTangent );
                         AccumBitangent.push_back( ProjBitangent );
                         AccumMagnitudeAngle.push_back( MagnitudeAngle );
                     }
                     else
                     {
                         AccumTangent[Found] += ProjTangent;
                         AccumBitangent[Found] += ProjBitangent;
                         AccumMagnitudeAngle[Found] += MagnitudeAngle;
                     }
                     TriSet.push_back( Index3i( TriangleID, Idx, Found ) );
                 } );
            for ( size_t k = 0; k < AccumTangent.size(); ++k )
            {
                AccumTangent[k]   = SafeNormalized( AccumTangent[k] );
                AccumBitangent[k] = SafeNormalized( AccumBitangent[k] );
            }
            for ( const Index3i& TriEntry : TriSet )
            {
                const int         TriangleID   = TriEntry.A;
                const int         TriIndex     = TriEntry.B;
                const glm::dvec3& Tangent      = AccumTangent[TriEntry.C];
                const glm::dvec3& Bitangent    = AccumBitangent[TriEntry.C];
                const int         TriVertIndex = 3 * TriangleID + TriIndex;
                m_Tangents[TriVertIndex]       = glm::vec<3, RealType>( Tangent );
                const glm::dvec3 TriVertNormal =
                     NormalOverlay->IsSetTriangle( TriangleID )
                          ? glm::dvec3( NormalOverlay->GetElement( NormalOverlay->GetTriangle( TriangleID )[TriIndex] ) )
                          : TriNormalOf( *m_Mesh, TriangleID );
                // UE VectorUtil::BitangentSign / Bitangent
                const double BinormalSign = glm::dot( glm::cross( TriVertNormal, Tangent ), Bitangent ) < 0 ? -1.0 : 1.0;
                m_Bitangents[TriVertIndex] =
                     glm::vec<3, RealType>( SafeNormalized( BinormalSign * glm::cross( TriVertNormal, Tangent ) ) );
            }
        }
        m_DegenerateTris = DegenerateTris;

        // Propagate to degenerates from the first non-degenerate edge neighbour (UE's own "todo" behaviour).
        std::vector<int> DegenSet = DegenerateTris;
        const auto       inDegen  = [&]( int t ) { return std::find( DegenSet.begin(), DegenSet.end(), t ) != DegenSet.end(); };
        std::vector<int> NewDegenerates;
        while ( !DegenerateTris.empty() )
        {
            NewDegenerates.clear();
            for ( const int TriangleID : DegenerateTris )
            {
                bool          bHandled = false;
                const Index3i TriEdges = m_Mesh->GetTriEdges( TriangleID );
                for ( int j = 0; j < 3 && !bHandled; ++j )
                {
                    const Index2i EV    = m_Mesh->GetEdgeV( TriEdges[j] );
                    const Index2i ET    = m_Mesh->GetEdgeT( TriEdges[j] );
                    const int     Other = ET.A == TriangleID ? ET.B : ET.A;
                    if ( Other < 0 || inDegen( Other ) )
                        continue;
                    const Index3i Tri  = m_Mesh->GetTriangle( TriangleID );
                    const Index3i OTri = m_Mesh->GetTriangle( Other );
                    const int     A = IndexUtil::FindTriIndex( EV.A, Tri ), B = IndexUtil::FindTriIndex( EV.B, Tri );
                    const int     Cc = 3 - A - B;
                    const int     OA = IndexUtil::FindTriIndex( EV.A, OTri ), OB = IndexUtil::FindTriIndex( EV.B, OTri );
                    m_Tangents[3 * TriangleID + A]    = m_Tangents[3 * Other + OA];
                    m_Bitangents[3 * TriangleID + A]  = m_Bitangents[3 * Other + OA];
                    m_Tangents[3 * TriangleID + B]    = m_Tangents[3 * Other + OB];
                    m_Bitangents[3 * TriangleID + B]  = m_Bitangents[3 * Other + OB];
                    m_Tangents[3 * TriangleID + Cc]   = m_Tangents[3 * TriangleID + A];
                    m_Bitangents[3 * TriangleID + Cc] = m_Bitangents[3 * TriangleID + A];
                    bHandled                          = true;
                }
                if ( bHandled )
                    DegenSet.erase( std::find( DegenSet.begin(), DegenSet.end(), TriangleID ) );
                else
                    NewDegenerates.push_back( TriangleID );
            }
            if ( NewDegenerates.size() == DegenerateTris.size() )
                DegenerateTris.clear(); // no progress
            else
                std::swap( DegenerateTris, NewDegenerates );
        }
    }

    template class MeshTangents<double>;
} // namespace Desert::Geometry
