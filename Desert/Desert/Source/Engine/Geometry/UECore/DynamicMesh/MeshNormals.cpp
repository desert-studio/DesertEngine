// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/MeshNormals.cpp:1-767, adapted: UE
// Core via UECore.hpp, namespace Desert::Geometry, check/ensure are UE_CHECK/UE_ENSURE, FMemory::Memzero is
// std::memset, ParallelFor is a serial for loop. TriangleToVertexIDs (MeshIndexUtil.cpp:8-51) and
// TMeshQueries::GetVertexWeightsOnTriangle (MeshQueries.h:831-843) are ported below as file-local helpers.
#include "Engine/Geometry/UECore/DynamicMesh/MeshNormals.hpp"

#include <cstring>
#include <Common/Core/Core.hpp>

using namespace Desert::Geometry;

namespace
{
    // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/MeshIndexUtil.cpp:8-51.
    void TriangleToVertexIDs( const DynamicMesh3* Mesh, const std::vector<int>& TriangleIDs,
                              std::vector<int>& VertexIDsOut )
    {
        int NumTris = static_cast<int32_t>( TriangleIDs.size() );
        if ( NumTris < 25 )
        {
            for ( int k = 0; k < NumTris; ++k )
            {
                if ( Mesh->IsTriangle( TriangleIDs[k] ) )
                {
                    Index3i Tri = Mesh->GetTriangle( TriangleIDs[k] );
                    if ( std::find( VertexIDsOut.begin(), VertexIDsOut.end(), Tri[0] ) == VertexIDsOut.end() )
                    {
                        VertexIDsOut.push_back( Tri[0] );
                    }
                    if ( std::find( VertexIDsOut.begin(), VertexIDsOut.end(), Tri[1] ) == VertexIDsOut.end() )
                    {
                        VertexIDsOut.push_back( Tri[1] );
                    }
                    if ( std::find( VertexIDsOut.begin(), VertexIDsOut.end(), Tri[2] ) == VertexIDsOut.end() )
                    {
                        VertexIDsOut.push_back( Tri[2] );
                    }
                }
            }
        }
        else
        {
            std::unordered_set<int> VertexSet;
            VertexSet.reserve( static_cast<int32_t>( TriangleIDs.size() ) * 3 );
            for ( int k = 0; k < NumTris; ++k )
            {
                if ( Mesh->IsTriangle( TriangleIDs[k] ) )
                {
                    Index3i Tri = Mesh->GetTriangle( TriangleIDs[k] );
                    VertexSet.insert( Tri[0] );
                    VertexSet.insert( Tri[1] );
                    VertexSet.insert( Tri[2] );
                }
            }
            VertexIDsOut.reserve( static_cast<int32_t>( VertexSet.size() ) );
            for ( int VertexID : VertexSet )
            {
                VertexIDsOut.push_back( VertexID );
            }
        }
    }

    // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/MeshQueries.h:831-843.
    glm::dvec3 GetVertexWeightsOnTriangleImpl( const DynamicMesh3& Mesh, int TriID, double TriArea,
                                               bool bWeightByArea, bool bWeightByAngle )
    {
        glm::dvec3 TriNormalWeights = glm::dvec3( 1 );
        if ( bWeightByAngle )
        {
            TriNormalWeights = Mesh.GetTriInternalAnglesR( TriID );
        }
        if ( bWeightByArea )
        {
            TriNormalWeights *= TriArea;
        }
        return TriNormalWeights;
    }
} // namespace

void MeshNormals::SetCount( int Count, bool bClearToZero )
{
    if ( static_cast<int32_t>( Normals.size() ) < Count )
    {
        Normals.resize( Count );
    }
    if ( bClearToZero )
    {
        std::memset( Normals.data(), 0,
                     static_cast<size_t>( static_cast<int32_t>( Normals.size() ) ) * sizeof( glm::dvec3 ) );
    }
}

void MeshNormals::CopyToVertexNormals( DynamicMesh3* SetMesh, bool bInvert ) const
{
    if ( SetMesh->HasVertexNormals() == false )
    {
        SetMesh->EnableVertexNormals( glm::vec3( 1, 0, 0 ) );
    }

    float sign = ( bInvert ) ? -1.0f : 1.0f;
    int const N    = std::min( static_cast<int32_t>( Normals.size() ), SetMesh->MaxVertexID() );
    for ( int vi = 0; vi < N; ++vi )
    {
        if ( Mesh->IsVertex( vi ) && SetMesh->IsVertex( vi ) )
        {
            SetMesh->SetVertexNormal( vi, sign * (glm::vec3)Normals[vi] );
        }
    }
}

void MeshNormals::GetVertexNormalsFromOverlayNormals( CombineSplitNormalsMethod CombineSplitNormals )
{
    assert( Mesh );

    // no overlay to copy from
    if ( !Mesh->HasAttributes() || !Mesh->Attributes()->PrimaryNormals() )
    {
        Normals.assign( Mesh->MaxVertexID(), glm::dvec3( 0, 0, 1 ) );
        return;
    }

    const DynamicMeshNormalOverlay* NormalOverlay = Mesh->Attributes()->PrimaryNormals();
    if ( CombineSplitNormals == CombineSplitNormalsMethod::Average )
    {
        Normals.assign( Mesh->MaxVertexID(), glm::dvec3( 0 ) );
        for ( int32_t const TID : Mesh->TriangleIndicesItr() )
        {
            if ( NormalOverlay->IsSetTriangle( TID ) )
            {
                Index3i   TriV = Mesh->GetTriangle( TID );
                glm::vec3 A{}, B{}, C{};
                NormalOverlay->GetTriElements( TID, A, B, C );
                Normals[TriV.A] += (glm::dvec3)A;
                Normals[TriV.B] += (glm::dvec3)B;
                Normals[TriV.C] += (glm::dvec3)C;
            }
        }

        for ( int32_t k = 0; k < static_cast<int32_t>( Normals.size() ); ++k )
        {
            Normalize( Normals[k] );
        }
    }
    else // CombineSplitNormalsMethod::CopyAny
    {
        Normals.assign( Mesh->MaxVertexID(), glm::dvec3( 0, 0, 1 ) );
        for ( int32_t const TID : Mesh->TriangleIndicesItr() )
        {
            if ( NormalOverlay->IsSetTriangle( TID ) )
            {
                Index3i   TriV = Mesh->GetTriangle( TID );
                glm::vec3 A{}, B{}, C{};
                NormalOverlay->GetTriElements( TID, A, B, C );
                Normals[TriV.A] = (glm::dvec3)A;
                Normals[TriV.B] = (glm::dvec3)B;
                Normals[TriV.C] = (glm::dvec3)C;
            }
        }
    }
}

void MeshNormals::CopyToOverlay( DynamicMeshNormalOverlay* NormalOverlay, bool bInvert ) const
{
    float sign = ( bInvert ) ? -1.0f : 1.0f;
    for ( int ElemIdx : NormalOverlay->ElementIndicesItr() )
    {
        NormalOverlay->SetElement( ElemIdx, sign * (glm::vec3)Normals[ElemIdx] );
    }
}

void MeshNormals::Compute_FaceAvg_AreaWeighted()
{
    SetCount( Mesh->MaxVertexID(), true );

    for ( int TriIdx : Mesh->TriangleIndicesItr() )
    {
        glm::dvec3 TriNormal{}, TriCentroid{};
        double    TriArea;
        Mesh->GetTriInfo( TriIdx, TriNormal, TriArea, TriCentroid );
        TriNormal *= TriArea;

        Index3i Triangle = Mesh->GetTriangle( TriIdx );
        Normals[Triangle.A] += TriNormal;
        Normals[Triangle.B] += TriNormal;
        Normals[Triangle.C] += TriNormal;
    }

    for ( int VertIdx : Mesh->VertexIndicesItr() )
    {
        Normalize( Normals[VertIdx] );
    }
}

void MeshNormals::Compute_FaceAvg( bool bWeightByArea, bool bWeightByAngle )
{
    if ( !bWeightByAngle && bWeightByArea )
    {
        Compute_FaceAvg_AreaWeighted(); // faster case
        return;
    }

    // most general case
    SetCount( Mesh->MaxVertexID(), true );

    for ( int TriIdx : Mesh->TriangleIndicesItr() )
    {
        glm::dvec3 TriNormal{}, TriCentroid{};
        double    TriArea;
        Mesh->GetTriInfo( TriIdx, TriNormal, TriArea, TriCentroid );
        glm::dvec3 TriNormalWeights =
             GetVertexWeightsOnTriangle( Mesh, TriIdx, TriArea, bWeightByArea, bWeightByAngle );

        Index3i Triangle = Mesh->GetTriangle( TriIdx );
        Normals[Triangle.A] += TriNormal * TriNormalWeights[0];
        Normals[Triangle.B] += TriNormal * TriNormalWeights[1];
        Normals[Triangle.C] += TriNormal * TriNormalWeights[2];
    }

    for ( int VertIdx : Mesh->VertexIndicesItr() )
    {
        Normalize( Normals[VertIdx] );
    }
}

void MeshNormals::Compute_Triangle()
{
    int NumTriangles = Mesh->MaxTriangleID();
    SetCount( NumTriangles, false );
    for ( int32_t Index = 0; Index < NumTriangles; ++Index )
    {
        if ( Mesh->IsTriangle( Index ) )
        {
            Normals[Index] = Mesh->GetTriNormal( Index );
        }
    }
}

void MeshNormals::SetDegenerateTriangleNormalsToNeighborNormal()
{
    assert( static_cast<int32_t>( Normals.size() ) >= Mesh->MaxTriangleID() );

    // We're going to look through the triangles and set any zero normals
    // to the normal of their neighbor, preferring to go toward the neighbor
    // with the longer side when possible. Since we could have multiple degenerate
    // triangles linked together, this may require a little neighborhood walk.

    // Our walking function
    auto GetNeighborThatHasNormal = [this]( int32_t StartTid, std::unordered_set<int32_t>& WalkedTidsOut,
                                            int32_t& NonDegenerateNeighborTidOut )
    {
        WalkedTidsOut.clear();
        NonDegenerateNeighborTidOut = DynamicMesh3::InvalidID;

        assert( StartTid != DynamicMesh3::InvalidID && Normals[StartTid] == glm::dvec3( 0 ) );

        // We don't like recursion, so we use a little stack instead to help us prioritize
        // the longer-side neighbors in our walk.
        std::vector<int32_t> TidsToSearch;
        TidsToSearch.push_back( StartTid );

        while ( !TidsToSearch.empty() )
        {
            int32_t const CurrentTid = TidsToSearch.back();
            TidsToSearch.pop_back();

            if ( WalkedTidsOut.contains( CurrentTid ) )
            {
                continue;
            }

            // See if we've reached a non-degenerate triangle
            if ( Normals[CurrentTid] != glm::dvec3( 0 ) )
            {
                NonDegenerateNeighborTidOut = CurrentTid;
                return;
            }

            WalkedTidsOut.insert( CurrentTid );

            // Sanity check so we don't go forever
            if ( static_cast<int32_t>( WalkedTidsOut.size() ) > Mesh->MaxTriangleID() )
            {
                assert( false );
                return;
            }

            // Otherwise, get neighbors and corresponding squared edge lengths.
            int32_t  NeighborTids[3];
            double   SquaredEdgeLengths[3];
            Index3i  TriEdges = Mesh->GetTriEdges( CurrentTid );
            for ( int i = 0; i < 3; ++i )
            {
                Index2i       Tids     = Mesh->GetEdgeT( TriEdges[i] );
                int32_t const OtherTid = ( Tids.A == CurrentTid ) ? Tids.B : Tids.A;
                NeighborTids[i]   = OtherTid;

                if ( OtherTid == DynamicMesh3::InvalidID )
                {
                    SquaredEdgeLengths[i] = 0;
                }
                else
                {
                    glm::dvec3 Vert1{}, Vert2{};
                    Mesh->GetEdgeV( TriEdges[i], Vert1, Vert2 );
                    SquaredEdgeLengths[i] = DistanceSquared( Vert1, Vert2 );
                }
            }

            // Order neighbors by ascending length
            if ( SquaredEdgeLengths[0] > SquaredEdgeLengths[1] )
            {
                std::swap( NeighborTids[0], NeighborTids[1] );
                std::swap( SquaredEdgeLengths[0], SquaredEdgeLengths[1] );
            }
            if ( SquaredEdgeLengths[1] > SquaredEdgeLengths[2] )
            {
                std::swap( NeighborTids[1], NeighborTids[2] );
                std::swap( SquaredEdgeLengths[1], SquaredEdgeLengths[2] );
            }
            if ( SquaredEdgeLengths[0] > SquaredEdgeLengths[1] )
            {
                std::swap( NeighborTids[0], NeighborTids[1] );
                std::swap( SquaredEdgeLengths[0], SquaredEdgeLengths[1] );
            }

            // Add onto stack. Longest length neighbor is at top of stack
            for ( int const NeighborTid : NeighborTids )
            {
                if ( NeighborTid != DynamicMesh3::InvalidID )
                {
                    TidsToSearch.push_back( NeighborTid );
                }
            }
        }
    };

    // It's possible that we could have an island of degenerates, ie no normal neighbor.
    // In that case we might as well not waste time starting the same walk from each one.
    std::unordered_set<int32_t> IslandDegenerates;

    std::unordered_set<int32_t> CurrentWalkedTids;

    for ( int32_t const Tid : Mesh->TriangleIndicesItr() )
    {
        if ( Normals[Tid] == glm::dvec3( 0 ) && !IslandDegenerates.contains( Tid ) )
        {
            // Find a normal to use
            CurrentWalkedTids.clear();
            int32_t NonDegenerateNeighborTid = DynamicMesh3::InvalidID;
            GetNeighborThatHasNormal( Tid, CurrentWalkedTids, NonDegenerateNeighborTid );

            // Make sure there was a non-degenerate neighbor.
            if ( NonDegenerateNeighborTid == DynamicMesh3::InvalidID )
            {
                DESERT_VERIFY_WARN( false, "MeshNormals::SetDegenerateTriangleNormalsToNeighborNormal: Had a "
                                           "component entirely composed of degenerate triangle normals." );
                IslandDegenerates.insert( CurrentWalkedTids.begin(), CurrentWalkedTids.end() );
            }
            else
            {
                // Apply the neighbor normal.
                glm::dvec3 NormalToUse = Normals[NonDegenerateNeighborTid];
                assert( NormalToUse != glm::dvec3( 0 ) );

                for ( int32_t const WalkedTid : CurrentWalkedTids )
                {
                    Normals[WalkedTid] = NormalToUse;
                }
            }
        } // end if normal is zero
    } // end for all triangles
}

void MeshNormals::Compute_Overlay_FaceAvg( const DynamicMeshNormalOverlay* NormalOverlay, bool bWeightByArea,
                                           bool bWeightByAngle )
{
    if ( ( !bWeightByAngle ) && bWeightByArea )
    {
        Compute_Overlay_FaceAvg_AreaWeighted( NormalOverlay ); // faster case
        return;
    }

    // most general case
    SetCount( NormalOverlay->MaxElementID(), true );

    for ( int TriIdx : Mesh->TriangleIndicesItr() )
    {
        Index3i Tri = NormalOverlay->GetTriangle( TriIdx );
        if ( Tri.A == INDEX_NONE )
        {
            continue;
        }

        glm::dvec3 V0{}, V1{}, V2{};
        Mesh->GetTriVertices( TriIdx, V0, V1, V2 );

        glm::dvec3 TriNormal{};
        double    TriArea;
        TriNormal = VectorUtil::NormalArea( V0, V1, V2, TriArea );
        glm::dvec3 TriNormalWeights =
             GetVertexWeightsOnTriangle( Mesh, TriIdx, TriArea, bWeightByArea, bWeightByAngle );

        for ( int j = 0; j < 3; ++j )
        {
            Normals[Tri[j]] += TriNormal * TriNormalWeights[j];
        }
    }

    for ( int ElemIdx : NormalOverlay->ElementIndicesItr() )
    {
        Normalize( Normals[ElemIdx] );
    }
}

void MeshNormals::Compute_Overlay_FaceAvg_AreaWeighted( const DynamicMeshNormalOverlay* NormalOverlay )
{
    SetCount( NormalOverlay->MaxElementID(), true );

    for ( int TriIdx : Mesh->TriangleIndicesItr() )
    {
        glm::dvec3 TriNormal{}, TriCentroid{};
        double    TriArea;
        Mesh->GetTriInfo( TriIdx, TriNormal, TriArea, TriCentroid );
        TriNormal *= TriArea;

        Index3i Tri = NormalOverlay->GetTriangle( TriIdx );
        for ( int j = 0; j < 3; ++j )
        {
            if ( Tri[j] != DynamicMesh3::InvalidID )
            {
                Normals[Tri[j]] += TriNormal;
            }
        }
    }

    for ( int ElemIdx : NormalOverlay->ElementIndicesItr() )
    {
        Normalize( Normals[ElemIdx] );
    }
}

void MeshNormals::QuickComputeVertexNormals( DynamicMesh3& Mesh, bool bInvert )
{
    MeshNormals normals( &Mesh );
    normals.ComputeVertexNormals();
    normals.CopyToVertexNormals( &Mesh, bInvert );
}

void MeshNormals::SmoothVertexNormals( DynamicMesh3& Mesh, int32_t SmoothingRounds, double SmoothingAlpha )
{
    SmoothingRounds = std::clamp( SmoothingRounds, 0, 500 );
    SmoothingAlpha  = std::clamp<double>( SmoothingAlpha, 0.0, 1.0 );
    if ( SmoothingRounds > 0 && SmoothingAlpha > 0 )
    {
        int32_t const     NumV = Mesh.MaxVertexID();
        std::vector<glm::dvec3> SmoothedNormals;
        SmoothedNormals.resize( NumV );
        for ( int32_t ri = 0; ri < SmoothingRounds; ++ri )
        {
            SmoothedNormals.assign( NumV, glm::dvec3( 0 ) );

            // compute
            for ( int32_t vid = 0; vid < NumV; ++vid )
            {
                if ( Mesh.IsVertex( vid ) )
                {
                    glm::dvec3 SmoothedNormal = glm::dvec3( 0 );
                    Mesh.EnumerateVertexVertices(
                         vid,
                         [&]( int32_t nbrvid ) { SmoothedNormal += (glm::dvec3)Mesh.GetVertexNormal( nbrvid ); } );
                    Normalize( SmoothedNormal );
                    SmoothedNormals[vid] =
                         Lerp( (glm::dvec3)Mesh.GetVertexNormal( vid ), SmoothedNormal, SmoothingAlpha );
                    Normalize( SmoothedNormals[vid] );
                }
            }

            // update
            for ( int32_t const vid : Mesh.VertexIndicesItr() )
            {
                Mesh.SetVertexNormal( vid, (glm::vec3)SmoothedNormals[vid] );
            }
        }
    }
}

void MeshNormals::QuickComputeVertexNormalsForTriangles( DynamicMesh3& Mesh, const std::vector<int32_t>& Triangles,
                                                         bool bWeightByArea, bool bWeightByAngle, bool bInvert )
{
    if ( Mesh.HasVertexNormals() == false )
    {
        Mesh.EnableVertexNormals( glm::vec3( 1, 0, 0 ) );
    }

    std::vector<int32_t> VertexIDs;
    TriangleToVertexIDs( &Mesh, Triangles, VertexIDs );
    for ( int32_t i = 0; i < static_cast<int32_t>( VertexIDs.size() ); ++i )
    {
        int32_t const vid       = VertexIDs[i];
        glm::dvec3    VtxNormal = ComputeVertexNormal( Mesh, vid, bWeightByArea, bWeightByAngle );
        Mesh.SetVertexNormal( vid, (glm::vec3)VtxNormal );
    }
}

bool MeshNormals::QuickRecomputeOverlayNormals( DynamicMesh3& Mesh, bool bInvert, bool bWeightByArea,
                                                bool bWeightByAngle, bool bParallelCompute )
{
    if ( !Mesh.HasAttributes() || Mesh.Attributes()->PrimaryNormals() == nullptr )
    {
        return false;
    }
    DynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals();
    if ( !bParallelCompute )
    {
        MeshNormals Normals( &Mesh );
        Normals.RecomputeOverlayNormals( NormalOverlay, bWeightByArea, bWeightByAngle );
        Normals.CopyToOverlay( NormalOverlay, bInvert );
        return true;
    }
    else
    {
        // Compute once per triangle normal and accumulate the results in element normals. UE threads this path
        // over atomic floats; the loops here run serially in triangle order, so plain floats accumulate the same
        // sums UE's single-threaded run does.
        std::vector<float> Normals( static_cast<size_t>( NormalOverlay->MaxElementID() ) * 3, 0.0f );

        for ( int32_t TID = 0; TID < Mesh.MaxTriangleID(); ++TID )
        {
            if ( !Mesh.IsTriangle( TID ) )
            {
                continue;
            }

            Index3i ElTri = NormalOverlay->GetTriangle( TID );
            if ( ElTri.A == INDEX_NONE )
            {
                // triangle was not set / has no elements
                continue;
            }

            glm::dvec3 V0{}, V1{}, V2{};
            Mesh.GetTriVertices( TID, V0, V1, V2 );

            glm::dvec3 TriNormal{};
            double     TriArea;
            TriNormal = VectorUtil::NormalArea( V0, V1, V2, TriArea );
            glm::vec3 TriNormalWeights =
                 (glm::vec3)GetVertexWeightsOnTriangleImpl( Mesh, TID, TriArea, bWeightByArea, bWeightByAngle );
            glm::vec3 TriNormalf = (glm::vec3)TriNormal;

            for ( int32_t SubIdx = 0; SubIdx < 3; ++SubIdx )
            {
                int32_t const ElID           = ElTri[SubIdx];
                glm::vec3     AddNormal      = TriNormalf * TriNormalWeights[SubIdx];
                int32_t const NormalArrayIdx = ElID * 3;
                for ( int32_t VecIdx = 0; VecIdx < 3; ++VecIdx )
                {
                    Normals[NormalArrayIdx + VecIdx] += AddNormal[VecIdx];
                }
            }
        }

        float Sign = ( bInvert ) ? -1.0f : 1.0f;
        for ( int32_t ElID = 0; ElID < NormalOverlay->MaxElementID(); ++ElID )
        {
            if ( NormalOverlay->IsElement( ElID ) )
            {
                int32_t const NormalsIdx = ElID * 3;
                // Note: Normalization intentionally computed in double precision for accuracy
                glm::dvec3 Normal( Normals[NormalsIdx], Normals[NormalsIdx + 1], Normals[NormalsIdx + 2] );
                NormalOverlay->SetElement( ElID, static_cast<glm::vec3>( double( Sign ) * Normalized( Normal ) ) );
            }
        }
    }
    return true;
}

bool MeshNormals::RecomputeOverlayTriNormals( DynamicMesh3& Mesh, const std::vector<int32_t>& Triangles,
                                              bool bWeightByArea, bool bWeightByAngle )
{
    if ( Mesh.HasAttributes() && Mesh.Attributes()->PrimaryNormals() != nullptr )
    {
        DynamicMeshNormalOverlay*   NormalOverlay = Mesh.Attributes()->PrimaryNormals();
        std::unordered_set<int32_t> UniqueElementIDs;
        for ( int32_t const tid : Triangles )
        {
            if ( NormalOverlay->IsSetTriangle( tid ) )
            {
                Index3i NormalTri = NormalOverlay->GetTriangle( tid );
                UniqueElementIDs.insert( NormalTri.A );
                UniqueElementIDs.insert( NormalTri.B );
                UniqueElementIDs.insert( NormalTri.C );
            }
        }

        return RecomputeOverlayElementNormals( Mesh,
                                               std::vector( UniqueElementIDs.begin(), UniqueElementIDs.end() ),
                                               bWeightByArea, bWeightByAngle );
    }
    return false;
}

bool MeshNormals::RecomputeOverlayElementNormals( DynamicMesh3& Mesh, const std::vector<int32_t>& ElementIDs,
                                                  bool bWeightByArea, bool bWeightByAngle )
{
    if ( Mesh.HasAttributes() && Mesh.Attributes()->PrimaryNormals() != nullptr )
    {
        DynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals();
        for ( int32_t k = 0; k < static_cast<int32_t>( ElementIDs.size() ); ++k )
        {
            int32_t const ElementID = ElementIDs[k];
            if ( NormalOverlay->IsElement( ElementID ) )
            {
                glm::dvec3 NewNormal = MeshNormals::ComputeOverlayNormal( Mesh, NormalOverlay, ElementID,
                                                                          bWeightByArea, bWeightByAngle );
                NormalOverlay->SetElement( ElementID, (glm::vec3)NewNormal );
            }
        }
        return true;
    }
    return false;
}

glm::dvec3 MeshNormals::ComputeVertexNormal( const DynamicMesh3& Mesh, int VertIdx, bool bWeightByArea,
                                             bool bWeightByAngle )
{
    glm::dvec3 SumNormal = glm::dvec3( 0 );
    Mesh.EnumerateVertexTriangles( VertIdx,
                                   [&]( int32_t TriIdx )
                                   {
                                       glm::dvec3 TriNormal{}, TriCentroid{};
                                       double    TriArea;
                                       Mesh.GetTriInfo( TriIdx, TriNormal, TriArea, TriCentroid );
                                       glm::dvec3 TriNormalWeights = GetVertexWeightsOnTriangle(
                                            &Mesh, TriIdx, TriArea, bWeightByArea, bWeightByAngle );

                                       Index3i       Triangle = Mesh.GetTriangle( TriIdx );
                                       int32_t const j        = IndexUtil::FindTriIndex( VertIdx, Triangle );
                                       SumNormal += TriNormal * TriNormalWeights[j];
                                   } );
    return Normalized( SumNormal );
}

glm::dvec3 MeshNormals::ComputeVertexNormal( const DynamicMesh3& Mesh, int32_t VertIdx,
                                             std::function<bool( int32_t )> TriangleFilterFunc, bool bWeightByArea,
                                             bool bWeightByAngle )
{
    glm::dvec3 NormalSum( 0, 0, 0 );
    Mesh.EnumerateVertexTriangles( VertIdx,
                                   [&]( int32_t TriIdx )
                                   {
                                       if ( TriangleFilterFunc( TriIdx ) )
                                       {
                                           glm::dvec3 TriNormal{}, TriCentroid{};
                                           double    TriArea;
                                           Mesh.GetTriInfo( TriIdx, TriNormal, TriArea, TriCentroid );
                                           glm::dvec3 TriNormalWeights = GetVertexWeightsOnTriangle(
                                                &Mesh, TriIdx, TriArea, bWeightByArea, bWeightByAngle );

                                           Index3i       Triangle = Mesh.GetTriangle( TriIdx );
                                           int32_t const j        = IndexUtil::FindTriIndex( VertIdx, Triangle );
                                           NormalSum += TriNormal * TriNormalWeights[j];
                                       }
                                   } );
    return Normalized( NormalSum );
}

glm::dvec3 MeshNormals::ComputeOverlayNormal( const DynamicMesh3&             Mesh,
                                              const DynamicMeshNormalOverlay* NormalOverlay, int ElemIdx,
                                              bool bWeightByArea, bool bWeightByAngle )
{
    int       ParentVertexID = NormalOverlay->GetParentVertex( ElemIdx );
    glm::dvec3 SumNormal      = glm::dvec3( 0 );
    int       Count          = 0;
    Mesh.EnumerateVertexTriangles(
         ParentVertexID,
         [&]( int32_t TriIdx )
         {
             if ( NormalOverlay->TriangleHasElement( TriIdx, ElemIdx ) )
             {
                 glm::dvec3 Normal{}, Centroid{};
                 double    Area;
                 Mesh.GetTriInfo( TriIdx, Normal, Area, Centroid );
                 glm::dvec3 TriNormalWeights =
                      GetVertexWeightsOnTriangle( &Mesh, TriIdx, Area, bWeightByArea, bWeightByAngle );
                 Index3i       Triangle = NormalOverlay->GetTriangle( TriIdx );
                 int32_t const j        = IndexUtil::FindTriIndex(
                      ElemIdx, Triangle ); // todo: we computed already in TriangleHasElement...
                 SumNormal += Normal * TriNormalWeights[j];
                 Count++;
             }
         } );

    return ( Count > 0 ) ? Normalized( SumNormal ) : glm::dvec3( 0 );
}

void MeshNormals::InitializeOverlayToPerVertexNormals( DynamicMeshNormalOverlay* NormalOverlay,
                                                       bool                      bUseMeshVertexNormalsIfAvailable )
{
    const DynamicMesh3*  Mesh            = NormalOverlay->GetParentMesh();
    bool                 bUseMeshNormals = bUseMeshVertexNormalsIfAvailable && Mesh->HasVertexNormals();
    MeshNormals          Normals( Mesh );
    if ( bUseMeshNormals == false )
    {
        Normals.ComputeVertexNormals();
    }

    NormalOverlay->ClearElements();

    std::vector<int> VertToNormalMap;
    VertToNormalMap.resize( Mesh->MaxVertexID() );
    for ( int vid : Mesh->VertexIndicesItr() )
    {
        glm::vec3 Normal     = ( bUseMeshNormals ) ? Mesh->GetVertexNormal( vid ) : (glm::vec3)Normals[vid];
        int       nid        = NormalOverlay->AppendElement( Normal );
        VertToNormalMap[vid] = nid;
    }

    for ( int tid : Mesh->TriangleIndicesItr() )
    {
        Index3i Tri  = Mesh->GetTriangle( tid );
        Tri.A        = VertToNormalMap[Tri.A];
        Tri.B        = VertToNormalMap[Tri.B];
        Tri.C        = VertToNormalMap[Tri.C];
        NormalOverlay->SetTriangle( tid, Tri );
    }
}

void MeshNormals::InitializeOverlayToPerTriangleNormals( DynamicMeshNormalOverlay* NormalOverlay )
{
    const DynamicMesh3* Mesh = NormalOverlay->GetParentMesh();

    NormalOverlay->ClearElements();

    for ( int32_t const tid : Mesh->TriangleIndicesItr() )
    {
        glm::dvec3    Normal = Mesh->GetTriNormal( tid );
        int32_t const e0     = NormalOverlay->AppendElement( glm::vec3( Normal ) );
        int32_t const e1     = NormalOverlay->AppendElement( glm::vec3( Normal ) );
        int32_t const e2     = NormalOverlay->AppendElement( glm::vec3( Normal ) );
        NormalOverlay->SetTriangle( tid, Index3i( e0, e1, e2 ) );
    }
}

void MeshNormals::InitializeOverlayTopologyFromOpeningAngle( const DynamicMesh3*       Mesh,
                                                             DynamicMeshNormalOverlay* NormalOverlay,
                                                             double                    AngleThresholdDeg )
{
    double NormalDotProdThreshold = std::cos( AngleThresholdDeg * ( glm::pi<double>() / double( 180 ) ) );

    MeshNormals FaceNormals( Mesh );
    FaceNormals.ComputeTriangleNormals();
    const std::vector<glm::dvec3>& Normals = FaceNormals.GetNormals();
    NormalOverlay->CreateFromPredicate( [&Normals, &NormalDotProdThreshold]( int VID, int TA, int TB )
                                        { return glm::dot( Normals[TA], Normals[TB] ) > NormalDotProdThreshold; },
                                        0 );
}

void MeshNormals::InitializeOverlayTopologyFromFaceGroups( const DynamicMesh3*       Mesh,
                                                           DynamicMeshNormalOverlay* NormalOverlay )
{
    DESERT_VERIFY_WARN( Mesh->HasTriangleGroups() );
    NormalOverlay->CreateFromPredicate( [Mesh]( int VID, int TA, int TB )
                                        { return Mesh->GetTriangleGroup( TA ) == Mesh->GetTriangleGroup( TB ); },
                                        0 );
}

void MeshNormals::InitializeMeshToPerTriangleNormals( DynamicMesh3* Mesh )
{
    if ( Mesh->HasAttributes() == false )
    {
        Mesh->EnableAttributes();
    }
    DynamicMeshNormalOverlay* Overlay = Mesh->Attributes()->PrimaryNormals();
    InitializeOverlayToPerTriangleNormals( Overlay );
}

void MeshNormals::InitializeOverlayRegionToPerVertexNormals( DynamicMeshNormalOverlay*   NormalOverlay,
                                                             const std::vector<int32_t>& Triangles )
{
    const DynamicMesh3* Mesh = NormalOverlay->GetParentMesh();

    // should we remove existing elements that may become unreferenced?

    std::unordered_set<int32_t> TriangleSet( Triangles.begin(), Triangles.end() );
    std::vector<int32_t>        Vertices;
    TriangleToVertexIDs( Mesh, Triangles, Vertices );
    auto          TriangleSetFunc = [&]( int32_t tid ) { return TriangleSet.contains( tid ); };
    int32_t const NumVertices     = static_cast<int32_t>( Vertices.size() );
    std::unordered_map<int32_t, int32_t> TriangleMap;
    TriangleMap.reserve( NumVertices );

    std::vector<int32_t> VertNormals;
    VertNormals.resize( NumVertices );
    for ( int32_t i = 0; i < NumVertices; ++i )
    {
        int32_t const   vid    = Vertices[i];
        glm::dvec3 const Normal = MeshNormals::ComputeVertexNormal(
             *Mesh, vid, std::function<bool( int32_t )>( TriangleSetFunc ), true, true );
        int32_t const nid = NormalOverlay->AppendElement( glm::vec3( Normal ) );
        VertNormals[i] = nid;

        TriangleMap.insert_or_assign( vid, i );
    }

    for ( int32_t const tid : Triangles )
    {
        Index3i Tri  = Mesh->GetTriangle( tid );
        Tri.A        = VertNormals[TriangleMap[Tri.A]];
        Tri.B        = VertNormals[TriangleMap[Tri.B]];
        Tri.C        = VertNormals[TriangleMap[Tri.C]];
        NormalOverlay->SetTriangle( tid, Tri );
    }
}

glm::dvec3 MeshNormals::GetVertexWeightsOnTriangle( const DynamicMesh3* Mesh, int TriID, double TriArea,
                                                    bool bWeightByArea, bool bWeightByAngle )
{
    return GetVertexWeightsOnTriangleImpl( *Mesh, TriID, TriArea, bWeightByArea, bWeightByAngle );
}
