#include "MeshElementOperation.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/MergeCoincidentMeshEdges.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/MeshBevel.hpp"
#include "Engine/Geometry/MeshCore/DynamicMeshEditor.hpp"
#include "Engine/Geometry/MeshHoleFillOperation.hpp"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <limits>
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
            const GroupTopology topology = PolyEditTopology( mesh );
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
        const GroupTopology topology = PolyEditTopology( before );
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

    // ── Disconnect ─────────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RegionOutcome> DisconnectSelection( const DynamicMesh3&     before,
                                                          const ElementSelection& selection )
    {
        const GroupTopology topology = PolyEditTopology( before );
        auto                region = RegionTriangles( before, topology, selection, "Disconnect" );
        if ( !region.IsSuccess() )
            return Common::MakeError<RegionOutcome>( region.GetError() );
        if ( static_cast<int32_t>( region.GetValue().size() ) == before.TriangleCount() )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Disconnect: all {} triangles are selected - nothing is left to disconnect them from",
                 before.TriangleCount() );
        auto                     mesh = std::make_shared<DynamicMesh3>( before );
        std::vector<DynamicMeshEditor::LoopPairSet> loops;
        std::string              failure;
        if ( !DynamicMeshEditor( mesh.get() ).DisconnectTriangles( region.GetValue(), loops, true, failure ) )
            return Common::MakeFormattedError<RegionOutcome>( "Mesh Disconnect: {}", failure );
        int32_t openEdges = 0;
        for ( const int32_t e : mesh->EdgeIndicesItr() )
            if ( mesh->IsBoundaryEdge( e ) )
                ++openEdges;
        return Finish( std::move( mesh ), "Disconnect", selection,
                       fmt::format( "{} triangles disconnected, {} open edges", region.GetValue().size(),
                                    openEdges ) );
    }

    // ── Offset ─────────────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RegionOutcome> OffsetSelection( const DynamicMesh3&     before,
                                                      const ElementSelection& selection, float distance )
    {
        if ( distance == 0.0f || !std::isfinite( distance ) )
            return Common::MakeFormattedError<RegionOutcome>( "Mesh Offset: distance {} cm changes nothing",
                                                              distance );
        const GroupTopology topology = PolyEditTopology( before );
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
        const GroupTopology  topology = PolyEditTopology( before );
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
        const GroupTopology topology = PolyEditTopology( before );
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
    // ── Topology verbs: Bridge, Collapse, Extrude Edge, Flip, Retriangulate (UE EditMeshPolygonsTool) ───────

    namespace
    {
        // The region's edge-connected components (triangles sharing an edge), each sorted.
        std::vector<std::vector<int32_t>> RegionComponents( const DynamicMesh3& mesh, const std::vector<int32_t>& region )
        {
            const std::unordered_set<int32_t> inRegion( region.begin(), region.end() );
            std::unordered_set<int32_t>       seen;
            std::vector<std::vector<int32_t>> components;
            for ( const int32_t seed : region )
            {
                if ( !seen.insert( seed ).second )
                    continue;
                std::vector<int32_t> component{ seed };
                for ( size_t k = 0; k < component.size(); ++k )
                {
                    const Index3i te = mesh.GetTriEdges( component[k] );
                    for ( int j = 0; j < 3; ++j )
                    {
                        const Index2i et = mesh.GetEdgeT( te[j] );
                        for ( const int32_t other : { et.A, et.B } )
                            if ( other >= 0 && inRegion.contains( other ) && seen.insert( other ).second )
                                component.push_back( other );
                    }
                }
                std::sort( component.begin(), component.end() );
                components.push_back( std::move( component ) );
            }
            return components;
        }

        // The border loops of a triangle set, each walked the way the set's own triangles wind it (a -> b where
        // a triangle of the set has the edge a -> b and the triangle across it, if any, is not in the set).
        Common::ResultStr<std::vector<std::vector<int>>> RegionLoops( const DynamicMesh3&         mesh,
                                                                       const std::vector<int32_t>& region,
                                                                       const char*                 name )
        {
            const std::unordered_set<int32_t> inRegion( region.begin(), region.end() );
            std::unordered_map<int, int>      next;
            for ( const int32_t t : region )
            {
                const Index3i tv = mesh.GetTriangle( t );
                const Index3i te = mesh.GetTriEdges( t );
                for ( int j = 0; j < 3; ++j )
                {
                    const Index2i et    = mesh.GetEdgeT( te[j] );
                    const int32_t other = et.A == t ? et.B : et.A;
                    if ( other >= 0 && inRegion.contains( other ) )
                        continue;
                    if ( !next.emplace( tv[j], tv[( j + 1 ) % 3] ).second )
                        return Common::MakeFormattedError<std::vector<std::vector<int>>>(
                             "Mesh {}: the selected faces touch themselves at vertex {} (a bow-tie border)", name,
                             tv[j] );
                }
            }
            std::vector<std::vector<int>> loops;
            std::unordered_set<int>       used;
            for ( const auto& [start, unused] : next )
            {
                if ( used.contains( start ) )
                    continue;
                std::vector<int> loop;
                for ( int v = start; !used.contains( v ); )
                {
                    used.insert( v );
                    loop.push_back( v );
                    const auto it = next.find( v );
                    if ( it == next.end() )
                        return Common::MakeFormattedError<std::vector<std::vector<int>>>(
                             "Mesh {}: the selection's border is not closed at vertex {}", name, v );
                    v = it->second;
                }
                loops.push_back( std::move( loop ) );
            }
            return Common::MakeSuccess( std::move( loops ) );
        }

        // 1 / (2 x the largest half-extent): the UV density Hole Fill projects its fills at.
        float ProjectionScale( const DynamicMesh3& mesh )
        {
            const glm::dvec3 extents = mesh.GetBounds().Extents();
            return static_cast<float>( 1.0 / ( 2.0 * std::max( { extents.x, extents.y, extents.z, 1e-9 } ) ) );
        }

        // Planar normal and a projection on the quad's own frame for each new quad (UE's AddQuad pattern).
        void SetQuadAttributes( DynamicMesh3& mesh, const std::vector<Index2i>& quads, float uvScale )
        {
            if ( !mesh.HasAttributes() )
                return;
            const DynamicMeshEditor editor( &mesh );
            for ( const Index2i& quad : quads )
            {
                const glm::vec3  normal = editor.ComputeAndSetQuadNormal( quad, true );
                const Index3i    tv     = mesh.GetTriangle( quad.A );
                const glm::dvec3 axisX  = glm::normalize( mesh.GetVertex( tv.B ) - mesh.GetVertex( tv.A ) );
                const glm::dvec3 axisY  = glm::normalize( glm::cross( glm::dvec3( normal ), axisX ) );
                editor.SetQuadUVsFromProjection( quad, axisX, axisY, uvScale, glm::vec2( 0.0f ) );
            }
        }
    } // namespace

    // ── Bridge ─────────────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RegionOutcome> BridgeSelection( const DynamicMesh3& before, const ElementSelection& selection )
    {
        const GroupTopology topology = PolyEditTopology( before );
        auto                region   = RegionTriangles( before, topology, selection, "Bridge" );
        if ( !region.IsSuccess() )
            return Common::MakeError<RegionOutcome>( region.GetError() );
        const auto components = RegionComponents( before, region.GetValue() );
        if ( components.size() != 2 )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Bridge: the selection is {} connected face region(s) - select exactly two (Shift-click the "
                 "second face)",
                 components.size() );
        std::vector<int> loops[2];
        for ( int r = 0; r < 2; ++r )
        {
            auto found = RegionLoops( before, components[r], "Bridge" );
            if ( !found.IsSuccess() )
                return Common::MakeError<RegionOutcome>( found.GetError() );
            if ( found.GetValue().size() != 1 )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Bridge: face region {} ({} triangles) has {} border loops - each side must be bounded "
                     "by one loop",
                     r + 1, components[r].size(), found.GetValue().size() );
            loops[r] = found.GetValue().front();
        }
        const size_t n = loops[0].size();
        if ( loops[1].size() != n )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Bridge: the two borders have {} and {} vertices - a bridge pairs them one to one", n,
                 loops[1].size() );
        for ( const int v : loops[0] )
            if ( std::find( loops[1].begin(), loops[1].end(), v ) != loops[1].end() )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Bridge: the two regions share vertex {} - they must be apart", v );
        // Loop1 winds as the triangles left around the first hole do (against the removed region); Loop2 as the
        // second region did, so its edge d -> c is wound by the triangle left beside it (UE's stitch contract).
        std::vector<int> loop1( loops[0].rbegin(), loops[0].rend() );
        size_t           bestShift = 0;
        double           bestCost  = std::numeric_limits<double>::max();
        for ( size_t k = 0; k < n; ++k )
        {
            double cost = 0.0;
            for ( size_t i = 0; i < n; ++i )
            {
                const glm::dvec3 d = before.GetVertex( loop1[i] ) - before.GetVertex( loops[1][( i + k ) % n] );
                cost += glm::dot( d, d );
            }
            if ( cost < bestCost )
            {
                bestCost  = cost;
                bestShift = k;
            }
        }
        std::vector<int> loop2( n );
        for ( size_t i = 0; i < n; ++i )
            loop2[i] = loops[1][( i + bestShift ) % n];

        auto                 mesh = std::make_shared<DynamicMesh3>( before );
        std::vector<int32_t> removed = components[0];
        removed.insert( removed.end(), components[1].begin(), components[1].end() );
        if ( !DynamicMeshEditor( mesh.get() ).RemoveTriangles( removed, true ) )
            return Common::MakeError<RegionOutcome>( "Mesh Bridge: the two faces could not be removed" );
        const int            group = mesh->HasTriangleGroups() ? mesh->AllocateTriangleGroup() : 0;
        std::vector<int32_t> strip;
        std::vector<Index2i> quads;
        for ( size_t i = 0; i < n; ++i )
        {
            const int a = loop1[i], b = loop1[( i + 1 ) % n], c = loop2[i], d = loop2[( i + 1 ) % n];
            const int t1 = mesh->AppendTriangle( Index3i( b, a, d ), group );
            const int t2 = t1 >= 0 ? mesh->AppendTriangle( Index3i( a, c, d ), group ) : -1;
            if ( t1 < 0 || t2 < 0 )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Bridge: quad {} of {} ({}, {}, {}, {}) could not be added (an edge would get a third "
                     "triangle)",
                     i, n, a, b, c, d );
            strip.push_back( t1 );
            strip.push_back( t2 );
            quads.emplace_back( t1, t2 );
        }
        SetQuadAttributes( *mesh, quads, ProjectionScale( *mesh ) );
        auto selected = SelectionOf( *mesh, strip, selection.Mode(), "Bridge" );
        if ( !selected.IsSuccess() )
            return Common::MakeError<RegionOutcome>( selected.GetError() );
        return Finish( std::move( mesh ), "Bridge", selected.ExtractValue(),
                       fmt::format( "{} + {} triangles removed, {} quads bridge the {}-vertex loops",
                                    components[0].size(), components[1].size(), n, n ) );
    }

    // ── Collapse ───────────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RegionOutcome> CollapseSelection( const DynamicMesh3& before, const ElementSelection& selection )
    {
        if ( selection.Mode() != ElementMode::Edge )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Collapse: collapses edges - switch the selection to Edge mode ({} is selected)",
                 ToString( selection.Mode() ) );
        if ( selection.Empty() )
            return Common::MakeError<RegionOutcome>( "Mesh Collapse: nothing is selected" );
        auto                                  mesh = std::make_shared<DynamicMesh3>( before );
        const DynamicMesh3::CollapseEdgeOptions options{};
        int                                   collapsed = 0, refused = 0, gone = 0;
        int                                   firstRefused = -1;
        for ( const int e : selection.Ids() )
        {
            if ( !mesh->IsEdge( e ) )
            {
                ++gone; // removed by an earlier collapse of the same selection
                continue;
            }
            const Index2i    ev = mesh->GetEdgeV( e );
            EdgeCollapseInfo info{};
            if ( mesh->CollapseEdge( ev.A, ev.B, 0.5, options, info ) == MeshResult::Ok )
                ++collapsed;
            else if ( refused++ == 0 )
                firstRefused = e;
        }
        if ( collapsed == 0 )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Collapse: none of the {} edges can be collapsed (edge {} first): it would close a hole, "
                 "fold a tetrahedron or leave a non-manifold edge",
                 selection.Size(), firstRefused );
        if ( mesh->HasAttributes() && mesh->Attributes()->PrimaryNormals() != nullptr )
            MeshNormals::QuickRecomputeOverlayNormals( *mesh );
        return Finish( std::move( mesh ), "Collapse", ElementSelection( ElementMode::Edge ),
                       fmt::format( "{} edges collapsed to their midpoints, {} refused, {} gone with an earlier one",
                                    collapsed, refused, gone ) );
    }

    // ── Extrude Edge ───────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RegionOutcome> ExtrudeEdgeSelection( const DynamicMesh3& before, const ElementSelection& selection,
                                                           float distance )
    {
        if ( selection.Mode() != ElementMode::Edge )
            return Common::MakeFormattedError<RegionOutcome>(
                 "Mesh Extrude Edge: extrudes open edges - switch the selection to Edge mode ({} is selected)",
                 ToString( selection.Mode() ) );
        if ( selection.Empty() )
            return Common::MakeError<RegionOutcome>( "Mesh Extrude Edge: nothing is selected" );
        if ( !( std::abs( distance ) > 0.0f ) )
            return Common::MakeError<RegionOutcome>( "Mesh Extrude Edge: the distance is 0 cm - nothing to extrude" );
        // Each edge as its triangle winds it (a -> b), with its outward direction in that triangle's plane.
        struct OpenEdge
        {
            int        A = -1, B = -1;
        };
        std::vector<OpenEdge>                   edges;
        std::unordered_map<int, glm::dvec3>     direction;
        for ( const int e : selection.Ids() )
        {
            if ( !before.IsEdge( e ) )
                continue;
            if ( !before.IsBoundaryEdge( e ) )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Extrude Edge: edge {} is closed (two faces meet on it) - only an open border edge can be "
                     "extruded; Delete or Disconnect a face beside it first",
                     e );
            const int     t  = before.GetEdgeT( e ).A;
            const Index3i tv = before.GetTriangle( t );
            const Index2i ev = before.GetEdgeV( e );
            OpenEdge      edge;
            for ( int j = 0; j < 3; ++j )
                if ( ( tv[j] == ev.A || tv[j] == ev.B ) && ( tv[( j + 1 ) % 3] == ev.A || tv[( j + 1 ) % 3] == ev.B ) )
                    edge = OpenEdge{ tv[j], tv[( j + 1 ) % 3] };
            const glm::dvec3 out = glm::cross( before.GetVertex( edge.B ) - before.GetVertex( edge.A ),
                                               before.GetTriNormal( t ) );
            const double     len = glm::length( out );
            if ( !( len > 1e-12 ) )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Extrude Edge: edge {} has zero length or a degenerate triangle ({})", e, t );
            direction[edge.A] += out / len;
            direction[edge.B] += out / len;
            edges.push_back( edge );
        }
        if ( edges.empty() )
            return Common::MakeError<RegionOutcome>( "Mesh Extrude Edge: none of the selected edges exists" );
        auto                         mesh = std::make_shared<DynamicMesh3>( before );
        std::unordered_map<int, int> moved;
        for ( const auto& [v, dir] : direction )
        {
            const double len = glm::length( dir );
            if ( !( len > 1e-12 ) )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Extrude Edge: the edges through vertex {} point opposite ways - no direction to extrude", v );
            moved[v] = mesh->AppendVertex( before.GetVertex( v ) + dir / len * static_cast<double>( distance ) );
        }
        const int            group = mesh->HasTriangleGroups() ? mesh->AllocateTriangleGroup() : 0;
        std::vector<int32_t> strip;
        std::vector<Index2i> quads;
        for ( const OpenEdge& edge : edges )
        {
            const int a = edge.A, b = edge.B, a2 = moved[a], b2 = moved[b];
            const int t1 = mesh->AppendTriangle( Index3i( b, a, a2 ), group );
            const int t2 = t1 >= 0 ? mesh->AppendTriangle( Index3i( b, a2, b2 ), group ) : -1;
            if ( t1 < 0 || t2 < 0 )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Extrude Edge: the quad on edge ({}, {}) could not be added", a, b );
            strip.push_back( t1 );
            strip.push_back( t2 );
            quads.emplace_back( t1, t2 );
        }
        SetQuadAttributes( *mesh, quads, ProjectionScale( *mesh ) );
        auto selected = SelectionOf( *mesh, strip, ElementMode::Edge, "Extrude Edge" );
        if ( !selected.IsSuccess() )
            return Common::MakeError<RegionOutcome>( selected.GetError() );
        return Finish( std::move( mesh ), "Extrude Edge", selected.ExtractValue(),
                       fmt::format( "{} open edges extruded {} cm, {} new vertices", edges.size(), distance,
                                    moved.size() ) );
    }

    // ── Flip ───────────────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RegionOutcome> FlipSelection( const DynamicMesh3& before, const ElementSelection& selection )
    {
        const GroupTopology topology = PolyEditTopology( before );
        auto                region   = RegionTriangles( before, topology, selection, "Flip" );
        if ( !region.IsSuccess() )
            return Common::MakeError<RegionOutcome>( region.GetError() );
        auto mesh = std::make_shared<DynamicMesh3>( before );
        DynamicMeshEditor( mesh.get() ).ReverseTriangleOrientations( region.GetValue(), true );
        return Finish( std::move( mesh ), "Flip", selection,
                       fmt::format( "{} triangles reversed", region.GetValue().size() ) );
    }

    // ── Retriangulate ──────────────────────────────────────────────────────────────────────────────────

    Common::ResultStr<RegionOutcome> RetriangulateSelection( const DynamicMesh3&     before,
                                                             const ElementSelection& selection )
    {
        const GroupTopology topology = PolyEditTopology( before );
        auto                region   = RegionTriangles( before, topology, selection, "Retriangulate" );
        if ( !region.IsSuccess() )
            return Common::MakeError<RegionOutcome>( region.GetError() );
        std::set<int> groups;
        for ( const int32_t t : region.GetValue() )
            groups.insert( before.GetTriangleGroup( t ) );
        DynamicMesh3         mesh( before );
        std::vector<int32_t> refilled;
        int                  removedTriangles = 0;
        for ( const int g : groups )
        {
            std::vector<int32_t> triangles;
            for ( const int t : mesh.TriangleIndicesItr() )
                if ( mesh.GetTriangleGroup( t ) == g )
                    triangles.push_back( t );
            auto loops = RegionLoops( mesh, triangles, "Retriangulate" );
            if ( !loops.IsSuccess() )
                return Common::MakeError<RegionOutcome>( loops.GetError() );
            if ( loops.GetValue().size() != 1 )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Retriangulate: polygroup {} has {} border loops - only a group bounded by one loop is "
                     "retriangulated (UE refuses a ring too)",
                     g, loops.GetValue().size() );
            if ( static_cast<int32_t>( triangles.size() ) == mesh.TriangleCount() )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Retriangulate: polygroup {} is the whole mesh - nothing would bound the refill", g );
            std::set<int> loopVertices( loops.GetValue().front().begin(), loops.GetValue().front().end() );
            if ( !DynamicMeshEditor( &mesh ).RemoveTriangles( triangles, true ) )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Retriangulate: polygroup {} could not be removed", g );
            removedTriangles += static_cast<int>( triangles.size() );
            const auto holes = HoleBoundaryLoops( mesh );
            int32_t    hole  = -1;
            for ( size_t h = 0; h < holes.size(); ++h )
                if ( std::set<int>( holes[h].begin(), holes[h].end() ) == loopVertices )
                    hole = static_cast<int32_t>( h );
            if ( hole < 0 )
                return Common::MakeFormattedError<RegionOutcome>(
                     "Mesh Retriangulate: polygroup {}'s border is not one open loop once it is removed", g );
            std::unordered_set<int> groupsLeft;
            for ( const int t : mesh.TriangleIndicesItr() )
                groupsLeft.insert( mesh.GetTriangleGroup( t ) );
            HoleFillSettings settings;
            settings.Type = HoleFillType::Minimal;
            auto filled   = HoleFill( mesh, { hole }, settings );
            if ( !filled.IsSuccess() )
                return Common::MakeFormattedError<RegionOutcome>( "Mesh Retriangulate: polygroup {}: {}", g,
                                                                  filled.GetError() );
            mesh = std::move( filled.ExtractValue().Mesh );
            for ( const int t : mesh.TriangleIndicesItr() )
                if ( !groupsLeft.contains( mesh.GetTriangleGroup( t ) ) )
                {
                    mesh.SetTriangleGroup( t, g );
                    refilled.push_back( t );
                }
        }
        auto result   = std::make_shared<DynamicMesh3>( std::move( mesh ) );
        auto selected = SelectionOf( *result, refilled, selection.Mode(), "Retriangulate" );
        if ( !selected.IsSuccess() )
            return Common::MakeError<RegionOutcome>( selected.GetError() );
        return Finish( std::move( result ), "Retriangulate", selected.ExtractValue(),
                       fmt::format( "{} polygroups: {} triangles replaced by {}", groups.size(), removedTriangles,
                                    refilled.size() ) );
    }
} // namespace Desert::Geometry
