// Ported from UE 5.8 .../DynamicMesh/Private/Operations/OffsetMeshRegion.cpp (see the header for the line ranges
// and the adaptations).
#include "Engine/Geometry/UECore/Operations/OffsetMeshRegion.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/UECore/DynamicMeshEditor.hpp"

#include <fmt/format.h>

#include <algorithm>

namespace Desert::Geometry
{
    void FindConnectedTriangleComponents( const FDynamicMesh3& Mesh, const TArray<int32>& Triangles,
                                          TArray<TArray<int32>>& ComponentsOut )
    {
        TSet<int32> Remaining( Triangles );
        TSet<int32> Visited;
        for ( int32 Seed : Triangles )
        {
            if ( Visited.Contains( Seed ) )
                continue;
            TArray<int32> Component, Stack;
            Stack.Add( Seed );
            Visited.Add( Seed );
            while ( Stack.Num() > 0 )
            {
                const int32 tid = Stack.Pop();
                Component.Add( tid );
                const FIndex3i Nbrs = Mesh.GetTriNeighbourTris( tid );
                for ( int j = 0; j < 3; ++j )
                    if ( Nbrs[j] >= 0 && Remaining.Contains( Nbrs[j] ) && !Visited.Contains( Nbrs[j] ) )
                    {
                        Visited.Add( Nbrs[j] );
                        Stack.Add( Nbrs[j] );
                    }
            }
            std::sort( Component.begin(), Component.end() );
            ComponentsOut.Add( Component );
        }
    }

    void ComputeNewGroupIDsAlongEdgeLoop( FDynamicMesh3& Mesh, const TArray<int32>& LoopEdgeIDs,
                                          TArray<int32>& NewLoopEdgeGroupIDs, TArray<int32>& NewGroupIDsOut,
                                          const TFunction<bool( int32, int32 )>& EdgesShouldHaveSameGroupFunc )
    {
        int32 NumEdgeIDs = LoopEdgeIDs.Num();
        NewLoopEdgeGroupIDs.SetNum( NumEdgeIDs );
        if ( NumEdgeIDs <= 2 )
        {
            if ( NumEdgeIDs > 0 )
            {
                int32 OneGroupID = Mesh.AllocateTriangleGroup();
                NewLoopEdgeGroupIDs.Init( OneGroupID, NumEdgeIDs );
                NewGroupIDsOut.Add( OneGroupID );
            }
            return;
        }
        NewLoopEdgeGroupIDs[0] = Mesh.AllocateTriangleGroup();
        NewGroupIDsOut.Add( NewLoopEdgeGroupIDs[0] );
        int32 LastDifferentGroupIndex = NumEdgeIDs - 1;
        while ( LastDifferentGroupIndex > 0 &&
                EdgesShouldHaveSameGroupFunc( LoopEdgeIDs[0], LoopEdgeIDs[LastDifferentGroupIndex] ) )
        {
            NewLoopEdgeGroupIDs[LastDifferentGroupIndex] = NewLoopEdgeGroupIDs[0];
            --LastDifferentGroupIndex;
        }
        for ( int32 j = 1; j <= LastDifferentGroupIndex; ++j )
        {
            if ( !EdgesShouldHaveSameGroupFunc( LoopEdgeIDs[j], LoopEdgeIDs[j - 1] ) )
            {
                NewLoopEdgeGroupIDs[j] = Mesh.AllocateTriangleGroup();
                NewGroupIDsOut.Add( NewLoopEdgeGroupIDs[j] );
            }
            else
                NewLoopEdgeGroupIDs[j] = NewLoopEdgeGroupIDs[j - 1];
        }
    }

    namespace
    {
        bool EdgesAreParallel( FDynamicMesh3* Mesh, int32 Eid1, int32 Eid2 )
        {
            FIndex2i  Vids1 = Mesh->GetEdgeV( Eid1 );
            FIndex2i  Vids2 = Mesh->GetEdgeV( Eid2 );
            FVector3d Vec1  = Mesh->GetVertex( Vids1.A ) - Mesh->GetVertex( Vids1.B );
            FVector3d Vec2  = Mesh->GetVertex( Vids2.A ) - Mesh->GetVertex( Vids2.B );
            constexpr double KindaSmall = 1e-4;
            if ( Normalize( Vec1, KindaSmall ) == 0 || Normalize( Vec2, KindaSmall ) == 0 )
                return true;
            return std::abs( Vec1.Dot( Vec2 ) ) >= 1 - KindaSmall;
        }

        int32 FindLoopShiftFromGroupIDs( const TArray<int32>& GroupIDs )
        {
            int32 N = GroupIDs.Num();
            if ( GroupIDs[0] != GroupIDs[N - 1] )
                return 0;
            for ( int32 k = 0; k < N - 1; ++k )
                if ( GroupIDs[k] != GroupIDs[k + 1] )
                    return k + 1;
            return 0;
        }

        template <typename ValueType>
        void LeftShiftArray( TArray<ValueType>& Values, int ShiftNum )
        {
            if ( ShiftNum == 0 )
                return;
            int32             N = Values.Num();
            TArray<ValueType> Tmp;
            Tmp.SetNum( N );
            for ( int32 k = 0; k < N; ++k )
                Tmp[k] = Values[( ShiftNum + k ) % N];
            Values = Tmp;
        }

        FVector3d GetAngleWeightedAverageNormal( const FDynamicMesh3& Mesh, int32 VertexID,
                                                 const TSet<int32>& TriangleList )
        {
            FVector3d ExtrusionVector( 0, 0, 0 );
            for ( int32 TriangleID : Mesh.VtxTrianglesItr( VertexID ) )
                if ( TriangleList.Contains( TriangleID ) )
                {
                    FIndex3i Triangle = Mesh.GetTriangle( TriangleID );
                    double   Angle    = Mesh.GetTriInternalAngleR( TriangleID, Triangle.IndexOf( VertexID ) );
                    ExtrusionVector   = ExtrusionVector + Mesh.GetTriNormal( TriangleID ) * Angle;
                }
            Normalize( ExtrusionVector );
            return ExtrusionVector;
        }

        FVector3d GetAngleWeightedAdjustedNormal( const FDynamicMesh3& Mesh, int32 VertexID,
                                                  const TSet<int32>& TriangleList, double MaxAdjustmentScale )
        {
            FVector3d InitialExtrusionVector = GetAngleWeightedAverageNormal( Mesh, VertexID, TriangleList );
            double    AngleSum = 0, Adjustment = 0;
            double    InvertedMaxScale = std::max( 1e-8, 1.0 / MaxAdjustmentScale );
            for ( int32 TriangleID : Mesh.VtxTrianglesItr( VertexID ) )
                if ( TriangleList.Contains( TriangleID ) )
                {
                    FIndex3i Triangle = Mesh.GetTriangle( TriangleID );
                    double   Angle    = Mesh.GetTriInternalAngleR( TriangleID, Triangle.IndexOf( VertexID ) );
                    double   CosTheta = Mesh.GetTriNormal( TriangleID ).Dot( InitialExtrusionVector );
                    CosTheta          = std::max( CosTheta, InvertedMaxScale );
                    Adjustment += Angle / CosTheta;
                    AngleSum += Angle;
                }
            Adjustment /= AngleSum;
            return InitialExtrusionVector * Adjustment;
        }

        // UE ComputeAverageUVScaleRatioAlongVertexPath (PolyEditingUVUtil.cpp:55): UV length over mesh length
        // along the path's edges that have UVs; 0 when none do.
        double UVScaleRatioAlongPath( const FDynamicMesh3& Mesh, const FDynamicMeshUVOverlay& UVOverlay,
                                      const TArray<int32>& VertexPath, double& PathLengthOut )
        {
            double MeshLength = 0, UVLength = 0;
            for ( int32 k = 0; k + 1 < VertexPath.Num(); ++k )
            {
                int32 EdgeID = Mesh.FindEdge( VertexPath[k], VertexPath[k + 1] );
                if ( EdgeID == FDynamicMesh3::InvalidID )
                    continue;
                double   EdgeLength = Distance( Mesh.GetVertex( VertexPath[k] ), Mesh.GetVertex( VertexPath[k + 1] ) );
                FIndex2i EdgeTris   = Mesh.GetEdgeT( EdgeID );
                double   EdgeUV     = 0;
                int      Count      = 0;
                for ( int32 j = 0; j < 2; ++j )
                {
                    const int t = j == 0 ? EdgeTris.A : EdgeTris.B;
                    if ( t == FDynamicMesh3::InvalidID || !UVOverlay.IsSetTriangle( t ) )
                        continue;
                    int32 EdgeIdx = Mesh.GetTriEdges( t ).IndexOf( EdgeID );
                    if ( EdgeIdx < 0 )
                        continue;
                    FVector2f UVs[3];
                    UVOverlay.GetTriElements( t, UVs[0], UVs[1], UVs[2] );
                    const FVector2f D = UVs[EdgeIdx] - UVs[( EdgeIdx + 1 ) % 3];
                    EdgeUV += std::sqrt( (double)D.X * D.X + (double)D.Y * D.Y );
                    Count++;
                }
                if ( Count > 0 )
                {
                    MeshLength += EdgeLength;
                    UVLength += EdgeUV / Count;
                }
            }
            PathLengthOut = MeshLength;
            return MeshLength > 0 ? UVLength / MeshLength : 0.0;
        }

        // One strip of quads: Quads[q] joins Outer[q], Outer[q+1] (row 0, the base loop) to Inner[q], Inner[q+1]
        // (row 1, the offset loop) - UE's FQuadGridPatch after its second ReverseRows, NumSubdivisions = 0.
        struct FStrip
        {
            TArray<int32>    Outer, Inner; // Columns + 1 vertices each
            TArray<FIndex2i> Quads;
        };

        void ComputeUVIslandForStrip( FDynamicMesh3& Mesh, const FStrip& Strip, double UVScaleFactor )
        {
            FDynamicMeshUVOverlay* UVOverlay = Mesh.Attributes()->PrimaryUV();
            if ( !UVOverlay )
                return;
            for ( const FIndex2i& Q : Strip.Quads )
            {
                UVOverlay->UnsetTriangle( Q.A );
                UVOverlay->UnsetTriangle( Q.B );
            }
            double UVLengthScale = 0, UVLengthWeight = 0;
            for ( int32 k = 0; k < 2; ++k )
            {
                double PathLength = 0;
                double Ratio      = UVScaleRatioAlongPath( Mesh, *UVOverlay, k == 0 ? Strip.Outer : Strip.Inner,
                                                           PathLength );
                if ( PathLength > 0 )
                {
                    UVLengthScale += Ratio;
                    UVLengthWeight += 1.0;
                }
            }
            UVLengthScale = ( UVLengthWeight == 0 || UVLengthScale == 0 ) ? 1.0 : ( UVLengthScale / UVLengthWeight );
            const int32   NumU = Strip.Outer.Num();
            TArray<int32> Row0, Row1;
            double        AccumDistU = 0;
            for ( int32 k = 0; k < NumU; ++k )
            {
                double DistV = Distance( Mesh.GetVertex( Strip.Outer[k] ), Mesh.GetVertex( Strip.Inner[k] ) );
                float  UseU  = (float)( UVLengthScale * AccumDistU * UVScaleFactor );
                float  EndV  = (float)( UVLengthScale * DistV * UVScaleFactor );
                Row0.Add( UVOverlay->AppendElement( FVector2f( UseU, 0.0f ) ) );
                Row1.Add( UVOverlay->AppendElement( FVector2f( UseU, EndV ) ) );
                if ( k < NumU - 1 )
                    AccumDistU += Distance( Mesh.GetVertex( Strip.Outer[k] ), Mesh.GetVertex( Strip.Outer[k + 1] ) );
            }
            for ( int32 q = 0; q < Strip.Quads.Num(); ++q )
                for ( int32 TriIdx = 0; TriIdx < 2; ++TriIdx )
                {
                    const int tid = TriIdx == 0 ? Strip.Quads[q].A : Strip.Quads[q].B;
                    FIndex3i  Tri = Mesh.GetTriangle( tid ), UVTri;
                    for ( int j = 0; j < 3; ++j )
                    {
                        const int v = Tri[j];
                        UVTri[j]    = v == Strip.Outer[q]       ? Row0[q]
                                      : v == Strip.Outer[q + 1] ? Row0[q + 1]
                                      : v == Strip.Inner[q]     ? Row1[q]
                                                                : Row1[q + 1];
                    }
                    UVOverlay->SetTriangle( tid, UVTri );
                }
        }
    } // namespace

    bool FOffsetMeshRegion::EdgesSeparateSameGroupsAndAreColinearAtBorder( FDynamicMesh3* Mesh, int32 Eid1,
                                                                            int32 Eid2, bool bCheckColinearityAtBorder )
    {
        if ( !Mesh->IsEdge( Eid1 ) || !Mesh->IsEdge( Eid2 ) )
            return false;
        const int Invalid = FDynamicMesh3::InvalidID;
        FIndex2i  Tris1   = Mesh->GetEdgeT( Eid1 );
        FIndex2i  Groups1( Mesh->GetTriangleGroup( Tris1.A ),
                           Tris1.B == Invalid ? Invalid : Mesh->GetTriangleGroup( Tris1.B ) );
        FIndex2i  Tris2 = Mesh->GetEdgeT( Eid2 );
        FIndex2i  Groups2( Mesh->GetTriangleGroup( Tris2.A ),
                           Tris2.B == Invalid ? Invalid : Mesh->GetTriangleGroup( Tris2.B ) );
        if ( bCheckColinearityAtBorder && Groups1.A == Groups2.A && Groups1.B == Invalid && Groups2.B == Invalid )
            return EdgesAreParallel( Mesh, Eid1, Eid2 );
        return ( Groups1.A == Groups2.A && Groups1.B == Groups2.B ) ||
               ( Groups1.A == Groups2.B && Groups1.B == Groups2.A );
    }

    bool FOffsetMeshRegion::Apply()
    {
        TArray<TArray<int32>> Components;
        FindConnectedTriangleComponents( *Mesh, Triangles, Components );
        bool bAllOK = true;
        OffsetRegions.SetNum( Components.Num() );
        for ( int k = 0; k < Components.Num(); ++k )
        {
            FOffsetInfo& Region = OffsetRegions[k];
            Region.OffsetTids   = Components[k];
            if ( bOffsetFullComponentsAsSolids )
            {
                // GrowToConnectedTriangles: the region is a whole component when no triangle outside it touches it.
                TSet<int32> InRegion( Region.OffsetTids );
                bool        bTouchesOutside = false;
                for ( int32 tid : Region.OffsetTids )
                {
                    const FIndex3i Nbrs = Mesh->GetTriNeighbourTris( tid );
                    for ( int j = 0; j < 3; ++j )
                        bTouchesOutside |= Nbrs[j] >= 0 && !InRegion.Contains( Nbrs[j] );
                }
                Region.bIsSolid = !bTouchesOutside;
            }
            if ( !ApplyOffset( Region ) )
            {
                FailureReason = fmt::format( "region {} of {}: {}", k, Components.Num(), FailureReason );
                bAllOK        = false;
            }
        }
        return bAllOK;
    }

    bool FOffsetMeshRegion::ApplyOffset( FOffsetInfo& Region )
    {
        const TArray<int32>& RegionTriangles = Region.OffsetTids;
        TMap<int32, int32>   OffsetGroupMap;
        if ( Mesh->HasTriangleGroups() )
            for ( int32 TriangleID : RegionTriangles )
            {
                int32 CurGroupID = Mesh->GetTriangleGroup( TriangleID );
                int32 NewGroupID;
                if ( const int32* Found = OffsetGroupMap.Find( CurGroupID ) )
                    NewGroupID = *Found;
                else
                {
                    NewGroupID = Mesh->AllocateTriangleGroup();
                    OffsetGroupMap.Add( CurGroupID, NewGroupID );
                    Region.OffsetGroups.Add( NewGroupID );
                }
                Mesh->SetTriangleGroup( TriangleID, NewGroupID );
            }

        FMeshRegionBoundaryLoops InitialLoops( Mesh, RegionTriangles, false );
        if ( !InitialLoops.Compute() )
        {
            FailureReason = InitialLoops.FailureReason;
            return false;
        }
        AllModifiedAndNewTriangles.Append( RegionTriangles );
        TSet<int32> TriangleSet( RegionTriangles );

        TArray<TArray<int32>> LoopsEdgeGroups;
        TArray<int32>         NewGroupIDs;
        LoopsEdgeGroups.SetNum( InitialLoops.Loops.Num() );
        for ( int32 i = 0; i < InitialLoops.Loops.Num(); ++i )
        {
            TArray<int32>& CurrentEdgeGroups = LoopsEdgeGroups[i];
            ComputeNewGroupIDsAlongEdgeLoop( *Mesh, InitialLoops.Loops[i].Edges, CurrentEdgeGroups, NewGroupIDs,
                                             LoopEdgesShouldHaveSameGroup );
            int GroupShift = FindLoopShiftFromGroupIDs( CurrentEdgeGroups );
            LeftShiftArray( InitialLoops.Loops[i].Vertices, GroupShift );
            LeftShiftArray( InitialLoops.Loops[i].Edges, GroupShift );
            LeftShiftArray( CurrentEdgeGroups, GroupShift );
        }

        // The material of the region triangle on each loop edge, read before the loops are cut and stitched.
        FDynamicMeshMaterialAttribute* MaterialIDAttrib =
             ( Mesh->HasAttributes() && Mesh->Attributes()->HasMaterialID() ) ? Mesh->Attributes()->GetMaterialID()
                                                                              : nullptr;

        FDynamicMeshEditor                       Editor( Mesh );
        TArray<FDynamicMeshEditor::FLoopPairSet> LoopPairs;
        FDynamicMeshEditResult                   DuplicateResult;
        if ( Region.bIsSolid )
        {
            TMap<int, int> IndexMap;
            Editor.DuplicateTriangles( RegionTriangles, IndexMap, DuplicateResult );
            AllModifiedAndNewTriangles.Append( DuplicateResult.NewTriangles );
            LoopPairs.SetNum( InitialLoops.Loops.Num() );
            for ( int LoopIndex = 0; LoopIndex < InitialLoops.Loops.Num(); ++LoopIndex )
            {
                FEdgeLoop&                        BaseLoop = InitialLoops.Loops[LoopIndex];
                FDynamicMeshEditor::FLoopPairSet& LoopPair = LoopPairs[LoopIndex];
                LoopPair.InnerVertices                     = BaseLoop.Vertices;
                LoopPair.InnerEdges                        = BaseLoop.Edges;
                if ( !bIsPositiveOffset )
                {
                    std::reverse( LoopPair.InnerVertices.begin(), LoopPair.InnerVertices.end() );
                    int32 LastEid = LoopPair.InnerEdges.Pop();
                    std::reverse( LoopPair.InnerEdges.begin(), LoopPair.InnerEdges.end() );
                    LoopPair.InnerEdges.Add( LastEid );
                    int32 LastEdgeGroupID = LoopsEdgeGroups[LoopIndex].Pop();
                    std::reverse( LoopsEdgeGroups[LoopIndex].begin(), LoopsEdgeGroups[LoopIndex].end() );
                    LoopsEdgeGroups[LoopIndex].Add( LastEdgeGroupID );
                }
                for ( int32 Vid : LoopPair.InnerVertices )
                    LoopPair.OuterVertices.Add( IndexMap[Vid] );
                VertexLoopToEdgeLoop( *Mesh, LoopPair.OuterVertices, LoopPair.OuterEdges );
            }
        }
        else if ( !Editor.DisconnectTriangles( TriangleSet, InitialLoops.Loops, LoopPairs, true, FailureReason ) )
            return false;

        // Materials along each (inner) loop, from the region triangle on the edge.
        TArray<TArray<int32>> LoopMaterialIDs;
        LoopMaterialIDs.SetNum( LoopPairs.Num() );
        if ( MaterialIDAttrib )
            for ( int32 i = 0; i < LoopPairs.Num(); ++i )
                for ( int32 e : LoopPairs[i].InnerEdges )
                    LoopMaterialIDs[i].Add( bInferMaterialID ? MaterialIDAttrib->GetValue( Mesh->GetEdgeT( e ).A )
                                                             : SetMaterialID );

        // FMeshVertexSelection::SelectTriangleVertices, in ascending order.
        TSet<int32> SelectedSet;
        for ( int32 tid : RegionTriangles )
        {
            const FIndex3i Tri = Mesh->GetTriangle( tid );
            for ( int j = 0; j < 3; ++j )
                SelectedSet.Add( Tri[j] );
        }
        TArray<int32> SelectedVids;
        for ( int32 v : SelectedSet )
            SelectedVids.Add( v );
        std::sort( SelectedVids.begin(), SelectedVids.end() );

        TArray<FVector3d> VertexExtrudeVectors;
        VertexExtrudeVectors.SetNum( SelectedVids.Num() );
        for ( int32 i = 0; i < SelectedVids.Num(); ++i )
        {
            switch ( ExtrusionVectorType )
            {
                case EVertexExtrusionVectorType::Zero:
                    VertexExtrudeVectors[i] = FVector3d( 0, 0, 0 );
                    break;
                case EVertexExtrusionVectorType::VertexNormal:
                    VertexExtrudeVectors[i] = FMeshNormals::ComputeVertexNormal( *Mesh, SelectedVids[i] );
                    break;
                case EVertexExtrusionVectorType::SelectionTriNormalsAngleWeightedAverage:
                    VertexExtrudeVectors[i] = GetAngleWeightedAverageNormal( *Mesh, SelectedVids[i], TriangleSet );
                    break;
                case EVertexExtrusionVectorType::SelectionTriNormalsAngleWeightedAdjusted:
                    VertexExtrudeVectors[i] = GetAngleWeightedAdjustedNormal( *Mesh, SelectedVids[i], TriangleSet,
                                                                              MaxScaleForAdjustingTriNormalsOffset );
                    break;
            }
        }
        for ( int32 i = 0; i < SelectedVids.Num(); ++i )
        {
            const int32 VertexID = SelectedVids[i];
            Mesh->SetVertex( VertexID,
                             OffsetPositionFunc( Mesh->GetVertex( VertexID ), VertexExtrudeVectors[i], VertexID ) );
        }

        // StitchRegionBorderLoopPairs_Version1 with NumSubdivisions = 0.
        bool bSuccess = true;
        Region.BaseLoops.SetNum( LoopPairs.Num() );
        Region.OffsetLoops.SetNum( LoopPairs.Num() );
        Region.StitchTriangles.SetNum( LoopPairs.Num() );
        Region.StitchPolygonIDs.SetNum( LoopPairs.Num() );
        for ( int32 LoopIndex = 0; LoopIndex < LoopPairs.Num(); ++LoopIndex )
        {
            const TArray<int32>&   OuterLoopV = LoopPairs[LoopIndex].OuterVertices;
            const TArray<int32>&   InnerLoopV = LoopPairs[LoopIndex].InnerVertices;
            const int32            NV         = OuterLoopV.Num();
            FDynamicMeshEditResult StitchResult;
            if ( !Editor.StitchVertexLoopsMinimal( InnerLoopV, OuterLoopV, StitchResult ) )
            {
                FailureReason = fmt::format( "loop {} ({} vertices) could not be stitched to its copy", LoopIndex, NV );
                bSuccess      = false;
                continue;
            }
            const TArray<int32>& PerEdgeNewGroupIDs = LoopsEdgeGroups[LoopIndex];
            for ( int32 q = 0; q < NV; ++q )
            {
                const FIndex2i QuadTris = StitchResult.NewQuads[q];
                Mesh->SetTriangleGroup( QuadTris.A, PerEdgeNewGroupIDs[q] );
                Mesh->SetTriangleGroup( QuadTris.B, PerEdgeNewGroupIDs[q] );
                if ( MaterialIDAttrib )
                {
                    MaterialIDAttrib->SetValue( QuadTris.A, LoopMaterialIDs[LoopIndex][q] );
                    MaterialIDAttrib->SetValue( QuadTris.B, LoopMaterialIDs[LoopIndex][q] );
                }
            }
            // Split the columns into strips of equal group (SplitColumnsByPredicate); the loop was shifted so
            // column 0 starts a group.
            TArray<FStrip> GroupStrips;
            for ( int32 q = 0; q < NV; ++q )
            {
                if ( q == 0 || PerEdgeNewGroupIDs[q] != PerEdgeNewGroupIDs[q - 1] )
                {
                    GroupStrips.Emplace();
                    GroupStrips.Last().Outer.Add( OuterLoopV[q] );
                    GroupStrips.Last().Inner.Add( InnerLoopV[q] );
                }
                GroupStrips.Last().Outer.Add( OuterLoopV[( q + 1 ) % NV] );
                GroupStrips.Last().Inner.Add( InnerLoopV[( q + 1 ) % NV] );
                GroupStrips.Last().Quads.Add( StitchResult.NewQuads[q] );
            }
            if ( Mesh->HasAttributes() )
            {
                for ( const FStrip& Strip : GroupStrips )
                {
                    TArray<int32> PatchTriangles;
                    for ( const FIndex2i& Q : Strip.Quads )
                    {
                        PatchTriangles.Add( Q.A );
                        PatchTriangles.Add( Q.B );
                    }
                    Editor.SetTriangleNormals( PatchTriangles );
                    if ( bUVIslandPerGroup )
                        ComputeUVIslandForStrip( *Mesh, Strip, UVScaleFactor );
                }
                if ( !bUVIslandPerGroup )
                {
                    FStrip Whole;
                    for ( int32 q = 0; q <= NV; ++q )
                    {
                        Whole.Outer.Add( OuterLoopV[q % NV] );
                        Whole.Inner.Add( InnerLoopV[q % NV] );
                    }
                    Whole.Quads = StitchResult.NewQuads;
                    ComputeUVIslandForStrip( *Mesh, Whole, UVScaleFactor );
                }
            }
            for ( const FIndex2i& Q : StitchResult.NewQuads )
            {
                Region.StitchTriangles[LoopIndex].Add( Q.A );
                Region.StitchTriangles[LoopIndex].Add( Q.B );
                Region.StitchPolygonIDs[LoopIndex].AddUnique( Mesh->GetTriangleGroup( Q.A ) );
            }
            Region.BaseLoops[LoopIndex].Vertices   = OuterLoopV;
            Region.OffsetLoops[LoopIndex].Vertices = InnerLoopV;
            VertexLoopToEdgeLoop( *Mesh, OuterLoopV, Region.BaseLoops[LoopIndex].Edges );
            VertexLoopToEdgeLoop( *Mesh, InnerLoopV, Region.OffsetLoops[LoopIndex].Edges );
        }

        if ( Region.bIsSolid )
        {
            if ( bIsPositiveOffset )
                Editor.ReverseTriangleOrientations( DuplicateResult.NewTriangles, true );
            else
                Editor.ReverseTriangleOrientations( RegionTriangles, true );
        }
        if ( bSingleGroupPerArea && Mesh->HasTriangleGroups() && Region.OffsetGroups.Num() > 1 )
        {
            for ( int32 TriangleID : RegionTriangles )
                Mesh->SetTriangleGroup( TriangleID, Region.OffsetGroups[0] );
            Region.OffsetGroups.SetNum( 1 );
        }
        return bSuccess;
    }
} // namespace Desert::Geometry
