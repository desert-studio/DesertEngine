// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMeshEditor.cpp (see the header for the
// line ranges and the adaptations).
#include "Engine/Geometry/UECore/DynamicMeshEditor.hpp"
#include "Engine/Geometry/UECore/MapLookup.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/UECore/IndexUtil.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <limits>
#include <Common/Core/Core.hpp>

namespace Desert::Geometry
{
    namespace
    {
        template <typename FuncType>
        bool StitchLoopsInternal( DynamicMeshEditor& Editor, int32_t NumQuads, FuncType&& GetQuadVidsForIndex,
                                  DynamicMeshEditResult& ResultOut )
        {
            ResultOut.NewQuads.reserve( NumQuads );
            ResultOut.NewGroups.reserve( NumQuads );
            bool bFailed = false;
            for ( int i = 0; i < NumQuads; ++i )
            {
                int32_t a = 0;
                int32_t b = 0;
                int32_t c = 0;
                int32_t d = 0;
                GetQuadVidsForIndex( i, a, b, c, d );
                const int NewGroupID = Editor.m_Mesh->AllocateTriangleGroup();
                ResultOut.NewGroups.push_back( NewGroupID );
                const int tid1 = Editor.m_Mesh->AppendTriangle( Index3i( b, a, d ), NewGroupID );
                const int tid2 = Editor.m_Mesh->AppendTriangle( Index3i( a, c, d ), NewGroupID );
                ResultOut.NewQuads.emplace_back( tid1, tid2 );
                if ( tid1 < 0 || tid2 < 0 )
                {
                    bFailed = true;
                    break;
                }
            }
            if ( !bFailed )
                return true;
            // Back out every triangle this stitch added (UE's operation_failed label).
            std::vector<int> Triangles;
            for ( const Index2i& Quad : ResultOut.NewQuads )
            {
                if ( Quad.A >= 0 )
                    Triangles.push_back( Quad.A );
                if ( Quad.B >= 0 )
                    Triangles.push_back( Quad.B );
            }
            // Best-effort rollback: the stitch has already failed and that failure is what the caller sees.
            static_cast<void>( Editor.RemoveTriangles( Triangles, false ) );
            return false;
        }
    } // namespace

    void VertexLoopToEdgeLoop( const DynamicMesh3& Mesh, const std::vector<int>& VertexLoop,
                               std::vector<int>& EdgeLoopOut )
    {
        const int N = static_cast<int32_t>( VertexLoop.size() );
        EdgeLoopOut.resize( N );
        for ( int i = 0; i < N; ++i )
            EdgeLoopOut[i] = Mesh.FindEdge( VertexLoop[i], VertexLoop[( i + 1 ) % N] );
    }

    bool DynamicMeshEditor::StitchVertexLoopsMinimal( const std::vector<int>& Loop1, const std::vector<int>& Loop2,
                                                      DynamicMeshEditResult& ResultOut )
    {
        const int N = static_cast<int32_t>( Loop1.size() );
        if ( !Common::EnsureOrWarn( N == static_cast<int32_t>( Loop2.size() ),
                                    "N == static_cast<int32_t>( Loop2.size() )" ) )
            return false;
        return StitchLoopsInternal(
             *this, N,
             [N, &Loop1, &Loop2]( int32_t Index, int32_t& VertA, int32_t& VertB, int32_t& VertC, int32_t& VertD )
             {
                 VertA = Loop1[Index];
                 VertB = Loop1[( Index + 1 ) % N];
                 VertC = Loop2[Index];
                 VertD = Loop2[( Index + 1 ) % N];
             },
             ResultOut );
    }

    bool DynamicMeshEditor::StitchVertexLoopToTriVidPairSequence( const std::vector<TriVidPair>& TriVidPairs,
                                                                  const std::vector<int>&        VertexLoop,
                                                                  DynamicMeshEditResult&         ResultOut )
    {
        const int N = static_cast<int32_t>( TriVidPairs.size() );
        if ( !Common::EnsureOrWarn( N == static_cast<int32_t>( VertexLoop.size() ),
                                    "N == static_cast<int32_t>( VertexLoop.size() )" ) )
            return false;
        return StitchLoopsInternal(
             *this, N,
             [this, N, &TriVidPairs, &VertexLoop]( int32_t Index, int32_t& VertA, int32_t& VertB, int32_t& VertC,
                                                   int32_t& VertD )
             {
                 Index3i TriVids1  = m_Mesh->GetTriangle( TriVidPairs[Index].first );
                 VertA             = TriVids1[TriVidPairs[Index].second.first];
                 VertB             = TriVids1[TriVidPairs[Index].second.second];
                 VertC             = VertexLoop[Index];
                 VertD             = VertexLoop[( Index + 1 ) % N];
             },
             ResultOut );
    }

    bool DynamicMeshEditor::ConvertLoopToTriVidPairSequence( const DynamicMesh3&      Mesh,
                                                             const std::vector<int>&  VidLoop,
                                                             const std::vector<int>&  EdgeLoop,
                                                             std::vector<TriVidPair>& TriVertPairsOut )
    {
        if ( !Common::EnsureOrWarn( EdgeLoop.size() == VidLoop.size(), "EdgeLoop.size() == VidLoop.size()" ) )
            return false;
        for ( int32_t QuadIndex = 0; QuadIndex < static_cast<int32_t>( EdgeLoop.size() ); ++QuadIndex )
        {
            int32_t const Tid       = Mesh.GetEdgeT( EdgeLoop[QuadIndex] ).A;
            int32_t const FirstVid  = VidLoop[QuadIndex];
            int32_t const SecondVid = VidLoop[( QuadIndex + 1 ) % static_cast<int32_t>( VidLoop.size() )];
            const Index3i TriVids   = Mesh.GetTriangle( Tid );
            auto const    SubIdx1   = static_cast<int8_t>( IndexUtil::FindTriIndex( FirstVid, TriVids ) );
            auto const    SubIdx2   = static_cast<int8_t>( IndexUtil::FindTriIndex( SecondVid, TriVids ) );
            if ( !( SubIdx1 >= 0 && SubIdx2 >= 0 ) )
                return false;
            TriVertPairsOut.emplace_back( Tid, std::pair<int8_t, int8_t>( SubIdx1, SubIdx2 ) );
        }
        return true;
    }

    bool DynamicMeshEditor::RemoveTriangles( const std::vector<int>& Triangles, bool bRemoveIsolatedVerts ) const
    {
        bool bAllOK = true;
        for ( int tid : Triangles )
        {
            if ( !m_Mesh->IsTriangle( tid ) )
                continue;
            if ( m_Mesh->RemoveTriangle( tid, bRemoveIsolatedVerts, false ) != MeshResult::Ok )
                bAllOK = false;
        }
        return bAllOK;
    }

    void DynamicMeshEditor::DuplicateTriangles( const std::vector<int>&       Triangles,
                                                std::unordered_map<int, int>& OldToNewVertex,
                                                DynamicMeshEditResult&        ResultOut ) const
    {
        ResultOut.Reset();
        std::unordered_map<int, int>              GroupMap;
        DynamicMeshAttributeSet*                  Attr = m_Mesh->HasAttributes() ? m_Mesh->Attributes() : nullptr;
        std::vector<std::unordered_map<int, int>> UVMaps;
        std::vector<std::unordered_map<int, int>> NormalMaps;
        if ( Attr )
        {
            UVMaps.resize( Attr->NumUVLayers() );
            NormalMaps.resize( Attr->NumNormalLayers() );
        }
        for ( int TriangleID : Triangles )
        {
            Index3i  Tri        = m_Mesh->GetTriangle( TriangleID );
            int      NewGroupID = -1;
            if ( m_Mesh->HasTriangleGroups() )
            {
                const int OldGroup = m_Mesh->GetTriangleGroup( TriangleID );
                if ( const int* Found = FindValue( GroupMap, OldGroup ) )
                    NewGroupID = *Found;
                else
                {
                    NewGroupID = m_Mesh->AllocateTriangleGroup();
                    GroupMap.insert_or_assign( OldGroup, NewGroupID );
                    ResultOut.NewGroups.push_back( NewGroupID );
                }
            }
            Index3i NewTri;
            for ( int j = 0; j < 3; ++j )
            {
                if ( const int* Found = FindValue( OldToNewVertex, Tri[j] ) )
                    NewTri[j] = *Found;
                else
                {
                    NewTri[j] = m_Mesh->AppendVertex( *m_Mesh, Tri[j] );
                    OldToNewVertex.insert_or_assign( Tri[j], NewTri[j] );
                    ResultOut.NewVertices.push_back( NewTri[j] );
                }
            }
            int NewTriangleID = m_Mesh->AppendTriangle( NewTri, NewGroupID );
            ResultOut.NewTriangles.push_back( NewTriangleID );
            if ( !Attr || NewTriangleID < 0 )
                continue;
            // CopyAttributes: every overlay element is duplicated once per source element.
            auto CopyOverlay = [&]( auto* Overlay, std::unordered_map<int, int>& Map )
            {
                if ( !Overlay || !Overlay->IsSetTriangle( TriangleID ) )
                    return;
                Index3i Elems = Overlay->GetTriangle( TriangleID );
                Index3i NewElems;
                for ( int j = 0; j < 3; ++j )
                {
                    if ( const int* Found = FindValue( Map, Elems[j] ) )
                        NewElems[j] = *Found;
                    else
                    {
                        NewElems[j] = Overlay->AppendElement( Overlay->GetElement( Elems[j] ) );
                        Map.insert_or_assign( Elems[j], NewElems[j] );
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

    bool DynamicMeshEditor::DisconnectTriangles( const std::vector<int>&   Triangles,
                                                 std::vector<LoopPairSet>& LoopSetOut,
                                                 bool bHandleBoundaryVertices, std::string& FailureOut ) const
    {
        MeshRegionBoundaryLoops RegionLoops( m_Mesh, Triangles, false );
        if ( !RegionLoops.Compute() )
        {
            FailureOut = RegionLoops.m_FailureReason;
            return false;
        }
        const std::unordered_set<int> TriangleSet( Triangles.begin(), Triangles.end() );
        return DisconnectTriangles( TriangleSet, RegionLoops.m_Loops, LoopSetOut, bHandleBoundaryVertices,
                                    FailureOut );
    }

    bool DynamicMeshEditor::DisconnectTriangles( const std::unordered_set<int>& TriangleSet,
                                                 const std::vector<EdgeLoop>&   Loops,
                                                 std::vector<LoopPairSet>&      LoopSetOut,
                                                 bool bHandleBoundaryVertices, std::string& FailureOut ) const
    {
        const int NumLoops = static_cast<int32_t>( Loops.size() );
        LoopSetOut.resize( NumLoops );
        std::vector<int>             FilteredTriangles;
        std::unordered_map<int, int> OldVidsToNewVids;
        for ( int li = 0; li < NumLoops; ++li )
        {
            const EdgeLoop& Loop                = Loops[li];
            LoopPairSet&    LoopPair            = LoopSetOut[li];
            LoopPair.OuterVertices         = Loop.Vertices;
            LoopPair.OuterEdges            = Loop.Edges;
            bool        bSawBoundaryInLoop = false;
            const int        NumVertices        = static_cast<int32_t>( Loop.Vertices.size() );
            std::vector<int> NewVertexLoop;
            NewVertexLoop.resize( NumVertices );
            for ( int vi = 0; vi < NumVertices; ++vi )
            {
                int VertID = Loop.Vertices[vi];
                // Already split as part of a bowtie: only the loop pairs are updated.
                if ( const int* ExistingNewVertID = FindValue( OldVidsToNewVids, VertID ) )
                {
                    if ( !m_Mesh->IsReferencedVertex( *ExistingNewVertID ) )
                    {
                        LoopPair.OuterVertices[vi]                                  = *ExistingNewVertID;
                        LoopPair.OuterEdges[vi]                                     = DynamicMesh3::InvalidID;
                        LoopPair.OuterEdges[( vi == 0 ) ? NumVertices - 1 : vi - 1] = DynamicMesh3::InvalidID;
                        NewVertexLoop[vi]                                           = VertID;
                    }
                    else
                        NewVertexLoop[vi] = *ExistingNewVertID;
                    continue;
                }
                FilteredTriangles.clear();
                int TriRingCount = 0;
                for ( const int RingTID : m_Mesh->VtxTrianglesItr( VertID ) )
                {
                    if ( TriangleSet.contains( RingTID ) )
                        FilteredTriangles.push_back( RingTID );
                    TriRingCount++;
                }
                if ( static_cast<int32_t>( FilteredTriangles.size() ) < TriRingCount )
                {
                    DynamicMeshInfo::VertexSplitInfo SplitInfo{};
                    const MeshResult MeshResult = m_Mesh->SplitVertex( VertID, FilteredTriangles, SplitInfo );
                    if ( MeshResult != MeshResult::Ok )
                    {
                        FailureOut = fmt::format( "splitting boundary vertex {} of loop {} failed", VertID, li );
                        return false;
                    }
                    OldVidsToNewVids.insert_or_assign( VertID, SplitInfo.NewVertex );
                    NewVertexLoop[vi] = SplitInfo.NewVertex;
                }
                else if ( bHandleBoundaryVertices )
                {
                    // A mesh-border vertex: the duplicate becomes the "old" one, the original stays inner.
                    int32_t const NewVertID = m_Mesh->AppendVertex( *m_Mesh, VertID );
                    OldVidsToNewVids.insert_or_assign( VertID, NewVertID );
                    LoopPair.OuterVertices[vi]                                  = NewVertID;
                    LoopPair.OuterEdges[vi]                                     = DynamicMesh3::InvalidID;
                    LoopPair.OuterEdges[( vi == 0 ) ? NumVertices - 1 : vi - 1] = DynamicMesh3::InvalidID;
                    NewVertexLoop[vi]                                           = VertID;
                    bSawBoundaryInLoop                                          = true;
                }
                else
                {
                    FailureOut = fmt::format( "vertex {} of loop {} lies on the mesh border", VertID, li );
                    return false;
                }
            }
            LoopPair.InnerVertices = NewVertexLoop;
            VertexLoopToEdgeLoop( *m_Mesh, NewVertexLoop, LoopPair.InnerEdges );
            for ( int e : LoopPair.InnerEdges )
                if ( e == DynamicMesh3::InvalidID )
                {
                    FailureOut = fmt::format( "the cut-loose copy of loop {} is not an edge loop", li );
                    return false;
                }
            LoopPair.bOuterIncludesIsolatedVertices = bSawBoundaryInLoop;
        }
        return true;
    }

    glm::vec3 DynamicMeshEditor::ComputeAndSetQuadNormal( const Index2i& QuadTris, bool bIsPlanar ) const
    {
        glm::dvec3 Normal = m_Mesh->GetTriNormal( QuadTris.A );
        if ( !bIsPlanar )
        {
            Normal = Normal + m_Mesh->GetTriNormal( QuadTris.B );
            Normalize( Normal );
        }
        const glm::vec3 NormalF( static_cast<float>( Normal.x ), static_cast<float>( Normal.y ),
                                 static_cast<float>( Normal.z ) );
        SetQuadNormals( QuadTris, NormalF );
        return NormalF;
    }

    void DynamicMeshEditor::SetQuadNormals( const Index2i& QuadTris, const glm::vec3& Normal ) const
    {
        DynamicMeshNormalOverlay*  Normals   = m_Mesh->Attributes()->PrimaryNormals();
        const Index3i              Triangle1 = m_Mesh->GetTriangle( QuadTris.A );
        Index3i                    NormalTriangle1;
        for ( int j = 0; j < 3; ++j )
            NormalTriangle1[j] = Normals->AppendElement( Normal );
        Normals->SetTriangle( QuadTris.A, NormalTriangle1 );
        if ( m_Mesh->IsTriangle( QuadTris.B ) )
        {
            Index3i Triangle2 = m_Mesh->GetTriangle( QuadTris.B );
            Index3i NormalTriangle2;
            for ( int j = 0; j < 3; ++j )
            {
                int i              = Triangle1.IndexOf( Triangle2[j] );
                NormalTriangle2[j] = ( i == -1 ) ? Normals->AppendElement( Normal ) : NormalTriangle1[i];
            }
            Normals->SetTriangle( QuadTris.B, NormalTriangle2 );
        }
    }

    void DynamicMeshEditor::SetTriangleNormals( const std::vector<int>& Triangles ) const
    {
        DynamicMeshNormalOverlay*  Normals = m_Mesh->Attributes()->PrimaryNormals();
        std::unordered_set<int>    TriangleSet( Triangles.begin(), Triangles.end() );
        auto TrianglePredicate = [&]( int32_t TriangleID ) { return TriangleSet.contains( TriangleID ); };
        std::unordered_map<int, int> Vertices;
        for ( int tid : Triangles )
        {
            if ( Normals->IsSetTriangle( tid ) )
                Normals->UnsetTriangle( tid );
            Index3i BaseTri = m_Mesh->GetTriangle( tid );
            Index3i ElemTri;
            for ( int j = 0; j < 3; ++j )
            {
                if ( const int* FoundElementID = FindValue( Vertices, BaseTri[j] ) )
                    ElemTri[j] = *FoundElementID;
                else
                {
                    const glm::dvec3 N =
                         MeshNormals::ComputeVertexNormal( *m_Mesh, BaseTri[j], TrianglePredicate );
                    ElemTri[j] = Normals->AppendElement( glm::vec3(
                         static_cast<float>( N.x ), static_cast<float>( N.y ), static_cast<float>( N.z ) ) );
                    Vertices.insert_or_assign( BaseTri[j], ElemTri[j] );
                }
            }
            Normals->SetTriangle( tid, ElemTri );
        }
    }

    void DynamicMeshEditor::SetQuadUVsFromProjection( const Index2i& QuadTris, const glm::dvec3& AxisX,
                                                      const glm::dvec3& AxisY, float UVScaleFactor,
                                                      const glm::vec2& UVTranslation ) const
    {
        DynamicMeshUVOverlay* UVs = m_Mesh->Attributes()->PrimaryUV();
        if ( !UVs )
            return;
        std::unordered_map<int, int> VertexToElement;
        for ( int TriIdx = 0; TriIdx < 2; ++TriIdx )
        {
            const int tid = TriIdx == 0 ? QuadTris.A : QuadTris.B;
            if ( !m_Mesh->IsTriangle( tid ) )
                continue;
            const Index3i Tri = m_Mesh->GetTriangle( tid );
            Index3i       Elems;
            for ( int j = 0; j < 3; ++j )
            {
                if ( const int* Found = FindValue( VertexToElement, Tri[j] ) )
                {
                    Elems[j] = *Found;
                    continue;
                }
                const glm::dvec3 P = m_Mesh->GetVertex( Tri[j] );
                const glm::vec2  UV( static_cast<float>( glm::dot( P, AxisX ) ) * UVScaleFactor + UVTranslation.x,
                                     static_cast<float>( glm::dot( P, AxisY ) ) * UVScaleFactor + UVTranslation.y );
                Elems[j] = UVs->AppendElement( UV );
                VertexToElement.insert_or_assign( Tri[j], Elems[j] );
            }
            UVs->SetTriangle( tid, Elems );
        }
    }

    void DynamicMeshEditor::ReverseTriangleOrientations( const std::vector<int>& Triangles,
                                                         bool                    bInvertNormals ) const
    {
        for ( int tid : Triangles )
            m_Mesh->ReverseTriOrientation( tid );
        if ( bInvertNormals )
            InvertTriangleNormals( Triangles );
    }

    void DynamicMeshEditor::InvertTriangleNormals( const std::vector<int>& Triangles ) const
    {
        if ( !m_Mesh->HasAttributes() )
            return;
        DynamicMeshAttributeSet* Attr = m_Mesh->Attributes();
        for ( int k = 0; k < Attr->NumNormalLayers(); ++k )
        {
            DynamicMeshNormalOverlay*  Normals = Attr->GetNormalLayer( k );
            std::unordered_set<int>    Done;
            for ( int tid : Triangles )
            {
                if ( !Normals->IsSetTriangle( tid ) )
                    continue;
                Index3i Elems = Normals->GetTriangle( tid );
                for ( int j = 0; j < 3; ++j )
                {
                    if ( Done.contains( Elems[j] ) )
                        continue;
                    Done.insert( Elems[j] );
                    const glm::vec3 N = Normals->GetElement( Elems[j] );
                    Normals->SetElement( Elems[j], glm::vec3( -N.x, -N.y, -N.z ) );
                }
            }
        }
    }

    // UE DynamicMeshEditor.cpp:553-590
    bool DynamicMeshEditor::AddTriangleFan_OrderedVertexLoop( int CenterVertex, const std::vector<int>& VertexLoop,
                                                              int GroupID, DynamicMeshEditResult& ResultOut ) const
    {
        if ( GroupID == -1 )
        {
            GroupID = m_Mesh->AllocateTriangleGroup();
            ResultOut.NewGroups.push_back( GroupID );
        }
        const int N = static_cast<int32_t>( VertexLoop.size() );
        for ( int i = 0; i < N; ++i )
        {
            const int A      = VertexLoop[i];
            const int B      = VertexLoop[( i + 1 ) % N];
            const int NewTID = m_Mesh->AppendTriangle( Index3i( CenterVertex, B, A ), GroupID );
            if ( NewTID < 0 )
            {
                // back out what was added so far
                const bool bRemoved = RemoveTriangles( ResultOut.NewTriangles, false );
                assert( bRemoved );
                return false;
            }
            ResultOut.NewTriangles.push_back( NewTID );
        }
        return true;
    }

    // UE DynamicMeshEditor.cpp:1231-1263
    void DynamicMeshEditor::SetTriangleNormals( const std::vector<int>& Triangles, const glm::vec3& Normal ) const
    {
        assert( m_Mesh->HasAttributes() );
        DynamicMeshNormalOverlay*    Normals = m_Mesh->Attributes()->PrimaryNormals();
        std::unordered_map<int, int> Vertices;
        for ( int Tid : Triangles )
        {
            if ( Normals->IsSetTriangle( Tid ) )
                Normals->UnsetTriangle( Tid );
            const Index3i BaseTri = m_Mesh->GetTriangle( Tid );
            Index3i       ElemTri;
            for ( int j = 0; j < 3; ++j )
            {
                const int* Found = FindValue( Vertices, BaseTri[j] );
                if ( Found == nullptr )
                {
                    ElemTri[j] = Normals->AppendElement( Normal );
                    Vertices.insert_or_assign( BaseTri[j], ElemTri[j] );
                }
                else
                {
                    ElemTri[j] = *Found;
                }
            }
            Normals->SetTriangle( Tid, ElemTri );
        }
    }

    // UE DynamicMeshEditor.cpp:1494-1549 with Frame3d(Origin, Normal).ToPlaneUV(P, 2): the frame is
    // Quaternion::SetFromTo(UnitZ, Normal) (Quaternion.h:420-460), so its X/Y axes are UnitX/UnitY rotated by it.
    void DynamicMeshEditor::SetTriangleUVsFromProjection( const std::vector<int>& Triangles,
                                                          const glm::dvec3& Origin, const glm::dvec3& Normal,
                                                          float UVScaleFactor ) const
    {
        if ( Triangles.empty() )
            return;
        assert( m_Mesh->HasAttributes() && m_Mesh->Attributes()->NumUVLayers() > 0 );
        DynamicMeshUVOverlay* UVs = m_Mesh->Attributes()->PrimaryUV();

        // SetFromTo(UnitZ, Normal): W = from.bisector, XYZ = from x bisector; an antiparallel Normal takes UE's
        // first W == 0 branch (|from.X| >= |from.Y| holds for UnitZ): X = -1, Y = Z = 0.
        const glm::dvec3 From( 0, 0, 1 );
        glm::dvec3       To = Normal;
        Normalize( To );
        glm::dvec3   Bisector       = From + To;
        const double BisectorLength = Normalize( Bisector );
        double       QW             = 0;
        glm::dvec3   QV( -1, 0, 0 );
        if ( BisectorLength > ZeroTolerance<double> )
        {
            QW = glm::dot( From, Bisector );
            QV = glm::cross( From, Bisector );
        }
        const auto Rotate = [&]( const glm::dvec3& V ) -> glm::dvec3
        {
            const glm::dvec3 T = 2.0 * glm::cross( QV, V );
            return V + QW * T + glm::cross( QV, T );
        };
        const glm::dvec3 AxisX = Rotate( glm::dvec3( 1, 0, 0 ) );
        const glm::dvec3 AxisY = Rotate( glm::dvec3( 0, 1, 0 ) );

        std::unordered_map<int, int> BaseToOverlay;
        std::vector<int>             AllUVIndices;
        glm::vec2                    UVMin( std::numeric_limits<float>::max(), std::numeric_limits<float>::max() );
        for ( int Tid : Triangles )
        {
            if ( UVs->IsSetTriangle( Tid ) )
                UVs->UnsetTriangle( Tid );
            const Index3i BaseTri = m_Mesh->GetTriangle( Tid );
            Index3i       ElemTri;
            for ( int j = 0; j < 3; ++j )
            {
                const int* Found = FindValue( BaseToOverlay, BaseTri[j] );
                if ( Found == nullptr )
                {
                    const glm::dvec3 Local = m_Mesh->GetVertex( BaseTri[j] ) - Origin;
                    const glm::vec2  UV( static_cast<float>( glm::dot( Local, AxisX ) ),
                                         static_cast<float>( glm::dot( Local, AxisY ) ) );
                    UVMin.x    = std::min( UVMin.x, UV.x );
                    UVMin.y    = std::min( UVMin.y, UV.y );
                    ElemTri[j] = UVs->AppendElement( UV );
                    AllUVIndices.push_back( ElemTri[j] );
                    BaseToOverlay.insert_or_assign( BaseTri[j], ElemTri[j] );
                }
                else
                {
                    ElemTri[j] = *Found;
                }
            }
            UVs->SetTriangle( Tid, ElemTri );
        }
        // shift so the bounding box min corner is at the origin, then scale
        for ( int UVID : AllUVIndices )
            UVs->SetElement( UVID, ( UVs->GetElement( UVID ) - UVMin ) * UVScaleFactor );
    }
} // namespace Desert::Geometry
