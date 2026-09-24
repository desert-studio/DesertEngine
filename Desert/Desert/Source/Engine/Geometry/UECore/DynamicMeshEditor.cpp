// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMeshEditor.cpp (see the header for the
// line ranges and the adaptations).
#include "Engine/Geometry/UECore/DynamicMeshEditor.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/UECore/IndexUtil.hpp"

#include <fmt/format.h>

namespace Desert::Geometry
{
    namespace
    {
        template <typename FuncType>
        bool StitchLoopsInternal( FDynamicMeshEditor& Editor, int32 NumQuads, FuncType&& GetQuadVidsForIndex,
                                  FDynamicMeshEditResult& ResultOut )
        {
            ResultOut.NewQuads.Reserve( NumQuads );
            ResultOut.NewGroups.Reserve( NumQuads );
            bool bFailed = false;
            for ( int i = 0; i < NumQuads; ++i )
            {
                int32 a, b, c, d;
                GetQuadVidsForIndex( i, a, b, c, d );
                int NewGroupID = Editor.Mesh->AllocateTriangleGroup();
                ResultOut.NewGroups.Add( NewGroupID );
                int tid1 = Editor.Mesh->AppendTriangle( FIndex3i( b, a, d ), NewGroupID );
                int tid2 = Editor.Mesh->AppendTriangle( FIndex3i( a, c, d ), NewGroupID );
                ResultOut.NewQuads.Add( FIndex2i( tid1, tid2 ) );
                if ( tid1 < 0 || tid2 < 0 )
                {
                    bFailed = true;
                    break;
                }
            }
            if ( !bFailed )
                return true;
            // Back out every triangle this stitch added (UE's operation_failed label).
            TArray<int> Triangles;
            for ( const FIndex2i& Quad : ResultOut.NewQuads )
            {
                if ( Quad.A >= 0 )
                    Triangles.Add( Quad.A );
                if ( Quad.B >= 0 )
                    Triangles.Add( Quad.B );
            }
            Editor.RemoveTriangles( Triangles, false );
            return false;
        }
    } // namespace

    void VertexLoopToEdgeLoop( const FDynamicMesh3& Mesh, const TArray<int>& VertexLoop, TArray<int>& EdgeLoopOut )
    {
        const int N = VertexLoop.Num();
        EdgeLoopOut.SetNum( N );
        for ( int i = 0; i < N; ++i )
            EdgeLoopOut[i] = Mesh.FindEdge( VertexLoop[i], VertexLoop[( i + 1 ) % N] );
    }

    bool FDynamicMeshEditor::StitchVertexLoopsMinimal( const TArray<int>& Loop1, const TArray<int>& Loop2,
                                                       FDynamicMeshEditResult& ResultOut )
    {
        int N = Loop1.Num();
        if ( !UE_ENSURE( N == Loop2.Num() ) )
            return false;
        return StitchLoopsInternal(
             *this, N,
             [N, &Loop1, &Loop2]( int32 Index, int32& VertA, int32& VertB, int32& VertC, int32& VertD )
             {
                 VertA = Loop1[Index];
                 VertB = Loop1[( Index + 1 ) % N];
                 VertC = Loop2[Index];
                 VertD = Loop2[( Index + 1 ) % N];
             },
             ResultOut );
    }

    bool FDynamicMeshEditor::StitchVertexLoopToTriVidPairSequence( const TArray<FTriVidPair>& TriVidPairs,
                                                                   const TArray<int>&         VertexLoop,
                                                                   FDynamicMeshEditResult&    ResultOut )
    {
        int N = TriVidPairs.Num();
        if ( !UE_ENSURE( N == VertexLoop.Num() ) )
            return false;
        return StitchLoopsInternal(
             *this, N,
             [this, N, &TriVidPairs, &VertexLoop]( int32 Index, int32& VertA, int32& VertB, int32& VertC,
                                                    int32& VertD )
             {
                 FIndex3i TriVids1 = Mesh->GetTriangle( TriVidPairs[Index].first );
                 VertA             = TriVids1[TriVidPairs[Index].second.first];
                 VertB             = TriVids1[TriVidPairs[Index].second.second];
                 VertC             = VertexLoop[Index];
                 VertD             = VertexLoop[( Index + 1 ) % N];
             },
             ResultOut );
    }

    bool FDynamicMeshEditor::ConvertLoopToTriVidPairSequence( const FDynamicMesh3& Mesh, const TArray<int>& VidLoop,
                                                              const TArray<int>&   EdgeLoop,
                                                              TArray<FTriVidPair>& TriVertPairsOut )
    {
        if ( !UE_ENSURE( EdgeLoop.Num() == VidLoop.Num() ) )
            return false;
        for ( int32 QuadIndex = 0; QuadIndex < EdgeLoop.Num(); ++QuadIndex )
        {
            int32    Tid       = Mesh.GetEdgeT( EdgeLoop[QuadIndex] ).A;
            int32    FirstVid  = VidLoop[QuadIndex];
            int32    SecondVid = VidLoop[( QuadIndex + 1 ) % VidLoop.Num()];
            FIndex3i TriVids   = Mesh.GetTriangle( Tid );
            int8     SubIdx1   = (int8)IndexUtil::FindTriIndex( FirstVid, TriVids );
            int8     SubIdx2   = (int8)IndexUtil::FindTriIndex( SecondVid, TriVids );
            if ( !( SubIdx1 >= 0 && SubIdx2 >= 0 ) )
                return false;
            TriVertPairsOut.Add( FTriVidPair( Tid, std::pair<int8, int8>( SubIdx1, SubIdx2 ) ) );
        }
        return true;
    }

    bool FDynamicMeshEditor::RemoveTriangles( const TArray<int>& Triangles, bool bRemoveIsolatedVerts )
    {
        bool bAllOK = true;
        for ( int tid : Triangles )
        {
            if ( !Mesh->IsTriangle( tid ) )
                continue;
            if ( Mesh->RemoveTriangle( tid, bRemoveIsolatedVerts, false ) != EMeshResult::Ok )
                bAllOK = false;
        }
        return bAllOK;
    }

    void FDynamicMeshEditor::DuplicateTriangles( const TArray<int>& Triangles, TMap<int, int>& OldToNewVertex,
                                                 FDynamicMeshEditResult& ResultOut )
    {
        ResultOut.Reset();
        TMap<int, int>                 GroupMap;
        FDynamicMeshAttributeSet*      Attr = Mesh->HasAttributes() ? Mesh->Attributes() : nullptr;
        TArray<TMap<int, int>>         UVMaps, NormalMaps;
        if ( Attr )
        {
            UVMaps.SetNum( Attr->NumUVLayers() );
            NormalMaps.SetNum( Attr->NumNormalLayers() );
        }
        for ( int TriangleID : Triangles )
        {
            FIndex3i Tri        = Mesh->GetTriangle( TriangleID );
            int      NewGroupID = -1;
            if ( Mesh->HasTriangleGroups() )
            {
                const int OldGroup = Mesh->GetTriangleGroup( TriangleID );
                if ( const int* Found = GroupMap.Find( OldGroup ) )
                    NewGroupID = *Found;
                else
                {
                    NewGroupID = Mesh->AllocateTriangleGroup();
                    GroupMap.Add( OldGroup, NewGroupID );
                    ResultOut.NewGroups.Add( NewGroupID );
                }
            }
            FIndex3i NewTri;
            for ( int j = 0; j < 3; ++j )
            {
                if ( const int* Found = OldToNewVertex.Find( Tri[j] ) )
                    NewTri[j] = *Found;
                else
                {
                    NewTri[j] = Mesh->AppendVertex( *Mesh, Tri[j] );
                    OldToNewVertex.Add( Tri[j], NewTri[j] );
                    ResultOut.NewVertices.Add( NewTri[j] );
                }
            }
            int NewTriangleID = Mesh->AppendTriangle( NewTri, NewGroupID );
            ResultOut.NewTriangles.Add( NewTriangleID );
            if ( !Attr || NewTriangleID < 0 )
                continue;
            // CopyAttributes: every overlay element is duplicated once per source element.
            auto CopyOverlay = [&]( auto* Overlay, TMap<int, int>& Map )
            {
                if ( !Overlay || !Overlay->IsSetTriangle( TriangleID ) )
                    return;
                FIndex3i Elems = Overlay->GetTriangle( TriangleID ), NewElems;
                for ( int j = 0; j < 3; ++j )
                {
                    if ( const int* Found = Map.Find( Elems[j] ) )
                        NewElems[j] = *Found;
                    else
                    {
                        NewElems[j] = Overlay->AppendElement( Overlay->GetElement( Elems[j] ) );
                        Map.Add( Elems[j], NewElems[j] );
                    }
                }
                Overlay->SetTriangle( NewTriangleID, NewElems );
            };
            for ( int k = 0; k < Attr->NumUVLayers(); ++k )
                CopyOverlay( Attr->GetUVLayer( k ), UVMaps[k] );
            for ( int k = 0; k < Attr->NumNormalLayers(); ++k )
                CopyOverlay( Attr->GetNormalLayer( k ), NormalMaps[k] );
            if ( Attr->HasMaterialID() )
                Attr->GetMaterialID()->SetValue( NewTriangleID, Attr->GetMaterialID()->GetValue( TriangleID ) );
        }
    }

    bool FDynamicMeshEditor::DisconnectTriangles( const TArray<int>& Triangles, TArray<FLoopPairSet>& LoopSetOut,
                                                  bool bHandleBoundaryVertices, std::string& FailureOut )
    {
        FMeshRegionBoundaryLoops RegionLoops( Mesh, Triangles, false );
        if ( !RegionLoops.Compute() )
        {
            FailureOut = RegionLoops.FailureReason;
            return false;
        }
        TSet<int> TriangleSet( Triangles );
        return DisconnectTriangles( TriangleSet, RegionLoops.Loops, LoopSetOut, bHandleBoundaryVertices,
                                    FailureOut );
    }

    bool FDynamicMeshEditor::DisconnectTriangles( const TSet<int>& TriangleSet, const TArray<FEdgeLoop>& Loops,
                                                  TArray<FLoopPairSet>& LoopSetOut, bool bHandleBoundaryVertices,
                                                  std::string& FailureOut )
    {
        int NumLoops = Loops.Num();
        LoopSetOut.SetNum( NumLoops );
        TArray<int>    FilteredTriangles;
        TMap<int, int> OldVidsToNewVids;
        for ( int li = 0; li < NumLoops; ++li )
        {
            const FEdgeLoop& Loop        = Loops[li];
            FLoopPairSet&    LoopPair    = LoopSetOut[li];
            LoopPair.OuterVertices       = Loop.Vertices;
            LoopPair.OuterEdges          = Loop.Edges;
            bool        bSawBoundaryInLoop = false;
            int         NumVertices        = Loop.Vertices.Num();
            TArray<int> NewVertexLoop;
            NewVertexLoop.SetNum( NumVertices );
            for ( int vi = 0; vi < NumVertices; ++vi )
            {
                int VertID = Loop.Vertices[vi];
                // Already split as part of a bowtie: only the loop pairs are updated.
                if ( const int* ExistingNewVertID = OldVidsToNewVids.Find( VertID ) )
                {
                    if ( !Mesh->IsReferencedVertex( *ExistingNewVertID ) )
                    {
                        LoopPair.OuterVertices[vi]                                   = *ExistingNewVertID;
                        LoopPair.OuterEdges[vi]                                      = FDynamicMesh3::InvalidID;
                        LoopPair.OuterEdges[( vi == 0 ) ? NumVertices - 1 : vi - 1] = FDynamicMesh3::InvalidID;
                        NewVertexLoop[vi]                                            = VertID;
                    }
                    else
                        NewVertexLoop[vi] = *ExistingNewVertID;
                    continue;
                }
                FilteredTriangles.Reset();
                int TriRingCount = 0;
                for ( int RingTID : Mesh->VtxTrianglesItr( VertID ) )
                {
                    if ( TriangleSet.Contains( RingTID ) )
                        FilteredTriangles.Add( RingTID );
                    TriRingCount++;
                }
                if ( FilteredTriangles.Num() < TriRingCount )
                {
                    DynamicMeshInfo::FVertexSplitInfo SplitInfo;
                    const EMeshResult MeshResult = Mesh->SplitVertex( VertID, FilteredTriangles, SplitInfo );
                    if ( MeshResult != EMeshResult::Ok )
                    {
                        FailureOut = fmt::format( "splitting boundary vertex {} of loop {} failed", VertID, li );
                        return false;
                    }
                    OldVidsToNewVids.Add( VertID, SplitInfo.NewVertex );
                    NewVertexLoop[vi] = SplitInfo.NewVertex;
                }
                else if ( bHandleBoundaryVertices )
                {
                    // A mesh-border vertex: the duplicate becomes the "old" one, the original stays inner.
                    int32 NewVertID = Mesh->AppendVertex( *Mesh, VertID );
                    OldVidsToNewVids.Add( VertID, NewVertID );
                    LoopPair.OuterVertices[vi]                                   = NewVertID;
                    LoopPair.OuterEdges[vi]                                      = FDynamicMesh3::InvalidID;
                    LoopPair.OuterEdges[( vi == 0 ) ? NumVertices - 1 : vi - 1] = FDynamicMesh3::InvalidID;
                    NewVertexLoop[vi]                                            = VertID;
                    bSawBoundaryInLoop                                           = true;
                }
                else
                {
                    FailureOut = fmt::format( "vertex {} of loop {} lies on the mesh border", VertID, li );
                    return false;
                }
            }
            LoopPair.InnerVertices = NewVertexLoop;
            VertexLoopToEdgeLoop( *Mesh, NewVertexLoop, LoopPair.InnerEdges );
            for ( int e : LoopPair.InnerEdges )
                if ( e == FDynamicMesh3::InvalidID )
                {
                    FailureOut = fmt::format( "the cut-loose copy of loop {} is not an edge loop", li );
                    return false;
                }
            LoopPair.bOuterIncludesIsolatedVertices = bSawBoundaryInLoop;
        }
        return true;
    }

    FVector3f FDynamicMeshEditor::ComputeAndSetQuadNormal( const FIndex2i& QuadTris, bool bIsPlanar )
    {
        FVector3d Normal = Mesh->GetTriNormal( QuadTris.A );
        if ( !bIsPlanar )
        {
            Normal = Normal + Mesh->GetTriNormal( QuadTris.B );
            Normalize( Normal );
        }
        FVector3f NormalF( (float)Normal.X, (float)Normal.Y, (float)Normal.Z );
        SetQuadNormals( QuadTris, NormalF );
        return NormalF;
    }

    void FDynamicMeshEditor::SetQuadNormals( const FIndex2i& QuadTris, const FVector3f& Normal )
    {
        FDynamicMeshNormalOverlay* Normals   = Mesh->Attributes()->PrimaryNormals();
        FIndex3i                   Triangle1 = Mesh->GetTriangle( QuadTris.A );
        FIndex3i                   NormalTriangle1;
        for ( int j = 0; j < 3; ++j )
            NormalTriangle1[j] = Normals->AppendElement( Normal );
        Normals->SetTriangle( QuadTris.A, NormalTriangle1 );
        if ( Mesh->IsTriangle( QuadTris.B ) )
        {
            FIndex3i Triangle2 = Mesh->GetTriangle( QuadTris.B );
            FIndex3i NormalTriangle2;
            for ( int j = 0; j < 3; ++j )
            {
                int i              = Triangle1.IndexOf( Triangle2[j] );
                NormalTriangle2[j] = ( i == -1 ) ? Normals->AppendElement( Normal ) : NormalTriangle1[i];
            }
            Normals->SetTriangle( QuadTris.B, NormalTriangle2 );
        }
    }

    void FDynamicMeshEditor::SetTriangleNormals( const TArray<int>& Triangles )
    {
        FDynamicMeshNormalOverlay* Normals = Mesh->Attributes()->PrimaryNormals();
        TSet<int>                  TriangleSet( Triangles );
        auto TrianglePredicate = [&]( int32 TriangleID ) { return TriangleSet.Contains( TriangleID ); };
        TMap<int, int> Vertices;
        for ( int tid : Triangles )
        {
            if ( Normals->IsSetTriangle( tid ) )
                Normals->UnsetTriangle( tid );
            FIndex3i BaseTri = Mesh->GetTriangle( tid );
            FIndex3i ElemTri;
            for ( int j = 0; j < 3; ++j )
            {
                if ( const int* FoundElementID = Vertices.Find( BaseTri[j] ) )
                    ElemTri[j] = *FoundElementID;
                else
                {
                    FVector3d N = FMeshNormals::ComputeVertexNormal( *Mesh, BaseTri[j], TrianglePredicate );
                    ElemTri[j]  = Normals->AppendElement( FVector3f( (float)N.X, (float)N.Y, (float)N.Z ) );
                    Vertices.Add( BaseTri[j], ElemTri[j] );
                }
            }
            Normals->SetTriangle( tid, ElemTri );
        }
    }

    void FDynamicMeshEditor::SetQuadUVsFromProjection( const FIndex2i& QuadTris, const FVector3d& AxisX,
                                                       const FVector3d& AxisY, float UVScaleFactor,
                                                       const FVector2f& UVTranslation )
    {
        FDynamicMeshUVOverlay* UVs = Mesh->Attributes()->PrimaryUV();
        if ( !UVs )
            return;
        TMap<int, int> VertexToElement;
        for ( int TriIdx = 0; TriIdx < 2; ++TriIdx )
        {
            const int tid = TriIdx == 0 ? QuadTris.A : QuadTris.B;
            if ( !Mesh->IsTriangle( tid ) )
                continue;
            FIndex3i Tri = Mesh->GetTriangle( tid ), Elems;
            for ( int j = 0; j < 3; ++j )
            {
                if ( const int* Found = VertexToElement.Find( Tri[j] ) )
                {
                    Elems[j] = *Found;
                    continue;
                }
                const FVector3d P = Mesh->GetVertex( Tri[j] );
                FVector2f       UV( (float)P.Dot( AxisX ) * UVScaleFactor + UVTranslation.X,
                                    (float)P.Dot( AxisY ) * UVScaleFactor + UVTranslation.Y );
                Elems[j] = UVs->AppendElement( UV );
                VertexToElement.Add( Tri[j], Elems[j] );
            }
            UVs->SetTriangle( tid, Elems );
        }
    }

    void FDynamicMeshEditor::ReverseTriangleOrientations( const TArray<int>& Triangles, bool bInvertNormals )
    {
        for ( int tid : Triangles )
            Mesh->ReverseTriOrientation( tid );
        if ( bInvertNormals )
            InvertTriangleNormals( Triangles );
    }

    void FDynamicMeshEditor::InvertTriangleNormals( const TArray<int>& Triangles )
    {
        if ( !Mesh->HasAttributes() )
            return;
        FDynamicMeshAttributeSet* Attr = Mesh->Attributes();
        for ( int k = 0; k < Attr->NumNormalLayers(); ++k )
        {
            FDynamicMeshNormalOverlay* Normals = Attr->GetNormalLayer( k );
            TSet<int>                  Done;
            for ( int tid : Triangles )
            {
                if ( !Normals->IsSetTriangle( tid ) )
                    continue;
                FIndex3i Elems = Normals->GetTriangle( tid );
                for ( int j = 0; j < 3; ++j )
                {
                    if ( Done.Contains( Elems[j] ) )
                        continue;
                    Done.Add( Elems[j] );
                    FVector3f N = Normals->GetElement( Elems[j] );
                    Normals->SetElement( Elems[j], FVector3f( -N.X, -N.Y, -N.Z ) );
                }
            }
        }
    }
} // namespace Desert::Geometry
