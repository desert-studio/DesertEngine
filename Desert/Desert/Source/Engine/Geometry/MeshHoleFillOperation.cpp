// UE 5.8 MeshModelingTools HoleFillTool / ModelingOperators HoleFillOp.cpp (FHoleFillOp::CalculateResult) on our
// DynamicMesh3: the loops come from MeshBoundaryLoops, each is filled by the chosen filler, then normals, UVs and
// tangents are set on the new triangles. The fillers:
//   Trivial - SimpleHoleFiller (ported from UE, TriangleFan);
//   Minimal - UE MinimalHoleFiller's result (a fill with no interior vertex, edges flipped to the least area),
//             reached by ear-clipping the loop on its Newell plane instead of UE's fan + collapse-all;
//   Smooth  - UE SmoothHoleFiller's pattern (fill, refine the fill towards the loop's edge length, smooth the
//             interior with the loop held); UE also remeshes/smooths a band of the surrounding surface
//             (RemeshingExteriorRegionWidth / SmoothingExteriorRegionWidth), which is not offered here.
#include "MeshHoleFillOperation.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/Operations/SimpleHoleFiller.hpp"
#include "Engine/Geometry/MeshCore/DynamicMeshEditor.hpp"
#include "Engine/Geometry/MeshCore/MeshBoundaryLoops.hpp"
#include "Engine/Geometry/MeshRegionOperation.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <unordered_set>

namespace Desert::Geometry
{
    const char* ToString( HoleFillType type )
    {
        switch ( type )
        {
            case HoleFillType::Trivial:
                return "Trivial";
            case HoleFillType::Minimal:
                return "Minimal";
            case HoleFillType::Smooth:
                return "Smooth";
        }
        return "Unknown";
    }

    namespace
    {
        // Newell's plane of the loop (UE PolygonTriangulation ComputePolygonPlane, as MeshRegionOperation's).
        void LoopPlane( const DynamicMesh3& mesh, const std::vector<int>& loop, glm::dvec3& normal,
                        glm::dvec3& origin )
        {
            normal          = glm::dvec3( 0.0 );
            origin          = glm::dvec3( 0.0 );
            const int count = static_cast<int>( loop.size() );
            for ( int i = count - 1, j = 0; j < count; i = j++ )
            {
                const glm::dvec3 pi = mesh.GetVertex( loop[i] );
                const glm::dvec3 pj = mesh.GetVertex( loop[j] );
                origin += pj;
                normal.x += ( pj.y - pi.y ) * ( pi.z + pj.z );
                normal.y += ( pj.z - pi.z ) * ( pi.x + pj.x );
                normal.z += ( pj.x - pi.x ) * ( pi.y + pj.y );
            }
            origin /= static_cast<double>( count );
            const double len = glm::length( normal );
            normal           = len > 1e-12 ? normal / len : glm::dvec3( 0.0, 0.0, 1.0 );
        }

        double Area( const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c )
        {
            return 0.5 * glm::length( glm::cross( b - a, c - a ) );
        }

        double AngleAt( const glm::dvec3& apex, const glm::dvec3& p, const glm::dvec3& q )
        {
            const glm::dvec3 u = p - apex, v = q - apex;
            const double     d = glm::length( u ) * glm::length( v );
            return d > 1e-20 ? std::acos( std::clamp( glm::dot( u, v ) / d, -1.0, 1.0 ) ) : 0.0;
        }

        // The loop as a polygon on its plane, clipped ear by ear (no new vertex). A triangle (prev, cur, next)
        // in loop order is appended reversed, as SimpleHoleFiller winds its fill against the loop.
        Common::ResultStr<std::vector<int>> EarClip( DynamicMesh3& mesh, const std::vector<int>& loop, int group )
        {
            glm::dvec3 n{}, o{};
            LoopPlane( mesh, loop, n, o );
            const glm::dvec3 ax = glm::normalize( std::abs( n.x ) < 0.9 ? glm::cross( n, glm::dvec3( 1, 0, 0 ) )
                                                                        : glm::cross( n, glm::dvec3( 0, 1, 0 ) ) );
            const glm::dvec3 ay = glm::cross( n, ax );
            std::vector<glm::dvec2> p2;
            for ( const int v : loop )
            {
                const glm::dvec3 d = mesh.GetVertex( v ) - o;
                p2.emplace_back( glm::dot( d, ax ), glm::dot( d, ay ) );
            }
            double signedArea = 0.0;
            for ( size_t i = 0, j = p2.size() - 1; i < p2.size(); j = i++ )
                signedArea += p2[j].x * p2[i].y - p2[i].x * p2[j].y;
            const double sgn   = signedArea >= 0.0 ? 1.0 : -1.0;
            const auto   cross = []( const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c )
            { return ( b.x - a.x ) * ( c.y - a.y ) - ( b.y - a.y ) * ( c.x - a.x ); };

            std::vector<int> idx( loop.size() );
            for ( size_t i = 0; i < idx.size(); ++i )
                idx[i] = static_cast<int>( i );
            std::vector<int> tris;
            while ( idx.size() > 3 )
            {
                const size_t m   = idx.size();
                bool         cut = false;
                for ( size_t k = 0; k < m && !cut; ++k )
                {
                    const int a = idx[( k + m - 1 ) % m], b = idx[k], c = idx[( k + 1 ) % m];
                    if ( cross( p2[a], p2[b], p2[c] ) * sgn <= 1e-12 )
                        continue; // reflex or degenerate corner
                    bool inside = false;
                    for ( const int q : idx )
                    {
                        if ( q == a || q == b || q == c || p2[q] == p2[a] || p2[q] == p2[b] || p2[q] == p2[c] )
                            continue;
                        if ( cross( p2[a], p2[b], p2[q] ) * sgn >= 0.0 &&
                             cross( p2[b], p2[c], p2[q] ) * sgn >= 0.0 &&
                             cross( p2[c], p2[a], p2[q] ) * sgn >= 0.0 )
                        {
                            inside = true;
                            break;
                        }
                    }
                    if ( inside )
                        continue;
                    const int t = mesh.AppendTriangle( Index3i( loop[a], loop[c], loop[b] ), group );
                    if ( t < 0 )
                        return Common::MakeFormattedError<std::vector<int>>(
                             "triangle ({}, {}, {}) could not be appended (an edge already has two triangles)",
                             loop[a], loop[c], loop[b] );
                    tris.push_back( t );
                    idx.erase( idx.begin() + static_cast<std::ptrdiff_t>( k ) );
                    cut = true;
                }
                if ( !cut )
                    return Common::MakeFormattedError<std::vector<int>>(
                         "the {}-vertex loop does not lie on its plane as a simple polygon (no ear left with {} "
                         "corners); the Trivial fill closes it",
                         loop.size(), idx.size() );
            }
            const int t = mesh.AppendTriangle( Index3i( loop[idx[0]], loop[idx[2]], loop[idx[1]] ), group );
            if ( t < 0 )
                return Common::MakeFormattedError<std::vector<int>>(
                     "the last triangle ({}, {}, {}) could not be appended", loop[idx[0]], loop[idx[2]],
                     loop[idx[1]] );
            tris.push_back( t );
            return Common::MakeSuccess( std::move( tris ) );
        }

        // UE MinimalHoleFiller's flip pass over the fill's own edges: a flip that lowers the area is taken; on a
        // tie (a planar fill) the Delaunay criterion decides. A flip that would fold the pair is never taken.
        int FlipToMinimal( DynamicMesh3& mesh, const std::unordered_set<int>& fill )
        {
            int flips = 0;
            for ( int pass = 0; pass < 100; ++pass )
            {
                std::unordered_set<int> edges;
                for ( const int t : fill )
                {
                    const Index3i te = mesh.GetTriEdges( t );
                    for ( int j = 0; j < 3; ++j )
                        edges.insert( te[j] );
                }
                bool changed = false;
                for ( const int e : edges )
                {
                    if ( !mesh.IsEdge( e ) || mesh.IsBoundaryEdge( e ) )
                        continue;
                    const Index2i et = mesh.GetEdgeT( e );
                    if ( !fill.contains( et.A ) || !fill.contains( et.B ) )
                        continue;
                    // a -> b as triangle A winds it; c opposite in A, d opposite in B.
                    const Index3i ta = mesh.GetTriangle( et.A );
                    const Index2i ev = mesh.GetEdgeV( e );
                    int           a = ev.A, b = ev.B;
                    for ( int j = 0; j < 3; ++j )
                        if ( ta[j] == ev.B && ta[( j + 1 ) % 3] == ev.A )
                            std::swap( a, b );
                    const Index2i opp = mesh.GetEdgeOpposingV( e );
                    const int     c   = ( opp.A == ta[0] || opp.A == ta[1] || opp.A == ta[2] ) ? opp.A : opp.B;
                    const int     d   = c == opp.A ? opp.B : opp.A;
                    if ( mesh.FindEdge( c, d ) != IndexConstants::InvalidID )
                        continue;
                    const glm::dvec3 A = mesh.GetVertex( a ), B = mesh.GetVertex( b ), C = mesh.GetVertex( c ),
                                     D    = mesh.GetVertex( d );
                    const glm::dvec3 nOld = glm::cross( B - A, C - A ) + glm::cross( A - B, D - B );
                    if ( glm::dot( glm::cross( D - C, B - C ), nOld ) <= 0.0 ||
                         glm::dot( glm::cross( C - D, A - D ), nOld ) <= 0.0 )
                        continue;
                    const double before = Area( A, B, C ) + Area( A, B, D );
                    const double after  = Area( C, D, B ) + Area( D, C, A );
                    const double tol    = 1e-7 * std::max( before, 1e-12 );
                    const bool   take   = after < before - tol ||
                                      ( std::abs( after - before ) <= tol &&
                                        AngleAt( C, A, B ) + AngleAt( D, A, B ) > std::numbers::pi + 1e-9 );
                    if ( !take )
                        continue;
                    DynamicMesh3::EdgeFlipInfo info;
                    if ( mesh.FlipEdge( e, info ) == MeshResult::Ok )
                    {
                        ++flips;
                        changed = true;
                    }
                }
                if ( !changed )
                    break;
            }
            return flips;
        }

        // UE SmoothHoleFiller: the fill refined towards @p targetLength (edges longer than 4/3 of it split) and
        // its interior vertices relaxed to their one-ring centroid @p iterations times, the loop held.
        void RefineAndSmooth( DynamicMesh3& mesh, std::unordered_set<int>& fill, std::vector<int>& interior,
                              double targetLength, int iterations )
        {
            for ( int round = 0; round < 12; ++round )
            {
                std::vector<int>        longEdges;
                std::unordered_set<int> seen;
                for ( const int t : fill )
                {
                    const Index3i te = mesh.GetTriEdges( t );
                    for ( int j = 0; j < 3; ++j )
                    {
                        const int e = te[j];
                        if ( !seen.insert( e ).second || mesh.IsBoundaryEdge( e ) )
                            continue;
                        const Index2i et = mesh.GetEdgeT( e );
                        if ( !fill.contains( et.A ) || !fill.contains( et.B ) )
                            continue;
                        const Index2i ev = mesh.GetEdgeV( e );
                        if ( glm::length( mesh.GetVertex( ev.A ) - mesh.GetVertex( ev.B ) ) >
                             targetLength * 4.0 / 3.0 )
                            longEdges.push_back( e );
                    }
                }
                if ( longEdges.empty() )
                    break;
                for ( const int e : longEdges )
                {
                    if ( !mesh.IsEdge( e ) )
                        continue;
                    const Index2i ev = mesh.GetEdgeV( e );
                    if ( glm::length( mesh.GetVertex( ev.A ) - mesh.GetVertex( ev.B ) ) <=
                         targetLength * 4.0 / 3.0 )
                        continue;
                    DynamicMesh3::EdgeSplitInfo info;
                    if ( mesh.SplitEdge( e, info, 0.5 ) != MeshResult::Ok )
                        continue;
                    interior.push_back( info.NewVertex );
                    fill.insert( info.NewTriangles.A );
                    if ( info.NewTriangles.B != IndexConstants::InvalidID )
                        fill.insert( info.NewTriangles.B );
                }
                FlipToMinimal( mesh, fill );
            }
            std::vector<glm::dvec3> next( interior.size() );
            for ( int it = 0; it < iterations; ++it )
            {
                for ( size_t i = 0; i < interior.size(); ++i )
                {
                    glm::dvec3 sum( 0.0 );
                    int        count = 0;
                    for ( const int nb : mesh.VtxVerticesItr( interior[i] ) )
                    {
                        sum += mesh.GetVertex( nb );
                        ++count;
                    }
                    next[i] = count > 0 ? sum / static_cast<double>( count ) : mesh.GetVertex( interior[i] );
                }
                for ( size_t i = 0; i < interior.size(); ++i )
                    mesh.SetVertex( interior[i], next[i] );
            }
        }
    } // namespace

    Common::ResultStr<DeformOutcome> HoleFill( const DynamicMesh3& input, const std::vector<int32_t>& loops,
                                               const HoleFillSettings& settings )
    {
        if ( settings.FillDensity <= 0.0f || settings.InteriorSmoothness < 0.0f ||
             settings.InteriorSmoothness > 1.0f )
            return Common::MakeFormattedError<DeformOutcome>(
                 "Hole Fill: Fill Density must be > 0 (is {}) and Interior Smoothness in [0, 1] (is {})",
                 settings.FillDensity, settings.InteriorSmoothness );
        DeformOutcome out{ input };
        DynamicMesh3& mesh = out.Mesh;
        if ( mesh.HasAttributes() && mesh.Attributes()->HasPrimaryColors() )
            return Common::MakeError<DeformOutcome>(
                 "Hole Fill: the mesh has a primary colour layer and the colour "
                 "fill (UE HoleFillUtil::FillColorOverlay) is not ported" );
        const MeshBoundaryLoops boundary( &input );
        if ( boundary.GetLoopCount() == 0 )
            return Common::MakeFormattedError<DeformOutcome>( "Hole Fill: the mesh has no hole ({} open spans)",
                                                              static_cast<int>( boundary.m_Spans.size() ) );
        if ( loops.empty() )
            return Common::MakeFormattedError<DeformOutcome>(
                 "Hole Fill: no hole is selected ({} holes; click a hole's edge or Select All)",
                 boundary.GetLoopCount() );

        const glm::dvec3 extents      = mesh.GetBounds().Extents();
        const double     uvScale      = 1.0 / ( 2.0 * std::max( { extents.x, extents.y, extents.z, 1e-9 } ) );
        int              newTriangles = 0, newVertices = 0;
        for ( const int32_t index : loops )
        {
            if ( index < 0 || index >= boundary.GetLoopCount() )
                return Common::MakeFormattedError<DeformOutcome>( "Hole Fill: hole {} does not exist ({} holes)",
                                                                  index, boundary.GetLoopCount() );
            const EdgeLoop&         loop  = boundary.m_Loops[index];
            const int               group = mesh.HasTriangleGroups() ? mesh.AllocateTriangleGroup() : 0;
            std::unordered_set<int> fill;
            std::vector<int>        interior;
            if ( settings.Type == HoleFillType::Trivial )
            {
                SimpleHoleFiller filler( &mesh, loop );
                if ( !filler.Fill( group ) )
                    return Common::MakeFormattedError<DeformOutcome>( "Hole Fill: hole {}: {}", index,
                                                                      filler.m_FailureReason );
                fill.insert( filler.m_NewTriangles.begin(), filler.m_NewTriangles.end() );
                if ( filler.m_NewVertex != IndexConstants::InvalidID )
                    interior.push_back( filler.m_NewVertex );
            }
            else
            {
                auto clipped = EarClip( mesh, loop.Vertices, group );
                if ( !clipped.IsSuccess() )
                    return Common::MakeFormattedError<DeformOutcome>(
                         "Hole Fill ({}): hole {}: {}", ToString( settings.Type ), index, clipped.GetError() );
                fill.insert( clipped.GetValue().begin(), clipped.GetValue().end() );
                FlipToMinimal( mesh, fill );
                if ( settings.Type == HoleFillType::Smooth )
                {
                    double perimeter = 0.0;
                    for ( const int e : loop.Edges )
                    {
                        const Index2i ev = mesh.GetEdgeV( e );
                        perimeter += glm::length( mesh.GetVertex( ev.A ) - mesh.GetVertex( ev.B ) );
                    }
                    const double target =
                         perimeter / static_cast<double>( loop.Edges.size() ) / settings.FillDensity;
                    RefineAndSmooth( mesh, fill, interior, target,
                                     static_cast<int>( std::lround( settings.InteriorSmoothness * 100.0f ) ) );
                }
            }
            const std::vector<int> tris( fill.begin(), fill.end() );
            if ( mesh.HasAttributes() )
            {
                glm::dvec3 n{}, o{};
                LoopPlane( mesh, loop.Vertices, n, o );
                const DynamicMeshEditor editor( &mesh );
                editor.SetTriangleNormals( tris );
                editor.SetTriangleUVsFromProjection( tris, o, -n, static_cast<float>( uvScale ) );
            }
            newTriangles += static_cast<int>( tris.size() );
            newVertices += static_cast<int>( interior.size() );
        }
        if ( auto tangents = RecomputeTangentSpace( mesh, "Hole Fill" ); !tangents.IsSuccess() )
            return Common::MakeError<DeformOutcome>( tangents.GetError() );
        out.MovedVertices = newVertices;
        out.Report = std::format( "Hole Fill ({}): {} of {} holes filled, {} new triangles, {} new vertices",
                                  ToString( settings.Type ), loops.size(), boundary.GetLoopCount(), newTriangles,
                                  newVertices );
        return Common::MakeSuccess( std::move( out ) );
    }

    std::vector<std::vector<int>> HoleBoundaryLoops( const DynamicMesh3& mesh )
    {
        const MeshBoundaryLoops       boundary( &mesh );
        std::vector<std::vector<int>> holes;
        holes.reserve( boundary.m_Loops.size() );
        for ( const EdgeLoop& loop : boundary.m_Loops )
            holes.push_back( loop.Vertices );
        return holes;
    }
} // namespace Desert::Geometry
