// Ported from UE 5.8 .../DynamicMesh/Private/Operations/OffsetMeshRegion.cpp (see the header for the line ranges
// and the adaptations).
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/OffsetMeshRegion.hpp"
#include "Engine/Geometry/MeshCore/MapLookup.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/DynamicMeshEditor.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Geometry
{
    void FindConnectedTriangleComponents( const DynamicMesh3& Mesh, const std::vector<int32_t>& Triangles,
                                          std::vector<std::vector<int32_t>>& ComponentsOut )
    {
        std::unordered_set<int32_t> const Remaining( Triangles.begin(), Triangles.end() );
        std::unordered_set<int32_t>       Visited;
        for ( int32_t const Seed : Triangles )
        {
            if ( Visited.contains( Seed ) )
                continue;
            std::vector<int32_t> Component;
            std::vector<int32_t> Stack;
            Stack.push_back( Seed );
            Visited.insert( Seed );
            while ( !Stack.empty() )
            {
                const int32_t tid = Stack.back();
                Stack.pop_back();
                Component.push_back( tid );
                const Index3i Nbrs = Mesh.GetTriNeighbourTris( tid );
                for ( int j = 0; j < 3; ++j )
                    if ( Nbrs[j] >= 0 && Remaining.contains( Nbrs[j] ) && !Visited.contains( Nbrs[j] ) )
                    {
                        Visited.insert( Nbrs[j] );
                        Stack.push_back( Nbrs[j] );
                    }
            }
            std::sort( Component.begin(), Component.end() );
            ComponentsOut.push_back( Component );
        }
    }

    void
    ComputeNewGroupIDsAlongEdgeLoop( DynamicMesh3& Mesh, const std::vector<int32_t>& LoopEdgeIDs,
                                     std::vector<int32_t>&                          NewLoopEdgeGroupIDs,
                                     std::vector<int32_t>&                          NewGroupIDsOut,
                                     const std::function<bool( int32_t, int32_t )>& EdgesShouldHaveSameGroupFunc )
    {
        auto const NumEdgeIDs = static_cast<int32_t>( LoopEdgeIDs.size() );
        NewLoopEdgeGroupIDs.resize( NumEdgeIDs );
        if ( NumEdgeIDs <= 2 )
        {
            if ( NumEdgeIDs > 0 )
            {
                int32_t const OneGroupID = Mesh.AllocateTriangleGroup();
                NewLoopEdgeGroupIDs.assign( NumEdgeIDs, OneGroupID );
                NewGroupIDsOut.push_back( OneGroupID );
            }
            return;
        }
        NewLoopEdgeGroupIDs[0] = Mesh.AllocateTriangleGroup();
        NewGroupIDsOut.push_back( NewLoopEdgeGroupIDs[0] );
        int32_t LastDifferentGroupIndex = NumEdgeIDs - 1;
        while ( LastDifferentGroupIndex > 0 &&
                EdgesShouldHaveSameGroupFunc( LoopEdgeIDs[0], LoopEdgeIDs[LastDifferentGroupIndex] ) )
        {
            NewLoopEdgeGroupIDs[LastDifferentGroupIndex] = NewLoopEdgeGroupIDs[0];
            --LastDifferentGroupIndex;
        }
        for ( int32_t j = 1; j <= LastDifferentGroupIndex; ++j )
        {
            if ( !EdgesShouldHaveSameGroupFunc( LoopEdgeIDs[j], LoopEdgeIDs[j - 1] ) )
            {
                NewLoopEdgeGroupIDs[j] = Mesh.AllocateTriangleGroup();
                NewGroupIDsOut.push_back( NewLoopEdgeGroupIDs[j] );
            }
            else
                NewLoopEdgeGroupIDs[j] = NewLoopEdgeGroupIDs[j - 1];
        }
    }

    namespace
    {
        bool EdgesAreParallel( DynamicMesh3* Mesh, int32_t Eid1, int32_t Eid2 )
        {
            Index2i const    Vids1      = Mesh->GetEdgeV( Eid1 );
            Index2i const    Vids2      = Mesh->GetEdgeV( Eid2 );
            glm::dvec3       Vec1       = Mesh->GetVertex( Vids1.A ) - Mesh->GetVertex( Vids1.B );
            glm::dvec3       Vec2       = Mesh->GetVertex( Vids2.A ) - Mesh->GetVertex( Vids2.B );
            constexpr double KindaSmall = 1e-4;
            if ( Normalize( Vec1, KindaSmall ) == 0 || Normalize( Vec2, KindaSmall ) == 0 )
                return true;
            return std::abs( glm::dot( Vec1, Vec2 ) ) >= 1 - KindaSmall;
        }

        int32_t FindLoopShiftFromGroupIDs( const std::vector<int32_t>& GroupIDs )
        {
            auto const N = static_cast<int32_t>( GroupIDs.size() );
            if ( GroupIDs[0] != GroupIDs[N - 1] )
                return 0;
            for ( int32_t k = 0; k < N - 1; ++k )
                if ( GroupIDs[k] != GroupIDs[k + 1] )
                    return k + 1;
            return 0;
        }

        template <typename ValueType>
        void LeftShiftArray( std::vector<ValueType>& Values, int ShiftNum )
        {
            if ( ShiftNum == 0 )
                return;
            auto const             N = static_cast<int32_t>( Values.size() );
            std::vector<ValueType> Tmp;
            Tmp.resize( N );
            for ( int32_t k = 0; k < N; ++k )
                Tmp[k] = Values[( ShiftNum + k ) % N];
            Values = Tmp;
        }

        glm::dvec3 GetAngleWeightedAverageNormal( const DynamicMesh3& Mesh, int32_t VertexID,
                                                  const std::unordered_set<int32_t>& TriangleList )
        {
            glm::dvec3 ExtrusionVector( 0, 0, 0 );
            for ( int32_t const TriangleID : Mesh.VtxTrianglesItr( VertexID ) )
                if ( TriangleList.contains( TriangleID ) )
                {
                    Index3i const Triangle = Mesh.GetTriangle( TriangleID );
                    double const  Angle    = Mesh.GetTriInternalAngleR( TriangleID, Triangle.IndexOf( VertexID ) );
                    ExtrusionVector        = ExtrusionVector + Mesh.GetTriNormal( TriangleID ) * Angle;
                }
            Normalize( ExtrusionVector );
            return ExtrusionVector;
        }

        glm::dvec3 GetAngleWeightedAdjustedNormal( const DynamicMesh3& Mesh, int32_t VertexID,
                                                   const std::unordered_set<int32_t>& TriangleList,
                                                   double                             MaxAdjustmentScale )
        {
            glm::dvec3 const InitialExtrusionVector =
                 GetAngleWeightedAverageNormal( Mesh, VertexID, TriangleList );
            double       AngleSum         = 0;
            double       Adjustment       = 0;
            double const InvertedMaxScale = std::max( 1e-8, 1.0 / MaxAdjustmentScale );
            for ( int32_t const TriangleID : Mesh.VtxTrianglesItr( VertexID ) )
                if ( TriangleList.contains( TriangleID ) )
                {
                    Index3i const Triangle = Mesh.GetTriangle( TriangleID );
                    double const  Angle    = Mesh.GetTriInternalAngleR( TriangleID, Triangle.IndexOf( VertexID ) );
                    double        CosTheta = glm::dot( Mesh.GetTriNormal( TriangleID ), InitialExtrusionVector );
                    CosTheta               = std::max( CosTheta, InvertedMaxScale );
                    Adjustment += Angle / CosTheta;
                    AngleSum += Angle;
                }
            Adjustment /= AngleSum;
            return InitialExtrusionVector * Adjustment;
        }

        // UE ComputeAverageUVScaleRatioAlongVertexPath (PolyEditingUVUtil.cpp:55): UV length over mesh length
        // along the path's edges that have UVs; 0 when none do.
        double UVScaleRatioAlongPath( const DynamicMesh3& Mesh, const DynamicMeshUVOverlay& UVOverlay,
                                      const std::vector<int32_t>& VertexPath, double& PathLengthOut )
        {
            double MeshLength = 0;
            double UVLength   = 0;
            for ( int32_t k = 0; k + 1 < static_cast<int32_t>( VertexPath.size() ); ++k )
            {
                int32_t const EdgeID = Mesh.FindEdge( VertexPath[k], VertexPath[k + 1] );
                if ( EdgeID == DynamicMesh3::InvalidID )
                    continue;
                double const EdgeLength =
                     Distance( Mesh.GetVertex( VertexPath[k] ), Mesh.GetVertex( VertexPath[k + 1] ) );
                Index2i const EdgeTris = Mesh.GetEdgeT( EdgeID );
                double        EdgeUV   = 0;
                int           Count    = 0;
                for ( int32_t j = 0; j < 2; ++j )
                {
                    const int t = j == 0 ? EdgeTris.A : EdgeTris.B;
                    if ( t == DynamicMesh3::InvalidID || !UVOverlay.IsSetTriangle( t ) )
                        continue;
                    int32_t const EdgeIdx = Mesh.GetTriEdges( t ).IndexOf( EdgeID );
                    if ( EdgeIdx < 0 )
                        continue;
                    glm::vec2 UVs[3]{};
                    UVOverlay.GetTriElements( t, UVs[0], UVs[1], UVs[2] );
                    const glm::vec2 D = UVs[EdgeIdx] - UVs[( EdgeIdx + 1 ) % 3];
                    EdgeUV += std::sqrt( static_cast<double>( D.x ) * D.x + static_cast<double>( D.y ) * D.y );
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
        // (row 1, the offset loop) - UE's QuadGridPatch after its second ReverseRows, NumSubdivisions = 0.
        struct Strip
        {
            std::vector<int32_t> Outer, Inner; // Columns + 1 vertices each
            std::vector<Index2i> Quads;
        };

        void ComputeUVIslandForStrip( DynamicMesh3& Mesh, const Strip& Strip, double UVScaleFactor )
        {
            DynamicMeshUVOverlay* UVOverlay = Mesh.Attributes()->PrimaryUV();
            if ( UVOverlay == nullptr )
                return;
            for ( const Index2i& Q : Strip.Quads )
            {
                UVOverlay->UnsetTriangle( Q.A );
                UVOverlay->UnsetTriangle( Q.B );
            }
            double UVLengthScale  = 0;
            double UVLengthWeight = 0;
            for ( int32_t k = 0; k < 2; ++k )
            {
                double       PathLength = 0;
                double const Ratio =
                     UVScaleRatioAlongPath( Mesh, *UVOverlay, k == 0 ? Strip.Outer : Strip.Inner, PathLength );
                if ( PathLength > 0 )
                {
                    UVLengthScale += Ratio;
                    UVLengthWeight += 1.0;
                }
            }
            UVLengthScale =
                 ( UVLengthWeight == 0 || UVLengthScale == 0 ) ? 1.0 : ( UVLengthScale / UVLengthWeight );
            const auto           NumU = static_cast<int32_t>( Strip.Outer.size() );
            std::vector<int32_t> Row0;
            std::vector<int32_t> Row1;
            double               AccumDistU = 0;
            for ( int32_t k = 0; k < NumU; ++k )
            {
                double const DistV =
                     Distance( Mesh.GetVertex( Strip.Outer[k] ), Mesh.GetVertex( Strip.Inner[k] ) );
                auto const UseU = static_cast<float>( UVLengthScale * AccumDistU * UVScaleFactor );
                auto const EndV = static_cast<float>( UVLengthScale * DistV * UVScaleFactor );
                Row0.push_back( UVOverlay->AppendElement( glm::vec2( UseU, 0.0f ) ) );
                Row1.push_back( UVOverlay->AppendElement( glm::vec2( UseU, EndV ) ) );
                if ( k < NumU - 1 )
                    AccumDistU +=
                         Distance( Mesh.GetVertex( Strip.Outer[k] ), Mesh.GetVertex( Strip.Outer[k + 1] ) );
            }
            for ( int32_t q = 0; q < static_cast<int32_t>( Strip.Quads.size() ); ++q )
                for ( int32_t TriIdx = 0; TriIdx < 2; ++TriIdx )
                {
                    const int tid = TriIdx == 0 ? Strip.Quads[q].A : Strip.Quads[q].B;
                    Index3i   Tri = Mesh.GetTriangle( tid );
                    Index3i   UVTri;
                    for ( int j = 0; j < 3; ++j )
                    {
                        const int v = Tri[j];
                        if ( v == Strip.Outer[q] )
                            UVTri[j] = Row0[q];
                        else if ( v == Strip.Outer[q + 1] )
                            UVTri[j] = Row0[q + 1];
                        else if ( v == Strip.Inner[q] )
                            UVTri[j] = Row1[q];
                        else
                            UVTri[j] = Row1[q + 1];
                    }
                    UVOverlay->SetTriangle( tid, UVTri );
                }
        }
    } // namespace

    bool OffsetMeshRegion::EdgesSeparateSameGroupsAndAreColinearAtBorder( DynamicMesh3* Mesh, int32_t Eid1,
                                                                          int32_t Eid2,
                                                                          bool    bCheckColinearityAtBorder )
    {
        if ( !Mesh->IsEdge( Eid1 ) || !Mesh->IsEdge( Eid2 ) )
            return false;
        const int     Invalid = DynamicMesh3::InvalidID;
        Index2i const Tris1   = Mesh->GetEdgeT( Eid1 );
        Index2i const Groups1( Mesh->GetTriangleGroup( Tris1.A ),
                               Tris1.B == Invalid ? Invalid : Mesh->GetTriangleGroup( Tris1.B ) );
        Index2i const Tris2 = Mesh->GetEdgeT( Eid2 );
        Index2i const Groups2( Mesh->GetTriangleGroup( Tris2.A ),
                               Tris2.B == Invalid ? Invalid : Mesh->GetTriangleGroup( Tris2.B ) );
        if ( bCheckColinearityAtBorder && Groups1.A == Groups2.A && Groups1.B == Invalid && Groups2.B == Invalid )
            return EdgesAreParallel( Mesh, Eid1, Eid2 );
        return ( Groups1.A == Groups2.A && Groups1.B == Groups2.B ) ||
               ( Groups1.A == Groups2.B && Groups1.B == Groups2.A );
    }

    bool OffsetMeshRegion::Apply()
    {
        std::vector<std::vector<int32_t>> Components;
        FindConnectedTriangleComponents( *m_Mesh, m_Triangles, Components );
        bool bAllOK = true;
        m_OffsetRegions.resize( static_cast<int32_t>( Components.size() ) );
        for ( int k = 0; k < static_cast<int32_t>( Components.size() ); ++k )
        {
            OffsetInfo& Region = m_OffsetRegions[k];
            Region.OffsetTids  = Components[k];
            if ( m_bOffsetFullComponentsAsSolids )
            {
                // GrowToConnectedTriangles: the region is a whole component when no triangle outside it touches
                // it.
                std::unordered_set<int32_t> const InRegion( Region.OffsetTids.begin(), Region.OffsetTids.end() );
                bool                              bTouchesOutside = false;
                for ( int32_t const tid : Region.OffsetTids )
                {
                    const Index3i Nbrs = m_Mesh->GetTriNeighbourTris( tid );
                    for ( int j = 0; j < 3; ++j )
                        bTouchesOutside |= Nbrs[j] >= 0 && !InRegion.contains( Nbrs[j] );
                }
                Region.bIsSolid = !bTouchesOutside;
            }
            if ( !ApplyOffset( Region ) )
            {
                m_FailureReason = fmt::format( "region {} of {}: {}", k, static_cast<int32_t>( Components.size() ),
                                               m_FailureReason );
                bAllOK          = false;
            }
        }
        return bAllOK;
    }

    bool OffsetMeshRegion::ApplyOffset( OffsetInfo& Region )
    {
        const std::vector<int32_t>&          RegionTriangles = Region.OffsetTids;
        std::unordered_map<int32_t, int32_t> OffsetGroupMap;
        if ( m_Mesh->HasTriangleGroups() )
            for ( int32_t const TriangleID : RegionTriangles )
            {
                int32_t const CurGroupID = m_Mesh->GetTriangleGroup( TriangleID );
                int32_t       NewGroupID = 0;
                if ( const int32_t* Found = FindValue( OffsetGroupMap, CurGroupID ) )
                    NewGroupID = *Found;
                else
                {
                    NewGroupID = m_Mesh->AllocateTriangleGroup();
                    OffsetGroupMap.insert_or_assign( CurGroupID, NewGroupID );
                    Region.OffsetGroups.push_back( NewGroupID );
                }
                m_Mesh->SetTriangleGroup( TriangleID, NewGroupID );
            }

        MeshRegionBoundaryLoops InitialLoops( m_Mesh, RegionTriangles, false );
        if ( !InitialLoops.Compute() )
        {
            m_FailureReason = InitialLoops.m_FailureReason;
            return false;
        }
        m_AllModifiedAndNewTriangles.insert( m_AllModifiedAndNewTriangles.end(), RegionTriangles.begin(),
                                             RegionTriangles.end() );
        std::unordered_set<int32_t> const TriangleSet( RegionTriangles.begin(), RegionTriangles.end() );

        std::vector<std::vector<int32_t>> LoopsEdgeGroups;
        std::vector<int32_t>              NewGroupIDs;
        LoopsEdgeGroups.resize( static_cast<int32_t>( InitialLoops.m_Loops.size() ) );
        for ( int32_t i = 0; i < static_cast<int32_t>( InitialLoops.m_Loops.size() ); ++i )
        {
            std::vector<int32_t>& CurrentEdgeGroups = LoopsEdgeGroups[i];
            ComputeNewGroupIDsAlongEdgeLoop( *m_Mesh, InitialLoops.m_Loops[i].Edges, CurrentEdgeGroups,
                                             NewGroupIDs, LoopEdgesShouldHaveSameGroup );
            int const GroupShift = FindLoopShiftFromGroupIDs( CurrentEdgeGroups );
            LeftShiftArray( InitialLoops.m_Loops[i].Vertices, GroupShift );
            LeftShiftArray( InitialLoops.m_Loops[i].Edges, GroupShift );
            LeftShiftArray( CurrentEdgeGroups, GroupShift );
        }

        // The material of the region triangle on each loop edge, read before the loops are cut and stitched.
        DynamicMeshMaterialAttribute* MaterialIDAttrib =
             ( m_Mesh->HasAttributes() && m_Mesh->Attributes()->HasMaterialID() )
                  ? m_Mesh->Attributes()->GetMaterialID()
                  : nullptr;

        DynamicMeshEditor                           Editor( m_Mesh );
        std::vector<DynamicMeshEditor::LoopPairSet> LoopPairs;
        DynamicMeshEditResult                       DuplicateResult;
        if ( Region.bIsSolid )
        {
            std::unordered_map<int, int> IndexMap;
            Editor.DuplicateTriangles( RegionTriangles, IndexMap, DuplicateResult );
            m_AllModifiedAndNewTriangles.insert( m_AllModifiedAndNewTriangles.end(),
                                                 DuplicateResult.NewTriangles.begin(),
                                                 DuplicateResult.NewTriangles.end() );
            LoopPairs.resize( static_cast<int32_t>( InitialLoops.m_Loops.size() ) );
            for ( int LoopIndex = 0; LoopIndex < static_cast<int32_t>( InitialLoops.m_Loops.size() ); ++LoopIndex )
            {
                EdgeLoop const&                 BaseLoop = InitialLoops.m_Loops[LoopIndex];
                DynamicMeshEditor::LoopPairSet& LoopPair = LoopPairs[LoopIndex];
                LoopPair.InnerVertices                   = BaseLoop.Vertices;
                LoopPair.InnerEdges                      = BaseLoop.Edges;
                if ( !m_bIsPositiveOffset )
                {
                    std::reverse( LoopPair.InnerVertices.begin(), LoopPair.InnerVertices.end() );
                    int32_t const LastEid = LoopPair.InnerEdges.back();
                    LoopPair.InnerEdges.pop_back();
                    std::reverse( LoopPair.InnerEdges.begin(), LoopPair.InnerEdges.end() );
                    LoopPair.InnerEdges.push_back( LastEid );
                    int32_t const LastEdgeGroupID = LoopsEdgeGroups[LoopIndex].back();
                    LoopsEdgeGroups[LoopIndex].pop_back();
                    std::reverse( LoopsEdgeGroups[LoopIndex].begin(), LoopsEdgeGroups[LoopIndex].end() );
                    LoopsEdgeGroups[LoopIndex].push_back( LastEdgeGroupID );
                }
                for ( int32_t const Vid : LoopPair.InnerVertices )
                    LoopPair.OuterVertices.push_back( IndexMap[Vid] );
                VertexLoopToEdgeLoop( *m_Mesh, LoopPair.OuterVertices, LoopPair.OuterEdges );
            }
        }
        else if ( !Editor.DisconnectTriangles( TriangleSet, InitialLoops.m_Loops, LoopPairs, true,
                                               m_FailureReason ) )
            return false;

        // Materials along each (inner) loop, from the region triangle on the edge.
        std::vector<std::vector<int32_t>> LoopMaterialIDs;
        LoopMaterialIDs.resize( static_cast<int32_t>( LoopPairs.size() ) );
        if ( MaterialIDAttrib != nullptr )
            for ( int32_t i = 0; i < static_cast<int32_t>( LoopPairs.size() ); ++i )
                for ( int32_t const e : LoopPairs[i].InnerEdges )
                    LoopMaterialIDs[i].push_back( m_bInferMaterialID
                                                       ? MaterialIDAttrib->GetValue( m_Mesh->GetEdgeT( e ).A )
                                                       : m_SetMaterialID );

        // FMeshVertexSelection::SelectTriangleVertices, in ascending order.
        std::unordered_set<int32_t> SelectedSet;
        for ( int32_t const tid : RegionTriangles )
        {
            const Index3i Tri = m_Mesh->GetTriangle( tid );
            for ( int j = 0; j < 3; ++j )
                SelectedSet.insert( Tri[j] );
        }
        std::vector<int32_t> SelectedVids;
        SelectedVids.reserve( SelectedSet.size() );
        for ( int32_t const v : SelectedSet )
            SelectedVids.push_back( v );
        std::sort( SelectedVids.begin(), SelectedVids.end() );

        std::vector<glm::dvec3> VertexExtrudeVectors;
        VertexExtrudeVectors.resize( static_cast<int32_t>( SelectedVids.size() ) );
        for ( int32_t i = 0; i < static_cast<int32_t>( SelectedVids.size() ); ++i )
        {
            switch ( m_ExtrusionVectorType )
            {
                case VertexExtrusionVectorType::Zero:
                    VertexExtrudeVectors[i] = glm::dvec3( 0, 0, 0 );
                    break;
                case VertexExtrusionVectorType::VertexNormal:
                    VertexExtrudeVectors[i] = MeshNormals::ComputeVertexNormal( *m_Mesh, SelectedVids[i] );
                    break;
                case VertexExtrusionVectorType::SelectionTriNormalsAngleWeightedAverage:
                    VertexExtrudeVectors[i] =
                         GetAngleWeightedAverageNormal( *m_Mesh, SelectedVids[i], TriangleSet );
                    break;
                case VertexExtrusionVectorType::SelectionTriNormalsAngleWeightedAdjusted:
                    VertexExtrudeVectors[i] = GetAngleWeightedAdjustedNormal(
                         *m_Mesh, SelectedVids[i], TriangleSet, m_MaxScaleForAdjustingTriNormalsOffset );
                    break;
            }
        }
        for ( int32_t i = 0; i < static_cast<int32_t>( SelectedVids.size() ); ++i )
        {
            const int32_t VertexID = SelectedVids[i];
            m_Mesh->SetVertex( VertexID, OffsetPositionFunc( m_Mesh->GetVertex( VertexID ),
                                                             VertexExtrudeVectors[i], VertexID ) );
        }

        // StitchRegionBorderLoopPairs_Version1 with NumSubdivisions = 0.
        bool bSuccess = true;
        Region.BaseLoops.resize( static_cast<int32_t>( LoopPairs.size() ) );
        Region.OffsetLoops.resize( static_cast<int32_t>( LoopPairs.size() ) );
        Region.StitchTriangles.resize( static_cast<int32_t>( LoopPairs.size() ) );
        Region.StitchPolygonIDs.resize( static_cast<int32_t>( LoopPairs.size() ) );
        for ( int32_t LoopIndex = 0; LoopIndex < static_cast<int32_t>( LoopPairs.size() ); ++LoopIndex )
        {
            const std::vector<int32_t>& OuterLoopV = LoopPairs[LoopIndex].OuterVertices;
            const std::vector<int32_t>& InnerLoopV = LoopPairs[LoopIndex].InnerVertices;
            const auto                  NV         = static_cast<int32_t>( OuterLoopV.size() );
            DynamicMeshEditResult       StitchResult;
            if ( !Editor.StitchVertexLoopsMinimal( InnerLoopV, OuterLoopV, StitchResult ) )
            {
                m_FailureReason =
                     fmt::format( "loop {} ({} vertices) could not be stitched to its copy", LoopIndex, NV );
                bSuccess = false;
                continue;
            }
            const std::vector<int32_t>& PerEdgeNewGroupIDs = LoopsEdgeGroups[LoopIndex];
            for ( int32_t q = 0; q < NV; ++q )
            {
                const Index2i QuadTris = StitchResult.NewQuads[q];
                m_Mesh->SetTriangleGroup( QuadTris.A, PerEdgeNewGroupIDs[q] );
                m_Mesh->SetTriangleGroup( QuadTris.B, PerEdgeNewGroupIDs[q] );
                if ( MaterialIDAttrib != nullptr )
                {
                    MaterialIDAttrib->SetValue( QuadTris.A, LoopMaterialIDs[LoopIndex][q] );
                    MaterialIDAttrib->SetValue( QuadTris.B, LoopMaterialIDs[LoopIndex][q] );
                }
            }
            // Split the columns into strips of equal group (SplitColumnsByPredicate); the loop was shifted so
            // column 0 starts a group.
            std::vector<Strip> GroupStrips;
            for ( int32_t q = 0; q < NV; ++q )
            {
                if ( q == 0 || PerEdgeNewGroupIDs[q] != PerEdgeNewGroupIDs[q - 1] )
                {
                    GroupStrips.emplace_back();
                    GroupStrips.back().Outer.push_back( OuterLoopV[q] );
                    GroupStrips.back().Inner.push_back( InnerLoopV[q] );
                }
                GroupStrips.back().Outer.push_back( OuterLoopV[( q + 1 ) % NV] );
                GroupStrips.back().Inner.push_back( InnerLoopV[( q + 1 ) % NV] );
                GroupStrips.back().Quads.push_back( StitchResult.NewQuads[q] );
            }
            if ( m_Mesh->HasAttributes() )
            {
                for ( const Strip& Strip : GroupStrips )
                {
                    std::vector<int32_t> PatchTriangles;
                    for ( const Index2i& Q : Strip.Quads )
                    {
                        PatchTriangles.push_back( Q.A );
                        PatchTriangles.push_back( Q.B );
                    }
                    Editor.SetTriangleNormals( PatchTriangles );
                    if ( m_bUVIslandPerGroup )
                        ComputeUVIslandForStrip( *m_Mesh, Strip, m_UVScaleFactor );
                }
                if ( !m_bUVIslandPerGroup )
                {
                    Strip Whole;
                    for ( int32_t q = 0; q <= NV; ++q )
                    {
                        Whole.Outer.push_back( OuterLoopV[q % NV] );
                        Whole.Inner.push_back( InnerLoopV[q % NV] );
                    }
                    Whole.Quads = StitchResult.NewQuads;
                    ComputeUVIslandForStrip( *m_Mesh, Whole, m_UVScaleFactor );
                }
            }
            for ( const Index2i& Q : StitchResult.NewQuads )
            {
                Region.StitchTriangles[LoopIndex].push_back( Q.A );
                Region.StitchTriangles[LoopIndex].push_back( Q.B );
                if ( std::find( Region.StitchPolygonIDs[LoopIndex].begin(),
                                Region.StitchPolygonIDs[LoopIndex].end(),
                                m_Mesh->GetTriangleGroup( Q.A ) ) == Region.StitchPolygonIDs[LoopIndex].end() )
                {
                    Region.StitchPolygonIDs[LoopIndex].push_back( m_Mesh->GetTriangleGroup( Q.A ) );
                }
            }
            Region.BaseLoops[LoopIndex].Vertices   = OuterLoopV;
            Region.OffsetLoops[LoopIndex].Vertices = InnerLoopV;
            VertexLoopToEdgeLoop( *m_Mesh, OuterLoopV, Region.BaseLoops[LoopIndex].Edges );
            VertexLoopToEdgeLoop( *m_Mesh, InnerLoopV, Region.OffsetLoops[LoopIndex].Edges );
        }

        if ( Region.bIsSolid )
        {
            if ( m_bIsPositiveOffset )
                Editor.ReverseTriangleOrientations( DuplicateResult.NewTriangles, true );
            else
                Editor.ReverseTriangleOrientations( RegionTriangles, true );
        }
        if ( m_bSingleGroupPerArea && m_Mesh->HasTriangleGroups() &&
             static_cast<int32_t>( Region.OffsetGroups.size() ) > 1 )
        {
            for ( int32_t const TriangleID : RegionTriangles )
                m_Mesh->SetTriangleGroup( TriangleID, Region.OffsetGroups[0] );
            Region.OffsetGroups.resize( 1 );
        }
        return bSuccess;
    }
} // namespace Desert::Geometry
