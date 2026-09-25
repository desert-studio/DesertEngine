// Ported from UE 5.8 .../DynamicMesh/Private/Operations/InsetMeshRegion.cpp and PolyEditingEdgeUtil.cpp (see the
// header for the line ranges and the adaptations).
#include "Engine/Geometry/UECore/DynamicMesh/Operations/InsetMeshRegion.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMeshEditor.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/OffsetMeshRegion.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace Desert::Geometry
{
    namespace
    {
        struct FLine3d
        {
            FVector3d Origin{ 0, 0, 0 };
            FVector3d Direction{ 0, 0, 1 };
            FVector3d NearestPoint( const FVector3d& P ) const
            {
                return Origin + Direction * ( P - Origin ).Dot( Direction );
            }
        };

        // ComputeInsetLineSegmentsFromEdges (PolyEditingEdgeUtil.cpp:11).
        void ComputeInsetLineSegmentsFromEdges( const FDynamicMesh3& Mesh, const TArray<int32>& EdgeList,
                                                double InsetDistance, TArray<FLine3d>& InsetLinesOut )
        {
            InsetLinesOut.SetNum( EdgeList.Num() );
            for ( int32 k = 0; k < EdgeList.Num(); ++k )
            {
                if ( !Mesh.IsEdge( EdgeList[k] ) )
                {
                    InsetLinesOut[k] = FLine3d();
                    continue;
                }
                const FDynamicMesh3::FEdge EdgeVT   = Mesh.GetEdge( EdgeList[k] );
                FVector3d                  A        = Mesh.GetVertex( EdgeVT.Vert.A );
                FVector3d                  B        = Mesh.GetVertex( EdgeVT.Vert.B );
                FVector3d                  EdgeDir  = Normalized( A - B );
                FVector3d                  Midpoint = ( A + B ) * 0.5;
                FVector3d                  Normal, Centroid;
                double                     Area;
                Mesh.GetTriInfo( EdgeVT.Tri.A, Normal, Area, Centroid );
                FVector3d InsetDir = Normal.Cross( EdgeDir );
                if ( ( Centroid - Midpoint ).Dot( InsetDir ) < 0 )
                    InsetDir = InsetDir * -1.0;
                InsetLinesOut[k] = FLine3d{ Midpoint + InsetDir * InsetDistance, EdgeDir };
            }
        }

        // SolveInsetVertexPositionFromLinePair (PolyEditingEdgeUtil.cpp:51); FDistLine3Line3d's closest points.
        FVector3d SolveInsetVertexPositionFromLinePair( const FVector3d& Position, const FLine3d& L1,
                                                        const FLine3d& L2 )
        {
            const double B = L1.Direction.Dot( L2.Direction );
            if ( std::abs( B ) > 0.999 )
                return L1.NearestPoint( Position );
            const FVector3d Diff = L1.Origin - L2.Origin;
            const double    D1   = Diff.Dot( L1.Direction );
            const double    D2   = Diff.Dot( L2.Direction );
            const double    Det  = 1.0 - B * B;
            const double    S1   = ( B * D2 - D1 ) / Det;
            const double    S2   = ( D2 - B * D1 ) / Det;
            const FVector3d P1   = L1.Origin + L1.Direction * S1;
            const FVector3d P2   = L2.Origin + L2.Direction * S2;
            return ( P1 + P2 ) * 0.5;
        }
    } // namespace

    bool FInsetMeshRegion::Apply()
    {
        TArray<TArray<int32>> Components;
        FindConnectedTriangleComponents( *Mesh, Triangles, Components );
        bool bAllOK = true;
        InsetRegions.SetNum( Components.Num() );
        for ( int32 k = 0; k < Components.Num(); ++k )
        {
            FInsetInfo& Region      = InsetRegions[k];
            Region.InitialTriangles = Components[k];
            if ( !ApplyInset( Region ) )
            {
                FailureReason = fmt::format( "region {} of {}: {}", k, Components.Num(), FailureReason );
                bAllOK        = false;
                continue;
            }
            AllModifiedTriangles.Append( Region.InitialTriangles );
            for ( const TArray<int32>& RegionTris : Region.StitchTriangles )
                AllModifiedTriangles.Append( RegionTris );
        }
        return bAllOK;
    }

    bool FInsetMeshRegion::ApplyInset( FInsetInfo& Region )
    {
        // UE solves interior vertices with a Laplacian deformer; that solver is not ported, so such a region is
        // refused here, before the mesh is touched.
        {
            FMeshRegionBoundaryLoops Loops( Mesh, Region.InitialTriangles, false );
            if ( !Loops.Compute() )
            {
                FailureReason = Loops.FailureReason;
                return false;
            }
            TSet<int32> LoopVertices;
            for ( const FEdgeLoop& Loop : Loops.Loops )
                for ( int32 v : Loop.Vertices )
                    LoopVertices.Add( v );
            for ( int32 tid : Region.InitialTriangles )
            {
                const FIndex3i Tri = Mesh->GetTriangle( tid );
                for ( int j = 0; j < 3; ++j )
                    if ( !LoopVertices.Contains( Tri[j] ) )
                    {
                        FailureReason =
                             fmt::format( "vertex {} is inside the region ({} triangles): an inset of a "
                                          "region with interior vertices needs UE's interior solve, "
                                          "which is not ported",
                                          Tri[j], Region.InitialTriangles.Num() );
                        return false;
                    }
            }
        }

        FDynamicMeshEditor                       Editor( Mesh );
        TArray<FDynamicMeshEditor::FLoopPairSet> LoopPairs;
        if ( !Editor.DisconnectTriangles( Region.InitialTriangles, LoopPairs, true, FailureReason ) )
            return false;

        TArray<TArray<FTriVidPair>> InsetStitchSides;
        InsetStitchSides.SetNum( LoopPairs.Num() );
        for ( int32 i = 0; i < LoopPairs.Num(); ++i )
            FDynamicMeshEditor::ConvertLoopToTriVidPairSequence( *Mesh, LoopPairs[i].InnerVertices,
                                                                 LoopPairs[i].InnerEdges, InsetStitchSides[i] );

        Region.InsetLoops.Reset();
        for ( const FDynamicMeshEditor::FLoopPairSet& LoopPair : LoopPairs )
        {
            const TArray<int32>& LoopVids = LoopPair.InnerVertices;
            TArray<FLine3d>      InsetLines;
            ComputeInsetLineSegmentsFromEdges( *Mesh, LoopPair.InnerEdges, InsetDistance, InsetLines );
            // SolveInsetVertexPositionsFromInsetLines, bIsLoop = true.
            TArray<FVector3d> NewPositions;
            const int32       N = LoopVids.Num();
            NewPositions.SetNum( N );
            for ( int32 vi = 0; vi < N; ++vi )
            {
                const FLine3d& PrevLine = ( vi == 0 ) ? InsetLines.Last() : InsetLines[vi - 1];
                NewPositions[vi] = SolveInsetVertexPositionFromLinePair( Mesh->GetVertex( LoopVids[vi] ), PrevLine,
                                                                         InsetLines[vi] );
            }
            for ( int32 k = 0; k < N; ++k )
                Mesh->SetVertex( LoopVids[k], NewPositions[k] );
            Region.InsetLoops.Emplace();
            Region.InsetLoops.Last().Vertices = LoopVids;
            Region.InsetLoops.Last().Edges    = LoopPair.InnerEdges;
        }

        const int32 NumInitialLoops = LoopPairs.Num();
        Region.BaseLoops.SetNum( NumInitialLoops );
        Region.StitchTriangles.SetNum( NumInitialLoops );
        Region.StitchPolygonIDs.SetNum( NumInitialLoops );
        TArray<TArray<FIndex2i>> QuadStrips;
        for ( int32 LoopIndex = 0; LoopIndex < NumInitialLoops; ++LoopIndex )
        {
            const FDynamicMeshEditor::FLoopPairSet& LoopPair  = LoopPairs[LoopIndex];
            const TArray<int32>&                    BaseLoopV = LoopPair.OuterVertices;
            const int32                             NumLoopV  = BaseLoopV.Num();
            TArray<int32>                           NewGroupIDs, EdgeGroups;
            TMap<int64_t, int32>                    NewGroupsMap; // (min, max) group pair packed
            for ( int32 k = 0; k < NumLoopV; ++k )
            {
                int32 InsetGroupID = Mesh->GetTriangleGroup( InsetStitchSides[LoopIndex][k].first );
                int32 BaseEdgeID   = Mesh->FindEdge( BaseLoopV[k], BaseLoopV[( k + 1 ) % NumLoopV] );
                int32 BaseGroupID =
                     ( BaseEdgeID >= 0 ) ? Mesh->GetTriangleGroup( Mesh->GetEdgeT( BaseEdgeID ).A ) : InsetGroupID;
                const int64_t GroupPair = ( int64_t( std::min( BaseGroupID, InsetGroupID ) ) << 32 ) |
                                          uint32_t( std::max( BaseGroupID, InsetGroupID ) );
                if ( !NewGroupsMap.Contains( GroupPair ) )
                {
                    int32 NewGroupID = Mesh->AllocateTriangleGroup();
                    NewGroupIDs.Add( NewGroupID );
                    NewGroupsMap.Add( GroupPair, NewGroupID );
                }
                EdgeGroups.Add( NewGroupsMap[GroupPair] );
            }
            FDynamicMeshEditResult StitchResult;
            if ( !Editor.StitchVertexLoopToTriVidPairSequence( InsetStitchSides[LoopIndex], BaseLoopV,
                                                               StitchResult ) )
            {
                FailureReason = fmt::format( "loop {} ({} vertices) could not be stitched", LoopIndex, NumLoopV );
                return false;
            }
            for ( int32 k = 0; k < StitchResult.NewQuads.Num(); k++ )
            {
                Mesh->SetTriangleGroup( StitchResult.NewQuads[k].A, EdgeGroups[k] );
                Mesh->SetTriangleGroup( StitchResult.NewQuads[k].B, EdgeGroups[k] );
            }
            StitchResult.GetAllTriangles( Region.StitchTriangles[LoopIndex] );
            Region.StitchPolygonIDs[LoopIndex] = NewGroupIDs;
            QuadStrips.Add( StitchResult.NewQuads );
            Region.BaseLoops[LoopIndex].Vertices = BaseLoopV;
            VertexLoopToEdgeLoop( *Mesh, BaseLoopV, Region.BaseLoops[LoopIndex].Edges );
        }

        if ( Mesh->HasAttributes() )
            for ( int32 StripIndex = 0; StripIndex < QuadStrips.Num(); ++StripIndex )
            {
                const TArray<int32>& BaseLoopV          = LoopPairs[StripIndex].OuterVertices;
                float                AccumUVTranslation = 0;
                FVector3d            FirstAxisX, FrameUp;
                for ( int32 k = 0; k < QuadStrips[StripIndex].Num(); k++ )
                {
                    const FVector3f NF = Editor.ComputeAndSetQuadNormal( QuadStrips[StripIndex][k], true );
                    const FVector3d Normal( NF.X, NF.Y, NF.Z );
                    FVector3d       AxisX, AxisY;
                    if ( k == 0 )
                    {
                        // FFrame3d(0, Normal).ConstrainedAlignAxis(0, FirstEdge, Normal): X is the first edge
                        // in the quad's plane, Y = Z x X.
                        FVector3d FirstEdge = Mesh->GetVertex( BaseLoopV[1] ) - Mesh->GetVertex( BaseLoopV[0] );
                        AxisX               = Normalized( FirstEdge - Normal * FirstEdge.Dot( Normal ) );
                        AxisY               = Normal.Cross( AxisX );
                        FrameUp             = AxisY;
                    }
                    else
                    {
                        // ConstrainedAlignAxis(2, Normal, FrameUp): rotate about FrameUp until Z meets Normal.
                        FVector3d Z = Normalized( Normal - FrameUp * Normal.Dot( FrameUp ) );
                        AxisY       = FrameUp;
                        AxisX       = AxisY.Cross( Z );
                    }
                    if ( k > 0 )
                        AccumUVTranslation += (float)Distance( Mesh->GetVertex( BaseLoopV[k] ),
                                                               Mesh->GetVertex( BaseLoopV[k - 1] ) );
                    Editor.SetQuadUVsFromProjection( QuadStrips[StripIndex][k], AxisX, AxisY, UVScaleFactor,
                                                     FVector2f( UVScaleFactor * AccumUVTranslation, 0.0f ) );
                }
            }
        return true;
    }
} // namespace Desert::Geometry
