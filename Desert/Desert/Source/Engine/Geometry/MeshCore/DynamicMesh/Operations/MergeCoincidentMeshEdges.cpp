// Ported from UE 5.8
// Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/Operations/MergeCoincidentMeshEdges.cpp,
// Public/Util/IndexPriorityQueue.h
// (Initialize, Insert, Dequeue); see the header for the adaptations.
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/MergeCoincidentMeshEdges.hpp"

#include "Engine/Geometry/MeshCore/IndexPriorityQueue.hpp"
#include "Engine/Geometry/MeshCore/Spatial/PointHashGrid3.hpp"

#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <unordered_set>

namespace Desert::Geometry
{
    const double MergeCoincidentMeshEdges::DEFAULT_TOLERANCE = ZeroTolerance<float>;

    bool MergeCoincidentMeshEdges::Apply()
    {
        m_MergeVtxDistSqr = m_MergeVertexTolerance * m_MergeVertexTolerance;
        double UseMergeSearchTol =
             ( m_MergeSearchTolerance > 0 ) ? m_MergeSearchTolerance : 2 * m_MergeVertexTolerance;

        // hash table of the boundary edge midpoints
        std::vector<glm::dvec3> BoundaryMidPoints;
        std::vector<int32_t>    ToMidPt;
        ToMidPt.assign( m_Mesh->MaxEdgeID(), -1 );
        for ( int32_t const EID : m_Mesh->BoundaryEdgeIndicesItr() )
        {
            BoundaryMidPoints.push_back( m_Mesh->GetEdgePoint( EID, 0.5 ) );
            ToMidPt[EID] = static_cast<int32_t>( BoundaryMidPoints.size() ) - 1;
        }
        m_InitialNumBoundaryEdges = static_cast<int32_t>( BoundaryMidPoints.size() );

        // denser grid as the number of boundary edges grows
        int hashN = 64;
        if ( m_InitialNumBoundaryEdges > 1000 )
            hashN = 128;
        if ( m_InitialNumBoundaryEdges > 10000 )
            hashN = 256;
        if ( m_InitialNumBoundaryEdges > 100000 )
            hashN = 512;

        const AxisAlignedBox3d Bounds   = m_Mesh->GetBounds();
        const double           MaxDim   = std::max( Bounds.Max.x - Bounds.Min.x,
                                                    std::max( Bounds.Max.y - Bounds.Min.y, Bounds.Max.z - Bounds.Min.z ) );
        const double           CellSize = std::max( ZeroTolerance<double>, MaxDim / static_cast<double>( hashN ) );
        PointHashGrid3         MidpointsHash( CellSize );
        UseMergeSearchTol = std::min( CellSize, UseMergeSearchTol );

        glm::dvec3           A{};
        glm::dvec3           B{};
        glm::dvec3           C{};
        glm::dvec3           D{};
        std::vector<int>     equivBuffer;
        std::vector<int32_t> SearchMatches;

        // Edge equivalence sets: every other boundary edge with the same midpoint, narrowed to those with the same
        // endpoints.
        using EdgesList = std::vector<int>;
        std::vector<std::unique_ptr<EdgesList>> EquivalenceSets( static_cast<size_t>( m_Mesh->MaxEdgeID() ) );
        std::unordered_set<int>                 RemainingEdges;
        for ( int const eid : m_Mesh->BoundaryEdgeIndicesItr() )
        {
            const glm::dvec3 midpt = BoundaryMidPoints[ToMidPt[eid]];
            SearchMatches.clear();
            MidpointsHash.FindPointsInBall(
                 midpt, UseMergeSearchTol, [&]( const int32_t& PtIdx )
                 { return DistSq( midpt, BoundaryMidPoints[ToMidPt[PtIdx]] ); }, SearchMatches );
            // inserted after the query, so only edges with earlier IDs are found
            MidpointsHash.InsertPointUnsafe( eid, midpt );
            const int N = static_cast<int32_t>( SearchMatches.size() );
            if ( N == 0 )
                continue;

            m_Mesh->GetEdgeV( eid, A, B );
            equivBuffer.clear();
            for ( int i = 0; i < N; ++i )
            {
                const int32_t MatchEID = SearchMatches[i];
                m_Mesh->GetEdgeV( MatchEID, C, D );
                if ( IsSameEdge( A, B, C, D ) )
                {
                    equivBuffer.push_back( MatchEID );
                    if ( !EquivalenceSets[MatchEID] )
                    {
                        EquivalenceSets[MatchEID] = std::make_unique<EdgesList>();
                        RemainingEdges.insert( MatchEID );
                    }
                    EquivalenceSets[MatchEID]->push_back( eid );
                }
            }
            if ( !equivBuffer.empty() )
            {
                EquivalenceSets[eid] = std::make_unique<EdgesList>( equivBuffer );
                RemainingEdges.insert( eid );
            }
        }

        // potential duplicates, fewest possible matches first
        IndexPriorityQueue DuplicatesQueue;
        DuplicatesQueue.Initialize( m_Mesh->MaxEdgeID() );
        for ( int const eid : std::vector( RemainingEdges.begin(), RemainingEdges.end() ) )
        {
            if ( m_OnlyUniquePairs )
            {
                if ( static_cast<int32_t>( EquivalenceSets[eid]->size() ) != 1 )
                    continue;
                // the reverse match must be the same and unique
                const int other_eid = ( *EquivalenceSets[eid] )[0];
                if ( static_cast<int32_t>( EquivalenceSets[other_eid]->size() ) != 1 ||
                     ( *EquivalenceSets[other_eid] )[0] != eid )
                    continue;
            }
            DuplicatesQueue.Insert( eid, static_cast<float>( EquivalenceSets[eid]->size() ) );
        }

        // greedy merge
        while ( DuplicatesQueue.GetCount() > 0 )
        {
            const int eid = DuplicatesQueue.Dequeue();
            if ( !m_Mesh->IsEdge( eid ) || !EquivalenceSets[eid] || !RemainingEdges.contains( eid ) )
                continue; // dealt with already
            if ( !m_Mesh->IsBoundaryEdge( eid ) )
                continue; // merged already

            EdgesList& Matches = *EquivalenceSets[eid];
            bool       bMerged = false;
            for ( int i = 0; i < static_cast<int32_t>( Matches.size() ) && !bMerged; ++i )
            {
                const int other_eid = Matches[i];
                if ( !m_Mesh->IsEdge( other_eid ) || !m_Mesh->IsBoundaryEdge( other_eid ) )
                    continue;
                const bool bWeldingAcrossEntireMesh = ( m_EdgesToMerge == nullptr );
                if ( !bWeldingAcrossEntireMesh && !m_EdgesToMerge->contains( eid ) &&
                     !m_EdgesToMerge->contains( other_eid ) )
                    continue;

                DynamicMesh3::MergeEdgesInfo MergeInfo;
                const MeshResult             Result = m_Mesh->MergeEdges( eid, other_eid, MergeInfo, true );
                if ( Result != MeshResult::Ok )
                {
                    // a failed pair leaves both equivalence sets
                    Matches.erase( Matches.begin() + i );
                    i--;
                    if ( EquivalenceSets[other_eid] )
                        std::erase( ( *EquivalenceSets[other_eid] ), eid );
                }
                else
                {
                    bMerged = true;
                    EquivalenceSets[other_eid].reset();
                    RemainingEdges.erase( other_eid );
                    if ( m_bWeldAttrsOnMergedEdges )
                    {
                        m_SplitAttributeWelder.WeldSplitElements( *m_Mesh, MergeInfo.KeptVerts[0] );
                        m_SplitAttributeWelder.WeldSplitElements( *m_Mesh, MergeInfo.KeptVerts[1] );
                    }
                }
            }
            EquivalenceSets[eid].reset();
            RemainingEdges.erase( eid );
        }

        m_FinalNumBoundaryEdges = 0;
        for ( int const eid : m_Mesh->BoundaryEdgeIndicesItr() )
        {
            (void)eid;
            m_FinalNumBoundaryEdges++;
        }
        return true;
    }
} // namespace Desert::Geometry
