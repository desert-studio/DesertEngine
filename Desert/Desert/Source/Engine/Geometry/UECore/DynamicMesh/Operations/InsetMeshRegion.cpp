// Ported from UE 5.8 .../DynamicMesh/Private/Operations/InsetMeshRegion.cpp (see the
// header for the line ranges and the adaptations).
#include "Engine/Geometry/UECore/DynamicMesh/Operations/InsetMeshRegion.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMeshEditor.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/OffsetMeshRegion.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/PolyEditingEdgeUtil.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace Desert::Geometry
{
    bool InsetMeshRegion::Apply()
    {
        std::vector<std::vector<int32_t>> Components;
        FindConnectedTriangleComponents( *m_Mesh, m_Triangles, Components );
        bool bAllOK = true;
        m_InsetRegions.resize( static_cast<int32_t>( Components.size() ) );
        for ( int32_t k = 0; k < static_cast<int32_t>( Components.size() ); ++k )
        {
            InsetInfo& Region       = m_InsetRegions[k];
            Region.InitialTriangles = Components[k];
            if ( !ApplyInset( Region ) )
            {
                m_FailureReason = fmt::format( "region {} of {}: {}", k, static_cast<int32_t>( Components.size() ),
                                               m_FailureReason );
                bAllOK        = false;
                continue;
            }
            m_AllModifiedTriangles.insert( m_AllModifiedTriangles.end(), Region.InitialTriangles.begin(),
                                           Region.InitialTriangles.end() );
            for ( const std::vector<int32_t>& RegionTris : Region.StitchTriangles )
                m_AllModifiedTriangles.insert( m_AllModifiedTriangles.end(), RegionTris.begin(),
                                               RegionTris.end() );
        }
        return bAllOK;
    }

    bool InsetMeshRegion::ApplyInset( InsetInfo& Region )
    {
        // UE solves interior vertices with a Laplacian deformer; that solver is not ported, so such a region is
        // refused here, before the mesh is touched.
        {
            MeshRegionBoundaryLoops Loops( m_Mesh, Region.InitialTriangles, false );
            if ( !Loops.Compute() )
            {
                m_FailureReason = Loops.m_FailureReason;
                return false;
            }
            std::unordered_set<int32_t> LoopVertices;
            for ( const EdgeLoop& Loop : Loops.m_Loops )
                for ( int32_t const v : Loop.Vertices )
                    LoopVertices.insert( v );
            for ( int32_t const tid : Region.InitialTriangles )
            {
                const Index3i Tri = m_Mesh->GetTriangle( tid );
                for ( int j = 0; j < 3; ++j )
                    if ( !LoopVertices.contains( Tri[j] ) )
                    {
                        m_FailureReason =
                             fmt::format( "vertex {} is inside the region ({} triangles): an inset of a "
                                          "region with interior vertices needs UE's interior solve, "
                                          "which is not ported",
                                          Tri[j], static_cast<int32_t>( Region.InitialTriangles.size() ) );
                        return false;
                    }
            }
        }

        DynamicMeshEditor                           Editor( m_Mesh );
        std::vector<DynamicMeshEditor::LoopPairSet> LoopPairs;
        if ( !Editor.DisconnectTriangles( Region.InitialTriangles, LoopPairs, true, m_FailureReason ) )
            return false;

        std::vector<std::vector<TriVidPair>> InsetStitchSides;
        InsetStitchSides.resize( static_cast<int32_t>( LoopPairs.size() ) );
        for ( int32_t i = 0; i < static_cast<int32_t>( LoopPairs.size() ); ++i )
            DynamicMeshEditor::ConvertLoopToTriVidPairSequence( *m_Mesh, LoopPairs[i].InnerVertices,
                                                                LoopPairs[i].InnerEdges, InsetStitchSides[i] );

        Region.InsetLoops.clear();
        for ( const DynamicMeshEditor::LoopPairSet& LoopPair : LoopPairs )
        {
            const std::vector<int32_t>& LoopVids = LoopPair.InnerVertices;
            std::vector<Line3d>         InsetLines;
            ComputeInsetLineSegmentsFromEdges( *m_Mesh, LoopPair.InnerEdges, m_InsetDistance, InsetLines );
            std::vector<glm::dvec3> NewPositions;
            SolveInsetVertexPositionsFromInsetLines( *m_Mesh, InsetLines, LoopVids, NewPositions, true );
            const int32_t N = static_cast<int32_t>( LoopVids.size() );
            for ( int32_t k = 0; k < N; ++k )
                m_Mesh->SetVertex( LoopVids[k], NewPositions[k] );
            Region.InsetLoops.emplace_back();
            Region.InsetLoops.back().Vertices = LoopVids;
            Region.InsetLoops.back().Edges    = LoopPair.InnerEdges;
        }

        const int32_t NumInitialLoops = static_cast<int32_t>( LoopPairs.size() );
        Region.BaseLoops.resize( NumInitialLoops );
        Region.StitchTriangles.resize( NumInitialLoops );
        Region.StitchPolygonIDs.resize( NumInitialLoops );
        std::vector<std::vector<Index2i>> QuadStrips;
        for ( int32_t LoopIndex = 0; LoopIndex < NumInitialLoops; ++LoopIndex )
        {
            const DynamicMeshEditor::LoopPairSet&   LoopPair  = LoopPairs[LoopIndex];
            const std::vector<int32_t>&             BaseLoopV = LoopPair.OuterVertices;
            const int32_t                           NumLoopV  = static_cast<int32_t>( BaseLoopV.size() );
            std::vector<int32_t>                    NewGroupIDs;
            std::vector<int32_t>                    EdgeGroups;
            std::unordered_map<int64_t, int32_t>    NewGroupsMap; // (min, max) group pair packed
            for ( int32_t k = 0; k < NumLoopV; ++k )
            {
                int32_t const InsetGroupID = m_Mesh->GetTriangleGroup( InsetStitchSides[LoopIndex][k].first );
                int32_t const BaseEdgeID   = m_Mesh->FindEdge( BaseLoopV[k], BaseLoopV[( k + 1 ) % NumLoopV] );
                int32_t const BaseGroupID  = ( BaseEdgeID >= 0 )
                                                  ? m_Mesh->GetTriangleGroup( m_Mesh->GetEdgeT( BaseEdgeID ).A )
                                                  : InsetGroupID;
                const int64_t GroupPair = ( int64_t( std::min( BaseGroupID, InsetGroupID ) ) << 32 ) |
                                          uint32_t( std::max( BaseGroupID, InsetGroupID ) );
                if ( !NewGroupsMap.contains( GroupPair ) )
                {
                    int32_t const NewGroupID = m_Mesh->AllocateTriangleGroup();
                    NewGroupIDs.push_back( NewGroupID );
                    NewGroupsMap.insert_or_assign( GroupPair, NewGroupID );
                }
                EdgeGroups.push_back( NewGroupsMap[GroupPair] );
            }
            DynamicMeshEditResult StitchResult;
            if ( !Editor.StitchVertexLoopToTriVidPairSequence( InsetStitchSides[LoopIndex], BaseLoopV,
                                                               StitchResult ) )
            {
                m_FailureReason =
                     fmt::format( "loop {} ({} vertices) could not be stitched", LoopIndex, NumLoopV );
                return false;
            }
            for ( int32_t k = 0; k < static_cast<int32_t>( StitchResult.NewQuads.size() ); k++ )
            {
                m_Mesh->SetTriangleGroup( StitchResult.NewQuads[k].A, EdgeGroups[k] );
                m_Mesh->SetTriangleGroup( StitchResult.NewQuads[k].B, EdgeGroups[k] );
            }
            StitchResult.GetAllTriangles( Region.StitchTriangles[LoopIndex] );
            Region.StitchPolygonIDs[LoopIndex] = NewGroupIDs;
            QuadStrips.push_back( StitchResult.NewQuads );
            Region.BaseLoops[LoopIndex].Vertices = BaseLoopV;
            VertexLoopToEdgeLoop( *m_Mesh, BaseLoopV, Region.BaseLoops[LoopIndex].Edges );
        }

        if ( m_Mesh->HasAttributes() )
            for ( int32_t StripIndex = 0; StripIndex < static_cast<int32_t>( QuadStrips.size() ); ++StripIndex )
            {
                const std::vector<int32_t>& BaseLoopV          = LoopPairs[StripIndex].OuterVertices;
                float                AccumUVTranslation = 0;
                glm::dvec3                  FirstAxisX{}, FrameUp{};
                for ( int32_t k = 0; k < static_cast<int32_t>( QuadStrips[StripIndex].size() ); k++ )
                {
                    const glm::vec3  NF = Editor.ComputeAndSetQuadNormal( QuadStrips[StripIndex][k], true );
                    const glm::dvec3 Normal( NF.x, NF.y, NF.z );
                    glm::dvec3       AxisX{}, AxisY{};
                    if ( k == 0 )
                    {
                        // Frame3d(0, Normal).ConstrainedAlignAxis(0, FirstEdge, Normal): X is the first edge
                        // in the quad's plane, Y = Z x X.
                        glm::dvec3 FirstEdge =
                             m_Mesh->GetVertex( BaseLoopV[1] ) - m_Mesh->GetVertex( BaseLoopV[0] );
                        AxisX                = Normalized( FirstEdge - Normal * glm::dot( FirstEdge, Normal ) );
                        AxisY                = glm::cross( Normal, AxisX );
                        FrameUp             = AxisY;
                    }
                    else
                    {
                        // ConstrainedAlignAxis(2, Normal, FrameUp): rotate about FrameUp until Z meets Normal.
                        glm::dvec3 Z = Normalized( Normal - FrameUp * glm::dot( Normal, FrameUp ) );
                        AxisY       = FrameUp;
                        AxisX        = glm::cross( AxisY, Z );
                    }
                    if ( k > 0 )
                        AccumUVTranslation += (float)Distance( m_Mesh->GetVertex( BaseLoopV[k] ),
                                                               m_Mesh->GetVertex( BaseLoopV[k - 1] ) );
                    Editor.SetQuadUVsFromProjection( QuadStrips[StripIndex][k], AxisX, AxisY, m_UVScaleFactor,
                                                     glm::vec2( m_UVScaleFactor * AccumUVTranslation, 0.0f ) );
                }
            }
        return true;
    }
} // namespace Desert::Geometry
