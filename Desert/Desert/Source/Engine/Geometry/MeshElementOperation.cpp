#include "MeshElementOperation.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/MergeCoincidentMeshEdges.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/MeshBevel.hpp"
#include "Engine/Geometry/MeshCore/DynamicMeshEditor.hpp"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Geometry
{
    namespace
    {
        // The selection's whole triangles, or a refusal naming the verb, the mode and the count.
        Common::ResultStr<std::vector<int32_t>> RegionTriangles( const DynamicMesh3&     mesh,
                                                                 const GroupTopology&    topology,
                                                                 const ElementSelection& selection,
                                                                 const char*             name )
        {
            if ( selection.Empty() )
                return Common::MakeFormattedError<std::vector<int32_t>>( "Mesh {}: nothing is selected", name );
            const ElementSelection triangles =
                 ConvertSelection( mesh, topology, selection, ElementMode::Triangle );
            if ( triangles.Empty() )
                return Common::MakeFormattedError<std::vector<int32_t>>(
                     "Mesh {}: the {} selected {} cover no whole triangle", name, selection.Size(),
                     ToString( selection.Mode() ) );
            return Common::MakeSuccess( std::vector<int32_t>( triangles.Ids().begin(), triangles.Ids().end() ) );
        }

        // @p triangles of @p mesh as a selection in @p mode.
        Common::ResultStr<ElementSelection> SelectionOf( const DynamicMesh3&         mesh,
                                                         const std::vector<int32_t>& triangles, ElementMode mode,
                                                         const char* name )
        {
            ElementSelection result( ElementMode::Triangle );
            for ( const int32_t t : triangles )
                if ( mesh.IsTriangle( t ) )
                    if ( auto added = result.Add( mesh, t ); !added.IsSuccess() )
                        return Common::MakeFormattedError<ElementSelection>( "Mesh {}: result triangle {}: {}",
                                                                             name, t, added.GetError() );
            if ( mode == ElementMode::Triangle )
                return Common::MakeSuccess( std::move( result ) );
            const GroupTopology topology( &mesh, true );
            return Common::MakeSuccess( ConvertSelection( mesh, topology, result, mode ) );
        }

        Common::ResultStr<RegionOutcome> Finish( std::shared_ptr<DynamicMesh3> mesh, const char* name,
                                                 ElementSelection selection, std::string report = {} )
        {
            if ( auto tangents = RecomputeTangentSpace( *mesh, name ); !tangents.IsSuccess() )
                return Common::MakeError<RegionOutcome>( tangents.GetError() );
            return Common::MakeSuccess(
                 RegionOutcome{ std::move( mesh ), std::move( selection ), std::move( report ) } );
        }
    } // namespace

    Common::ResultStr<CutPlane> CutPlaneFromScreenLine( const glm::mat4& modelViewProj, const glm::vec2& ndcA,
                                                        const glm::vec2& ndcB )
    {
        if ( glm::length( ndcB - ndcA ) <= 1e-6f )
            return Common::MakeFormattedError<CutPlane>(
                 "Cut: the line's two points coincide at ({:.4f}, {:.4f}) - drag a line across the view", ndcA.x,
                 ndcA.y );
        const float det = glm::determinant( modelViewProj );
        if ( !std::isfinite( det ) || std::abs( det ) <= 1e-20f )
            return Common::MakeFormattedError<CutPlane>( "Cut: the view matrix is singular (determinant {})",
                                                         det );
        const glm::mat4 inverse   = glm::inverse( modelViewProj );
        auto            unproject = [&]( const glm::vec2& p, float depth )
        {
            const glm::vec4 h = inverse * glm::vec4( p, depth, 1.0f );
            return glm::vec3( h ) / h.w;
        };
        // Depths 0.25 / 0.75 rather than the clip planes: both are inside every depth convention's range and
        // clear of a far plane at infinity.
        const glm::vec3 a0 = unproject( ndcA, 0.25f );
        const glm::vec3 a1 = unproject( ndcA, 0.75f );
        const glm::vec3 b0 = unproject( ndcB, 0.25f );
        const glm::vec3 n  = glm::cross( a1 - a0, b0 - a0 );
        if ( glm::length( n ) <= 0.0f || !std::isfinite( n.x + n.y + n.z ) )
            return Common::MakeError<CutPlane>( "Cut: the line and the view direction span no plane" );
        return Common::MakeSuccess( CutPlane{ a0, glm::normalize( n ) } );
    }

    // ── Delete ─────────────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RegionOutcome> DeleteSelection( const DynamicMesh3&     before,
                                                      const ElementSelection& selection )
    {
        const GroupTopology topology( &before, true );
        auto                region = RegionTriangles( before, topology, selection, "Delete" );
        if ( !region.IsSuccess() )
            return Common::MakeError<RegionOutcome>( region.GetError() );
        auto mesh = std::make_shared<DynamicMesh3>( before );
        if ( !DynamicMeshEditor( mesh.get() ).RemoveTriangles( region.GetValue(), true ) )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Delete: {} of the {} triangles could not be "
                 "removed",
                 region.GetValue().size() - static_cast<size_t>( before.TriangleCount() - mesh->TriangleCount() ),
                 region.GetValue().size() );
        return Finish( std::move( mesh ), "Delete", ElementSelection( selection.Mode() ) );
    }

    // ── Offset ─────────────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RegionOutcome> OffsetSelection( const DynamicMesh3&     before,
                                                      const ElementSelection& selection, float distance )
    {
        if ( distance == 0.0f || !std::isfinite( distance ) )
            return Common::MakeFormattedError<RegionOutcome>( "Mesh Offset: distance {} cm changes nothing",
                                                              distance );
        const GroupTopology topology( &before, true );
        auto                region = RegionTriangles( before, topology, selection, "Offset" );
        if ( !region.IsSuccess() )
            return Common::MakeError<RegionOutcome>( region.GetError() );
        std::set<int> vertices;
        for ( const int32_t t : region.GetValue() )
        {
            const Index3i tv = before.GetTriangle( t );
            vertices.insert( { tv.A, tv.B, tv.C } );
        }
        auto mesh = std::make_shared<DynamicMesh3>( before );
        for ( const int v : vertices )
            mesh->SetVertex( v, before.GetVertex( v ) + MeshNormals::ComputeVertexNormal( before, v ) *
                                                             static_cast<double>( distance ) );
        std::set<int32_t> touched;
        for ( const int v : vertices )
            for ( const int t : before.VtxTrianglesItr( v ) )
                touched.insert( t );
        for ( const int32_t t : touched )
            if ( glm::dot( before.GetTriNormal( t ), mesh->GetTriNormal( t ) ) < 0.0 )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Offset: triangle {} flips over at {} cm - the offset folds the surface", t, distance );
        if ( mesh->HasAttributes() && mesh->Attributes()->PrimaryNormals() != nullptr )
            MeshNormals::RecomputeOverlayTriNormals( *mesh,
                                                     std::vector<int32_t>( touched.begin(), touched.end() ) );
        return Finish( std::move( mesh ), "Offset", selection );
    }

    // ── Bevel ──────────────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RegionOutcome> BevelSelection( const DynamicMesh3& before, const ElementSelection& selection,
                                                     float width )
    {
        if ( selection.Mode() == ElementMode::Vertex )
            return Common::MakeError<RegionOutcome>(
                 "Mesh Bevel: works on Edge, Triangle or PolyGroup selections, as UE's PolyEdit bevels edges and "
                 "faces - select the edges to bevel" );
        if ( !( width > 0.0f ) )
            return Common::MakeFormattedError<RegionOutcome>( "Mesh Bevel: the width must be above 0 cm, not {}",
                                                              width );
        if ( selection.Empty() )
            return Common::MakeError<RegionOutcome>( "Mesh Bevel: nothing is selected" );
        const GroupTopology  topology( &before, true );
        std::vector<int32_t> groupEdges;
        if ( selection.Mode() == ElementMode::Edge )
        {
            for ( const int e : selection.Ids() )
            {
                const int groupEdge = topology.FindGroupEdgeID( e );
                if ( groupEdge < 0 )
                    return Common::MakeFormattedError<RegionOutcome>(
                         "Mesh Bevel: edge {} lies inside a polygroup - only edges between two groups bevel", e );
                if ( std::find( groupEdges.begin(), groupEdges.end(), groupEdge ) == groupEdges.end() )
                    groupEdges.push_back( groupEdge );
            }
        }
        else
        {
            auto region = RegionTriangles( before, topology, selection, "Bevel" );
            if ( !region.IsSuccess() )
                return Common::MakeError<RegionOutcome>( region.GetError() );
            std::unordered_set<int> groups;
            for ( const int32_t t : region.GetValue() )
                groups.insert( topology.GetGroupID( t ) );
            for ( int i = 0; i < static_cast<int>( topology.m_Edges.size() ); ++i )
            {
                const Index2i g = topology.m_Edges[static_cast<size_t>( i )].Groups;
                if ( groups.contains( g.A ) != groups.contains( g.B ) )
                    groupEdges.push_back( i );
            }
            if ( groupEdges.empty() )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Bevel: the {} selected groups have no border edge to bevel", groups.size() );
        }
        auto      mesh = std::make_shared<DynamicMesh3>( before );
        MeshBevel bevel;
        bevel.m_InsetDistance  = width;
        bevel.m_MaterialIDMode = MeshBevel::MaterialIDMode::InferMaterialID;
        if ( !bevel.InitializeFromGroupTopologyEdges( *mesh, topology, groupEdges ) )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Bevel: the {} group edges cannot be bevelled: {}", groupEdges.size(),
                 bevel.m_FailureReason );
        if ( !bevel.Apply( *mesh ) )
            return Common::MakeFormattedError<RegionOutcome>( "Mesh Bevel: {}", bevel.m_FailureReason );
        auto result = SelectionOf( *mesh, bevel.m_NewTriangles, ElementMode::PolyGroup, "Bevel" );
        if ( !result.IsSuccess() )
            return Common::MakeError<RegionOutcome>( result.GetError() );
        return Finish( std::move( mesh ), "Bevel", result.ExtractValue() );
    }

    // ── Cut ────────────────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RegionOutcome> CutSelection( const DynamicMesh3& before, const ElementSelection& selection,
                                                   const CutPlane& plane )
    {
        if ( !before.HasTriangleGroups() )
            return Common::MakeError<RegionOutcome>( "Mesh Cut: the mesh has no polygroups to cut" );
        if ( !( glm::length( plane.Normal ) > 0.0f ) )
            return Common::MakeError<RegionOutcome>( "Mesh Cut: the plane has no normal" );
        const GroupTopology topology( &before, true );
        auto                region = RegionTriangles( before, topology, selection, "Cut" );
        if ( !region.IsSuccess() )
            return Common::MakeError<RegionOutcome>( region.GetError() );
        std::unordered_set<int> groups;
        for ( const int32_t t : region.GetValue() )
            groups.insert( before.GetTriangleGroup( t ) );

        auto             mesh = std::make_shared<DynamicMesh3>( before );
        const glm::dvec3 origin( plane.Point );
        const glm::dvec3 normal     = glm::normalize( glm::dvec3( plane.Normal ) );
        auto             distanceOf = [&]( int v ) { return glm::dot( mesh->GetVertex( v ) - origin, normal ); };
        constexpr double kOnPlane   = 1e-5; // cm

        // Every crossing edge of a selected group's triangle, split where it crosses (MeshPlaneCut's rule: an
        // edge made by a split is never split again).
        const int               maxEdge = mesh->MaxEdgeID();
        std::unordered_set<int> made;
        std::unordered_set<int> crossed;
        for ( int e = 0; e < maxEdge; ++e )
        {
            if ( !mesh->IsEdge( e ) || made.contains( e ) )
                continue;
            const Index2i et  = mesh->GetEdgeT( e );
            const bool    inA = groups.contains( mesh->GetTriangleGroup( et.A ) );
            const bool inB = et.B != DynamicMesh3::InvalidID && groups.contains( mesh->GetTriangleGroup( et.B ) );
            if ( !inA && !inB )
                continue;
            const Index2i ev = mesh->GetEdgeV( e );
            const double  da = distanceOf( ev.A ), db = distanceOf( ev.B );
            if ( std::abs( da ) < kOnPlane || std::abs( db ) < kOnPlane || da * db > 0.0 )
                continue;
            DynamicMeshInfo::EdgeSplitInfo split;
            if ( mesh->SplitEdge( e, split, da / ( da - db ) ) != MeshResult::Ok )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh Cut: edge {} ({}, {}) could not be split",
                                                                  e, ev.A, ev.B );
            for ( const int n : { split.NewEdges.A, split.NewEdges.B, split.NewEdges.C } )
                if ( n != DynamicMesh3::InvalidID )
                    made.insert( n );
            if ( inA )
                crossed.insert( mesh->GetTriangleGroup( et.A ) );
            if ( inB )
                crossed.insert( mesh->GetTriangleGroup( et.B ) );
        }
        if ( crossed.empty() )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Cut: the plane crosses none of the {} selected "
                 "polygroups",
                 groups.size() );

        // The positive side of every crossed group becomes a group of its own, its normals a region of their own.
        std::vector<int32_t>              halves;
        std::vector<std::vector<int32_t>> regions;
        std::unordered_map<int, int>      newGroupOf;
        std::unordered_map<int, size_t>   regionOf;
        for ( const int t : mesh->TriangleIndicesItr() )
        {
            const int group = mesh->GetTriangleGroup( t );
            if ( !crossed.contains( group ) )
                continue;
            const glm::dvec3 centroid = mesh->GetTriCentroid( t );
            const bool       positive = glm::dot( centroid - origin, normal ) > 0.0;
            int              target   = group;
            if ( positive )
            {
                auto [it, fresh] = newGroupOf.try_emplace( group, 0 );
                if ( fresh )
                    it->second = mesh->AllocateTriangleGroup();
                target = it->second;
                mesh->SetTriangleGroup( t, target );
            }
            auto [slot, freshRegion] = regionOf.try_emplace( target, regions.size() );
            if ( freshRegion )
                regions.emplace_back();
            regions[slot->second].push_back( t );
        }
        if ( mesh->HasAttributes() && mesh->Attributes()->PrimaryNormals() != nullptr )
            for ( const std::vector<int32_t>& half : regions )
            {
                MeshNormals::InitializeOverlayRegionToPerVertexNormals( mesh->Attributes()->PrimaryNormals(),
                                                                        half );
                MeshNormals::RecomputeOverlayTriNormals( *mesh, half );
            }
        ElementSelection result( ElementMode::PolyGroup );
        for ( const auto& [group, slot] : regionOf )
            if ( auto added = result.Add( *mesh, group ); !added.IsSuccess() )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh Cut: result group {}: {}", group,
                                                                  added.GetError() );
        return Finish( std::move( mesh ), "Cut", std::move( result ),
                       fmt::format( "{} polygroups cut in two", crossed.size() ) );
    }

    // ── Clean ──────────────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RegionOutcome> CleanMesh( const DynamicMesh3& before, float weldTolerance, ElementMode mode )
    {
        if ( !( weldTolerance >= 0.0f ) )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Clean: the weld tolerance must be >= 0 cm, not {}", weldTolerance );
        auto mesh = std::make_shared<DynamicMesh3>( before );

        // 1. Coincident open edges, as Weld Edges merges them.
        int weldedEdges = 0;
        {
            MergeCoincidentMeshEdges merger( mesh.get() );
            merger.m_MergeVertexTolerance    = std::max( 1e-6, static_cast<double>( weldTolerance ) );
            merger.m_bWeldAttrsOnMergedEdges = true;
            merger.m_SplitAttributeWelder.m_UVDistSqrdThreshold = 0.01f * 0.01f;
            merger.m_SplitAttributeWelder.m_NormalVecDotThreshold =
                 std::abs( 1.f - std::cos( 0.1f * 3.14159265f / 180.f ) );
            merger.m_SplitAttributeWelder.m_TangentVecDotThreshold =
                 merger.m_SplitAttributeWelder.m_NormalVecDotThreshold;
            if ( mesh->TriangleCount() > 0 && merger.Apply() )
                weldedEdges = merger.m_InitialNumBoundaryEdges - merger.m_FinalNumBoundaryEdges;
        }

        // 2. Edges shorter than the tolerance collapse onto their lower vertex ID (which keeps its position).
        int collapsed = 0;
        if ( weldTolerance > 0.0f )
        {
            const double limit = static_cast<double>( weldTolerance ) * weldTolerance;
            for ( bool again = true; again; )
            {
                again = false;
                for ( const int e : mesh->EdgeIndicesItr() )
                {
                    const Index2i ev = mesh->GetEdgeV( e );
                    if ( glm::dot( mesh->GetVertex( ev.A ) - mesh->GetVertex( ev.B ),
                                   mesh->GetVertex( ev.A ) - mesh->GetVertex( ev.B ) ) >= limit )
                        continue;
                    DynamicMeshInfo::EdgeCollapseInfo info;
                    if ( mesh->CollapseEdge( std::min( ev.A, ev.B ), std::max( ev.A, ev.B ), 0.0, info ) ==
                         MeshResult::Ok )
                    {
                        ++collapsed;
                        again = true;
                        break; // the edge iteration is stale after a collapse
                    }
                }
            }
        }

        // 3. Zero-area triangles with distinct corners: flip the longest edge.
        int flipped = 0, kept = 0;
        for ( const int t : mesh->TriangleIndicesItr() )
        {
            if ( mesh->GetTriArea( t ) > 1e-12 )
                continue;
            const Index3i edges   = mesh->GetTriEdges( t );
            int           longest = edges.A;
            double        best    = -1.0;
            for ( int j = 0; j < 3; ++j )
            {
                const Index2i ev     = mesh->GetEdgeV( edges[j] );
                const double  length = glm::length( mesh->GetVertex( ev.A ) - mesh->GetVertex( ev.B ) );
                if ( length > best )
                {
                    best    = length;
                    longest = edges[j];
                }
            }
            DynamicMeshInfo::EdgeFlipInfo info;
            if ( mesh->FlipEdge( longest, info ) == MeshResult::Ok )
                ++flipped;
            else
                ++kept;
        }

        // 4. Vertices with no triangle.
        const int isolated = mesh->RemoveUnusedVertices();

        if ( weldedEdges == 0 && collapsed == 0 && flipped == 0 && isolated == 0 )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Clean: nothing to clean at tolerance {} cm ({} zero-area triangles could not be flipped)",
                 weldTolerance, kept );
        if ( mesh->HasAttributes() && mesh->Attributes()->PrimaryNormals() != nullptr &&
             ( collapsed + flipped ) > 0 )
            MeshNormals::QuickRecomputeOverlayNormals( *mesh );
        return Finish( std::move( mesh ), "Clean", ElementSelection( mode ),
                       fmt::format( "welded {} open edges, collapsed {} short edges, flipped {} zero-area "
                                    "triangles ({} kept), dropped {} isolated vertices",
                                    weldedEdges, collapsed, flipped, kept, isolated ) );
    }
} // namespace Desert::Geometry
