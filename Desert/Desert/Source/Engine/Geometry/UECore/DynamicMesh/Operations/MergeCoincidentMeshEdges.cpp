// Ported from UE 5.8
// Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/Operations/MergeCoincidentMeshEdges.cpp,
// Public/Spatial/PointHashGrid3.h (FindPointsInBall, InsertPointUnsafe) and Public/Util/IndexPriorityQueue.h
// (Initialize, Insert, Dequeue); see the header for the adaptations.
#include "Engine/Geometry/UECore/DynamicMesh/Operations/MergeCoincidentMeshEdges.hpp"

#include "Engine/Geometry/UECore/IndexPriorityQueue.hpp"

#include <array>
#include <cmath>
#include <map>
#include <memory>

namespace Desert::Geometry
{
    namespace
    {
        // TPointHashGrid3<int32_t, double> with FScaleGridIndexer3 at the origin.
        class PointHashGrid3
        {
        public:
            explicit PointHashGrid3( double CellSize ) : CellSize( CellSize )
            {
            }
            void InsertPointUnsafe( int32_t Value, const glm::dvec3& Pos )
            {
                Hash[ToGrid( Pos )].push_back( Value );
            }
            template <typename DistanceSqFn>
            void FindPointsInBall( const glm::dvec3& QueryPoint, double Radius, DistanceSqFn&& DistanceSqFunc,
                                   std::vector<int32_t>& ResultsOut ) const
            {
                const glm::dvec3 Lo( QueryPoint.x - Radius, QueryPoint.y - Radius, QueryPoint.z - Radius );
                const glm::dvec3 Hi( QueryPoint.x + Radius, QueryPoint.y + Radius, QueryPoint.z + Radius );
                const auto      MinIdx = ToGrid( Lo ), MaxIdx = ToGrid( Hi );
                const double    RadiusSquared = Radius * Radius;
                for ( int64_t zi = MinIdx[2]; zi <= MaxIdx[2]; zi++ )
                    for ( int64_t yi = MinIdx[1]; yi <= MaxIdx[1]; yi++ )
                        for ( int64_t xi = MinIdx[0]; xi <= MaxIdx[0]; xi++ )
                        {
                            const auto It = Hash.find( { xi, yi, zi } );
                            if ( It == Hash.end() )
                                continue;
                            for ( int32_t const Value : It->second )
                                if ( DistanceSqFunc( Value ) < RadiusSquared )
                                    ResultsOut.push_back( Value );
                        }
            }

        private:
            using Key = std::array<int64_t, 3>;
            Key ToGrid( const glm::dvec3& P ) const
            {
                return { static_cast<int64_t>( std::floor( P.x / CellSize ) ),
                         static_cast<int64_t>( std::floor( P.y / CellSize ) ),
                         static_cast<int64_t>( std::floor( P.z / CellSize ) ) };
            }
            double                       CellSize;
            std::map<Key, std::vector<int32_t>> Hash;
        };

    } // namespace

    const double MergeCoincidentMeshEdges::DEFAULT_TOLERANCE = FMathf::ZeroTolerance;

    bool MergeCoincidentMeshEdges::Apply()
    {
        MergeVtxDistSqr          = MergeVertexTolerance * MergeVertexTolerance;
        double UseMergeSearchTol = ( MergeSearchTolerance > 0 ) ? MergeSearchTolerance : 2 * MergeVertexTolerance;

        // hash table of the boundary edge midpoints
        std::vector<glm::dvec3> BoundaryMidPoints;
        std::vector<int32_t>    ToMidPt;
        ToMidPt.assign( Mesh->MaxEdgeID(), -1 );
        for ( int32_t const EID : Mesh->BoundaryEdgeIndicesItr() )
        {
            BoundaryMidPoints.push_back( Mesh->GetEdgePoint( EID, 0.5 ) );
            ToMidPt[EID] = static_cast<int32_t>( BoundaryMidPoints.size() ) - 1;
        }
        InitialNumBoundaryEdges = static_cast<int32_t>( BoundaryMidPoints.size() );

        // denser grid as the number of boundary edges grows
        int hashN = 64;
        if ( InitialNumBoundaryEdges > 1000 )
            hashN = 128;
        if ( InitialNumBoundaryEdges > 10000 )
            hashN = 256;
        if ( InitialNumBoundaryEdges > 100000 )
            hashN = 512;

        const AxisAlignedBox3d  Bounds   = Mesh->GetBounds();
        const double            MaxDim   = std::max( Bounds.Max.x - Bounds.Min.x,
                                                     std::max( Bounds.Max.y - Bounds.Min.y, Bounds.Max.z - Bounds.Min.z ) );
        const double CellSize = std::max( FMathd::ZeroTolerance, MaxDim / static_cast<double>( hashN ) );
        PointHashGrid3          MidpointsHash( CellSize );
        UseMergeSearchTol = std::min( CellSize, UseMergeSearchTol );

        glm::dvec3           A{}, B{}, C{}, D{};
        std::vector<int>     equivBuffer;
        std::vector<int32_t> SearchMatches;

        // Edge equivalence sets: every other boundary edge with the same midpoint, narrowed to those with the same
        // endpoints.
        using EdgesList = std::vector<int>;
        std::vector<std::unique_ptr<EdgesList>> EquivalenceSets( static_cast<size_t>( Mesh->MaxEdgeID() ) );
        std::unordered_set<int>                 RemainingEdges;
        for ( int eid : Mesh->BoundaryEdgeIndicesItr() )
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

            Mesh->GetEdgeV( eid, A, B );
            equivBuffer.clear();
            for ( int i = 0; i < N; ++i )
            {
                const int32_t MatchEID = SearchMatches[i];
                Mesh->GetEdgeV( MatchEID, C, D );
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
        DuplicatesQueue.Initialize( Mesh->MaxEdgeID() );
        for ( int eid : std::vector( RemainingEdges.begin(), RemainingEdges.end() ) )
        {
            if ( OnlyUniquePairs )
            {
                if ( static_cast<int32_t>( EquivalenceSets[eid]->size() ) != 1 )
                    continue;
                // the reverse match must be the same and unique
                const int other_eid = ( *EquivalenceSets[eid] )[0];
                if ( static_cast<int32_t>( EquivalenceSets[other_eid]->size() ) != 1 ||
                     ( *EquivalenceSets[other_eid] )[0] != eid )
                    continue;
            }
            DuplicatesQueue.Insert( eid, static_cast<int32_t>( (float)EquivalenceSets[eid]->size() ) );
        }

        // greedy merge
        while ( DuplicatesQueue.GetCount() > 0 )
        {
            const int eid = DuplicatesQueue.Dequeue();
            if ( !Mesh->IsEdge( eid ) || !EquivalenceSets[eid] || !RemainingEdges.contains( eid ) )
                continue; // dealt with already
            if ( !Mesh->IsBoundaryEdge( eid ) )
                continue; // merged already

            EdgesList& Matches = *EquivalenceSets[eid];
            bool       bMerged = false;
            for ( int i = 0; i < static_cast<int32_t>( Matches.size() ) && !bMerged; ++i )
            {
                const int other_eid = Matches[i];
                if ( !Mesh->IsEdge( other_eid ) || !Mesh->IsBoundaryEdge( other_eid ) )
                    continue;
                const bool bWeldingAcrossEntireMesh = ( EdgesToMerge == nullptr );
                if ( !bWeldingAcrossEntireMesh && !EdgesToMerge->contains( eid ) &&
                     !EdgesToMerge->contains( other_eid ) )
                    continue;

                DynamicMesh3::MergeEdgesInfo MergeInfo;
                const MeshResult             Result = Mesh->MergeEdges( eid, other_eid, MergeInfo );
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
                    if ( bWeldAttrsOnMergedEdges )
                    {
                        SplitAttributeWelder.WeldSplitElements( *Mesh, MergeInfo.KeptVerts[0] );
                        SplitAttributeWelder.WeldSplitElements( *Mesh, MergeInfo.KeptVerts[1] );
                    }
                }
            }
            EquivalenceSets[eid].reset();
            RemainingEdges.erase( eid );
        }

        FinalNumBoundaryEdges = 0;
        for ( int eid : Mesh->BoundaryEdgeIndicesItr() )
        {
            (void)eid;
            FinalNumBoundaryEdges++;
        }
        return true;
    }
} // namespace Desert::Geometry
