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
        class FPointHashGrid3
        {
        public:
            explicit FPointHashGrid3( double CellSize ) : CellSize( CellSize )
            {
            }
            void InsertPointUnsafe( int32_t Value, const FVector3d& Pos )
            {
                Hash[ToGrid( Pos )].Add( Value );
            }
            template <typename DistanceSqFn>
            void FindPointsInBall( const FVector3d& QueryPoint, double Radius, DistanceSqFn&& DistanceSqFunc,
                                   TArray<int32_t>& ResultsOut ) const
            {
                const FVector3d Lo( QueryPoint.X - Radius, QueryPoint.Y - Radius, QueryPoint.Z - Radius );
                const FVector3d Hi( QueryPoint.X + Radius, QueryPoint.Y + Radius, QueryPoint.Z + Radius );
                const auto      MinIdx = ToGrid( Lo ), MaxIdx = ToGrid( Hi );
                const double    RadiusSquared = Radius * Radius;
                for ( int64_t zi = MinIdx[2]; zi <= MaxIdx[2]; zi++ )
                    for ( int64_t yi = MinIdx[1]; yi <= MaxIdx[1]; yi++ )
                        for ( int64_t xi = MinIdx[0]; xi <= MaxIdx[0]; xi++ )
                        {
                            const auto It = Hash.find( { xi, yi, zi } );
                            if ( It == Hash.end() )
                                continue;
                            for ( int32_t Value : It->second )
                                if ( DistanceSqFunc( Value ) < RadiusSquared )
                                    ResultsOut.Add( Value );
                        }
            }

        private:
            using Key = std::array<int64_t, 3>;
            Key ToGrid( const FVector3d& P ) const
            {
                return { static_cast<int64_t>( std::floor( P.X / CellSize ) ),
                         static_cast<int64_t>( std::floor( P.Y / CellSize ) ),
                         static_cast<int64_t>( std::floor( P.Z / CellSize ) ) };
            }
            double                       CellSize;
            std::map<Key, TArray<int32_t>> Hash;
        };

    } // namespace

    const double FMergeCoincidentMeshEdges::DEFAULT_TOLERANCE = FMathf::ZeroTolerance;

    bool FMergeCoincidentMeshEdges::Apply()
    {
        MergeVtxDistSqr          = MergeVertexTolerance * MergeVertexTolerance;
        double UseMergeSearchTol = ( MergeSearchTolerance > 0 ) ? MergeSearchTolerance : 2 * MergeVertexTolerance;

        // hash table of the boundary edge midpoints
        TArray<FVector3d> BoundaryMidPoints;
        TArray<int32_t>   ToMidPt;
        ToMidPt.Init( -1, Mesh->MaxEdgeID() );
        for ( int32_t EID : Mesh->BoundaryEdgeIndicesItr() )
            ToMidPt[EID] = BoundaryMidPoints.Add( Mesh->GetEdgePoint( EID, 0.5 ) );
        InitialNumBoundaryEdges = BoundaryMidPoints.Num();

        // denser grid as the number of boundary edges grows
        int hashN = 64;
        if ( InitialNumBoundaryEdges > 1000 )
            hashN = 128;
        if ( InitialNumBoundaryEdges > 10000 )
            hashN = 256;
        if ( InitialNumBoundaryEdges > 100000 )
            hashN = 512;

        const FAxisAlignedBox3d Bounds   = Mesh->GetBounds();
        const double            MaxDim   = std::max( Bounds.Max.X - Bounds.Min.X,
                                                     std::max( Bounds.Max.Y - Bounds.Min.Y, Bounds.Max.Z - Bounds.Min.Z ) );
        const double            CellSize = std::max( FMathd::ZeroTolerance, MaxDim / (double)hashN );
        FPointHashGrid3         MidpointsHash( CellSize );
        UseMergeSearchTol = std::min( CellSize, UseMergeSearchTol );

        FVector3d     A, B, C, D;
        TArray<int>   equivBuffer;
        TArray<int32_t> SearchMatches;

        // Edge equivalence sets: every other boundary edge with the same midpoint, narrowed to those with the same
        // endpoints.
        using EdgesList = TArray<int>;
        std::vector<std::unique_ptr<EdgesList>> EquivalenceSets( static_cast<size_t>( Mesh->MaxEdgeID() ) );
        TSet<int>                               RemainingEdges;
        for ( int eid : Mesh->BoundaryEdgeIndicesItr() )
        {
            const FVector3d midpt = BoundaryMidPoints[ToMidPt[eid]];
            SearchMatches.Reset();
            MidpointsHash.FindPointsInBall(
                 midpt, UseMergeSearchTol, [&]( const int32_t& PtIdx )
                 { return DistSq( midpt, BoundaryMidPoints[ToMidPt[PtIdx]] ); }, SearchMatches );
            // inserted after the query, so only edges with earlier IDs are found
            MidpointsHash.InsertPointUnsafe( eid, midpt );
            const int N = SearchMatches.Num();
            if ( N == 0 )
                continue;

            Mesh->GetEdgeV( eid, A, B );
            equivBuffer.Reset();
            for ( int i = 0; i < N; ++i )
            {
                const int32_t MatchEID = SearchMatches[i];
                Mesh->GetEdgeV( MatchEID, C, D );
                if ( IsSameEdge( A, B, C, D ) )
                {
                    equivBuffer.Add( MatchEID );
                    if ( !EquivalenceSets[MatchEID] )
                    {
                        EquivalenceSets[MatchEID] = std::make_unique<EdgesList>();
                        RemainingEdges.Add( MatchEID );
                    }
                    EquivalenceSets[MatchEID]->Add( eid );
                }
            }
            if ( equivBuffer.Num() > 0 )
            {
                EquivalenceSets[eid] = std::make_unique<EdgesList>( equivBuffer );
                RemainingEdges.Add( eid );
            }
        }

        // potential duplicates, fewest possible matches first
        FIndexPriorityQueue DuplicatesQueue;
        DuplicatesQueue.Initialize( Mesh->MaxEdgeID() );
        for ( int eid : RemainingEdges.Array() )
        {
            if ( OnlyUniquePairs )
            {
                if ( EquivalenceSets[eid]->Num() != 1 )
                    continue;
                // the reverse match must be the same and unique
                const int other_eid = ( *EquivalenceSets[eid] )[0];
                if ( EquivalenceSets[other_eid]->Num() != 1 || ( *EquivalenceSets[other_eid] )[0] != eid )
                    continue;
            }
            DuplicatesQueue.Insert( eid, (float)EquivalenceSets[eid]->Num() );
        }

        // greedy merge
        while ( DuplicatesQueue.GetCount() > 0 )
        {
            const int eid = DuplicatesQueue.Dequeue();
            if ( !Mesh->IsEdge( eid ) || !EquivalenceSets[eid] || !RemainingEdges.Contains( eid ) )
                continue; // dealt with already
            if ( !Mesh->IsBoundaryEdge( eid ) )
                continue; // merged already

            EdgesList& Matches = *EquivalenceSets[eid];
            bool       bMerged = false;
            for ( int i = 0; i < Matches.Num() && !bMerged; ++i )
            {
                const int other_eid = Matches[i];
                if ( !Mesh->IsEdge( other_eid ) || !Mesh->IsBoundaryEdge( other_eid ) )
                    continue;
                const bool bWeldingAcrossEntireMesh = ( EdgesToMerge == nullptr );
                if ( !bWeldingAcrossEntireMesh && !EdgesToMerge->Contains( eid ) &&
                     !EdgesToMerge->Contains( other_eid ) )
                    continue;

                FDynamicMesh3::FMergeEdgesInfo MergeInfo;
                const EMeshResult              Result = Mesh->MergeEdges( eid, other_eid, MergeInfo );
                if ( Result != EMeshResult::Ok )
                {
                    // a failed pair leaves both equivalence sets
                    Matches.RemoveAt( i );
                    i--;
                    if ( EquivalenceSets[other_eid] )
                        EquivalenceSets[other_eid]->Remove( eid );
                }
                else
                {
                    bMerged = true;
                    EquivalenceSets[other_eid].reset();
                    RemainingEdges.Remove( other_eid );
                    if ( bWeldAttrsOnMergedEdges )
                    {
                        SplitAttributeWelder.WeldSplitElements( *Mesh, MergeInfo.KeptVerts[0] );
                        SplitAttributeWelder.WeldSplitElements( *Mesh, MergeInfo.KeptVerts[1] );
                    }
                }
            }
            EquivalenceSets[eid].reset();
            RemainingEdges.Remove( eid );
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
