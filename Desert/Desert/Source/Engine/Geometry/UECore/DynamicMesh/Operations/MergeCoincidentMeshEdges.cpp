// Ported from UE 5.8
// Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/Operations/MergeCoincidentMeshEdges.cpp,
// Public/Spatial/PointHashGrid3.h (FindPointsInBall, InsertPointUnsafe) and Public/Util/IndexPriorityQueue.h
// (Initialize, Insert, Dequeue); see the header for the adaptations.
#include "Engine/Geometry/UECore/DynamicMesh/Operations/MergeCoincidentMeshEdges.hpp"

#include <array>
#include <cmath>
#include <map>
#include <memory>

namespace Desert::Geometry
{
    namespace
    {
        // TPointHashGrid3<int32, double> with FScaleGridIndexer3 at the origin.
        class FPointHashGrid3
        {
        public:
            explicit FPointHashGrid3( double CellSize ) : CellSize( CellSize )
            {
            }
            void InsertPointUnsafe( int32 Value, const FVector3d& Pos )
            {
                Hash[ToGrid( Pos )].Add( Value );
            }
            template <typename DistanceSqFn>
            void FindPointsInBall( const FVector3d& QueryPoint, double Radius, DistanceSqFn&& DistanceSqFunc,
                                   TArray<int32>& ResultsOut ) const
            {
                const FVector3d Lo( QueryPoint.X - Radius, QueryPoint.Y - Radius, QueryPoint.Z - Radius );
                const FVector3d Hi( QueryPoint.X + Radius, QueryPoint.Y + Radius, QueryPoint.Z + Radius );
                const auto      MinIdx = ToGrid( Lo ), MaxIdx = ToGrid( Hi );
                const double    RadiusSquared = Radius * Radius;
                for ( int64 zi = MinIdx[2]; zi <= MaxIdx[2]; zi++ )
                    for ( int64 yi = MinIdx[1]; yi <= MaxIdx[1]; yi++ )
                        for ( int64 xi = MinIdx[0]; xi <= MaxIdx[0]; xi++ )
                        {
                            const auto It = Hash.find( { xi, yi, zi } );
                            if ( It == Hash.end() )
                                continue;
                            for ( int32 Value : It->second )
                                if ( DistanceSqFunc( Value ) < RadiusSquared )
                                    ResultsOut.Add( Value );
                        }
            }

        private:
            using Key = std::array<int64, 3>;
            Key ToGrid( const FVector3d& P ) const
            {
                return { static_cast<int64>( std::floor( P.X / CellSize ) ),
                         static_cast<int64>( std::floor( P.Y / CellSize ) ),
                         static_cast<int64>( std::floor( P.Z / CellSize ) ) };
            }
            double                       CellSize;
            std::map<Key, TArray<int32>> Hash;
        };

        // FIndexPriorityQueue: a binary min-heap stored 1-based in a flat array, with an ID -> heap index map.
        class FIndexPriorityQueue
        {
        public:
            void Initialize( int MaxNodeID )
            {
                Nodes.Reset();
                Nodes.Add( {} ); // [0] unused, as UE
                IdToIndex.Init( 0, MaxNodeID );
            }
            int GetCount() const
            {
                return Nodes.Num() - 1;
            }
            void Insert( int NodeID, float Priority )
            {
                const int Index   = Nodes.Add( { NodeID, Priority } );
                IdToIndex[NodeID] = Index;
                MoveUp( Index );
            }
            int Dequeue()
            {
                const int Head = Nodes[1].Id;
                const int Last = GetCount();
                Swap( 1, Last );
                Nodes.RemoveAt( Last );
                IdToIndex[Head] = 0;
                if ( GetCount() > 0 )
                    MoveDown( 1 );
                return Head;
            }

        private:
            struct FNode
            {
                int   Id       = -1;
                float Priority = 0;
            };
            void Swap( int A, int B )
            {
                std::swap( Nodes[A], Nodes[B] );
                IdToIndex[Nodes[A].Id] = A;
                IdToIndex[Nodes[B].Id] = B;
            }
            void MoveUp( int Index )
            {
                while ( Index > 1 && Nodes[Index / 2].Priority > Nodes[Index].Priority )
                {
                    Swap( Index, Index / 2 );
                    Index /= 2;
                }
            }
            void MoveDown( int Index )
            {
                const int Count = GetCount();
                for ( ;; )
                {
                    int       Smallest = Index;
                    const int Left = 2 * Index, Right = 2 * Index + 1;
                    if ( Left <= Count && Nodes[Left].Priority < Nodes[Smallest].Priority )
                        Smallest = Left;
                    if ( Right <= Count && Nodes[Right].Priority < Nodes[Smallest].Priority )
                        Smallest = Right;
                    if ( Smallest == Index )
                        return;
                    Swap( Index, Smallest );
                    Index = Smallest;
                }
            }
            TArray<FNode> Nodes;
            TArray<int>   IdToIndex;
        };
    } // namespace

    const double FMergeCoincidentMeshEdges::DEFAULT_TOLERANCE = FMathf::ZeroTolerance;

    bool FMergeCoincidentMeshEdges::Apply()
    {
        MergeVtxDistSqr          = MergeVertexTolerance * MergeVertexTolerance;
        double UseMergeSearchTol = ( MergeSearchTolerance > 0 ) ? MergeSearchTolerance : 2 * MergeVertexTolerance;

        // hash table of the boundary edge midpoints
        TArray<FVector3d> BoundaryMidPoints;
        TArray<int32>     ToMidPt;
        ToMidPt.Init( -1, Mesh->MaxEdgeID() );
        for ( int32 EID : Mesh->BoundaryEdgeIndicesItr() )
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
        const double            MaxDim   = FMath::Max( Bounds.Max.X - Bounds.Min.X,
                                                       FMath::Max( Bounds.Max.Y - Bounds.Min.Y, Bounds.Max.Z - Bounds.Min.Z ) );
        const double            CellSize = FMath::Max( FMathd::ZeroTolerance, MaxDim / (double)hashN );
        FPointHashGrid3         MidpointsHash( CellSize );
        UseMergeSearchTol = FMath::Min( CellSize, UseMergeSearchTol );

        FVector3d     A, B, C, D;
        TArray<int>   equivBuffer;
        TArray<int32> SearchMatches;

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
                 midpt, UseMergeSearchTol, [&]( const int32& PtIdx )
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
                const int32 MatchEID = SearchMatches[i];
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
