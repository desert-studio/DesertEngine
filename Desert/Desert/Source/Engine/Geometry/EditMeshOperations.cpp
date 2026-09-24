#include "EditMeshOperations.hpp"

#include "EditMeshNormals.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

namespace Desert::Geometry
{
    const char* ToString( ExtrudeDirection direction )
    {
        switch ( direction )
        {
            case ExtrudeDirection::VertexNormals:
                return "VertexNormals";
            case ExtrudeDirection::RegionNormal:
                return "RegionNormal";
        }
        return "Unknown";
    }

    namespace
    {
        // A face may tilt this far from the averaged direction before the lengthening stops: 1 / 0.25 = at
        // most 4x the distance, the point past which a vertex would shoot off rather than keep faces apart.
        constexpr float kMinNormalAgreement = 0.25f;
        // A triangle whose normal turns by more than 90 degrees has flipped over.
        constexpr float kFlipDot = 0.0f;
        // Texel density used for a wall when the region has no UV area to measure one from: 1 UV per metre.
        constexpr float kFallbackUVPerCm = 0.01f;

        using Outcome = Common::ResultStr<MeshEditOutcome>;

        // One edge of the region's boundary, directed the way the region triangle traverses it.
        struct BoundaryEdge
        {
            int From     = InvalidId;
            int To       = InvalidId;
            int Triangle = InvalidId; // the region triangle on it
            int Outside  = InvalidId; // the other triangle, InvalidId on the mesh's open border
        };

        struct Region
        {
            std::vector<int>  Triangles; // ascending
            std::vector<char> InRegion;  // by triangle ID
            std::vector<int>  Vertices;  // ascending
            // Closed loops of boundary edges, each in region winding order (loop[i].To == loop[i+1].From).
            std::vector<std::vector<BoundaryEdge>> Loops;
            int                                    BoundaryEdgeCount = 0;
        };

        glm::vec3 AreaNormal( const EditMesh& mesh, int t ) // length = twice the area
        {
            const auto& c = mesh.GetTriangle( t );
            return glm::cross( mesh.GetPosition( c[1] ) - mesh.GetPosition( c[0] ),
                               mesh.GetPosition( c[2] ) - mesh.GetPosition( c[0] ) );
        }

        // The selection as a triangle region with its boundary loops. `what` names the operation in refusals.
        Common::ResultStr<Region> BuildRegion( const EditMesh& mesh, const ElementSelection& selection,
                                               const char* what )
        {
            if ( selection.Empty() )
                return Common::MakeFormattedError<Region>( "{}: the selection is empty ({} mode)", what,
                                                           ToString( selection.Mode() ) );
            ElementSelection  live  = selection;
            const PruneReport stale = live.Prune( mesh );
            if ( stale.Total() > 0 )
                return Common::MakeFormattedError<Region>(
                     "{}: {} of the {} selected {} elements are no longer on this mesh ({} missing, {} changed)",
                     what, stale.Total(), selection.Size(), ToString( selection.Mode() ), stale.Missing,
                     stale.Changed );
            const ElementSelection triangles = ConvertSelection( mesh, live, ElementMode::Triangle );
            if ( triangles.Empty() )
                return Common::MakeFormattedError<Region>(
                     "{}: the {} selected {} elements cover no whole triangle - select every edge (or vertex) "
                     "of a triangle, or switch to Triangle / PolyGroup mode",
                     what, selection.Size(), ToString( selection.Mode() ) );

            Region region;
            region.Triangles.assign( triangles.Ids().begin(), triangles.Ids().end() );
            region.InRegion.assign( static_cast<size_t>( mesh.MaxTriangleId() ), 0 );
            std::vector<char> vertexIn( static_cast<size_t>( mesh.MaxVertexId() ), 0 );
            for ( const int t : region.Triangles )
            {
                region.InRegion[t] = 1;
                for ( const int v : mesh.GetTriangle( t ) )
                    vertexIn[v] = 1;
            }
            for ( int v = 0; v < mesh.MaxVertexId(); ++v )
                if ( vertexIn[v] )
                    region.Vertices.push_back( v );

            // Boundary edges keyed by their start vertex; a vertex that starts two is where the region
            // touches itself.
            std::unordered_map<int, BoundaryEdge> byFrom;
            for ( const int t : region.Triangles )
            {
                const auto& corners = mesh.GetTriangle( t );
                const auto& edges   = mesh.GetTriangleEdges( t );
                for ( int j = 0; j < 3; ++j )
                {
                    const auto& et    = mesh.GetEdgeTriangles( edges[j] );
                    const int   other = et[0] == t ? et[1] : et[0];
                    if ( other != InvalidId && region.InRegion[other] )
                        continue;
                    const BoundaryEdge b{ corners[j], corners[( j + 1 ) % 3], t, other };
                    if ( !byFrom.emplace( b.From, b ).second )
                        return Common::MakeFormattedError<Region>(
                             "{}: the region touches itself at vertex {} (two boundary loops pass through it); "
                             "the cut-loose copy would pinch there - grow or shrink the selection by that corner",
                             what, b.From );
                    ++region.BoundaryEdgeCount;
                }
            }
            while ( !byFrom.empty() )
            {
                // Start from the lowest vertex so the loop order does not depend on hashing.
                int start = byFrom.begin()->first;
                for ( const auto& [from, edge] : byFrom )
                    start = std::min( start, from );
                std::vector<BoundaryEdge> loop;
                int                       at = start;
                do
                {
                    const auto it = byFrom.find( at );
                    if ( it == byFrom.end() )
                        return Common::MakeFormattedError<Region>(
                             "{}: the boundary of the region does not close at vertex {} - the mesh's edge "
                             "table is inconsistent",
                             what, at );
                    loop.push_back( it->second );
                    at = it->second.To;
                    byFrom.erase( it );
                } while ( at != start );
                region.Loops.push_back( std::move( loop ) );
            }
            return Common::MakeSuccess( std::move( region ) );
        }

        // Refuses a region that reaches the mesh's open border, naming the first such edge.
        Common::BoolResultStr RequireClosedBoundary( const Region& region, const char* what )
        {
            for ( const auto& loop : region.Loops )
                for ( const BoundaryEdge& b : loop )
                    if ( b.Outside == InvalidId )
                        return Common::MakeFormattedError<bool>(
                             "{}: the region reaches the mesh's open border at edge ({}, {}) - there is no "
                             "outside triangle to join that side to; deselect triangle {} or close the hole",
                             what, b.From, b.To, b.Triangle );
            return Common::MakeSuccess( true );
        }

        // Per region vertex: the angle-weighted average of the region's face normals at it, lengthened so each
        // of those faces moves by the full distance.
        std::unordered_map<int, glm::vec3> RegionVertexDirections( const EditMesh& mesh, const Region& region )
        {
            std::unordered_map<int, glm::vec3> sums;
            for ( const int t : region.Triangles )
            {
                const glm::vec3 n = TriangleNormal( mesh, t );
                for ( int j = 0; j < 3; ++j )
                    sums[mesh.GetTriangle( t )[j]] += n * CornerAngle( mesh, t, j );
            }
            std::unordered_map<int, glm::vec3> directions;
            for ( auto& [v, sum] : sums )
            {
                const float length = glm::length( sum );
                if ( length <= 0.0f )
                {
                    directions[v] = glm::vec3( 0.0f );
                    continue;
                }
                const glm::vec3 n         = sum / length;
                float           agreement = 1.0f;
                for ( const int t : mesh.GetVertexTriangles( v ) )
                    if ( region.InRegion[t] && glm::length( AreaNormal( mesh, t ) ) > 0.0f )
                        agreement = std::min( agreement, glm::dot( n, TriangleNormal( mesh, t ) ) );
                directions[v] = n / std::max( agreement, kMinNormalAgreement );
            }
            return directions;
        }

        // ── normals: rebuilt per polygroup at a set of vertices ─────────────────────────────────────────

        // Refuses when a triangle's normal turned by more than 90 degrees between `before` (ID b) and the
        // result (ID a).
        Common::BoolResultStr RequireNoFlip( const EditMesh& before, const EditMesh& after,
                                             const std::vector<std::pair<int, int>>& pairs, const char* what,
                                             float distance )
        {
            for ( const auto& [b, a] : pairs )
            {
                const glm::vec3 nb = TriangleNormal( before, b );
                const glm::vec3 na = TriangleNormal( after, a );
                if ( glm::length( nb ) > 0.0f && glm::dot( nb, na ) <= kFlipDot )
                    return Common::MakeFormattedError<bool>(
                         "{}: at {} cm triangle {} turns over (its normal turns by more than 90 degrees) - the "
                         "distance is larger than the region allows there",
                         what, distance, b );
            }
            return Common::MakeSuccess( true );
        }

        template <typename T>
        struct LayerRecord
        {
            EditMeshOverlay<T>*             Layer = nullptr;
            std::vector<std::array<int, 3>> Elements; // per region triangle, InvalidId when unset
            std::unordered_map<int, T>      Values;
            std::unordered_map<int, int>    Copies; // old element -> element on the region copy
        };

        template <typename T>
        LayerRecord<T> Record( EditMeshOverlay<T>* layer, const Region& region )
        {
            LayerRecord<T> record;
            record.Layer = layer;
            if ( layer == nullptr )
                return record;
            for ( const int t : region.Triangles )
            {
                std::array<int, 3> e{ InvalidId, InvalidId, InvalidId };
                if ( layer->IsSetTriangle( t ) )
                {
                    e = layer->GetTriangle( t );
                    for ( const int id : e )
                        record.Values[id] = layer->GetElement( id );
                }
                record.Elements.push_back( e );
            }
            return record;
        }

        // What the region copy and the strip are built from - everything read from the input mesh before
        // the region's triangles are removed.
        struct Snapshot
        {
            std::vector<std::array<int, 3>>     Corners;
            std::vector<int>                    Groups;
            std::vector<int>                    Materials;
            LayerRecord<glm::vec4>              Tangents;
            LayerRecord<glm::vec4>              Colors;
            std::vector<LayerRecord<glm::vec2>> UVs;
            std::unordered_map<int, int>        IndexOf; // region triangle ID -> index in the vectors above
        };

        Snapshot TakeSnapshot( EditMesh& mesh, const Region& region )
        {
            Snapshot s;
            for ( size_t i = 0; i < region.Triangles.size(); ++i )
            {
                const int t = region.Triangles[i];
                s.Corners.push_back( mesh.GetTriangle( t ) );
                s.Groups.push_back( mesh.Attributes().GetPolyGroup( t ) );
                s.Materials.push_back( mesh.Attributes().GetMaterialId( t ) );
                s.IndexOf[t] = static_cast<int>( i );
            }
            s.Tangents = Record( mesh.Attributes().Tangents(), region );
            s.Colors   = Record( mesh.Attributes().Colors(), region );
            for ( int layer = 0; layer < mesh.Attributes().UVLayerCount(); ++layer )
                s.UVs.push_back( Record( mesh.Attributes().UV( layer ), region ) );
            return s;
        }

        // The UV shift a world displacement means inside triangle t of `mesh` (its UV Jacobian applied to
        // the in-plane part of the displacement).
        glm::vec2 UVShift( const EditMesh& mesh, const std::array<int, 3>& corners,
                           const std::array<glm::vec2, 3>& uv, const glm::vec3& displacement )
        {
            const glm::vec3 e1  = mesh.GetPosition( corners[1] ) - mesh.GetPosition( corners[0] );
            const glm::vec3 e2  = mesh.GetPosition( corners[2] ) - mesh.GetPosition( corners[0] );
            const float     a   = glm::dot( e1, e1 );
            const float     b   = glm::dot( e1, e2 );
            const float     c   = glm::dot( e2, e2 );
            const float     det = a * c - b * b;
            if ( std::abs( det ) <= 1e-12f )
                return glm::vec2( 0.0f );
            const float r1    = glm::dot( displacement, e1 );
            const float r2    = glm::dot( displacement, e2 );
            const float alpha = ( c * r1 - b * r2 ) / det;
            const float beta  = ( a * r2 - b * r1 ) / det;
            return alpha * ( uv[1] - uv[0] ) + beta * ( uv[2] - uv[0] );
        }

        enum class StripUV : uint8_t
        {
            ArcLength,   // extrude walls: their own island, unwrapped by length along the loop and by height
            RegionChart, // inset ring: the region's own chart, continuous across the ring's inner side
        };

        struct CutOptions
        {
            std::string What;
            float       Distance       = 0.0f;
            StripUV     UV             = StripUV::ArcLength;
            bool        CheckStripFlip = false; // Inset only: its ring must stay in the region's surface
        };

        // THE CUT-AND-STITCH CORE shared by Extrude, Push/Pull, Inset and Outset: the region's triangles are
        // re-created on copies of their vertices at `moved` positions, and every boundary loop is stitched to
        // its copy by a strip of quads. `moved` has an entry for every region vertex.
        Outcome CutAndStitch( const EditMesh& input, const Region& region, const ElementSelection& selection,
                              const std::unordered_map<int, glm::vec3>& moved, const CutOptions& options )
        {
            MeshEditOutcome out;
            out.Mesh       = input;
            EditMesh& mesh = out.Mesh;

            int nextGroup = 0;
            for ( const int t : mesh.TriangleIds() )
                nextGroup = std::max( nextGroup, mesh.Attributes().GetPolyGroup( t ) + 1 );

            Snapshot snap = TakeSnapshot( mesh, region );

            // Texel density per UV layer, from the region itself, so a wall is textured at the same scale.
            std::vector<float> density;
            for ( const auto& layer : snap.UVs )
            {
                float world = 0.0f;
                float uv    = 0.0f;
                for ( size_t i = 0; i < region.Triangles.size(); ++i )
                {
                    if ( layer.Elements[i][0] == InvalidId )
                        continue;
                    world += 0.5f * glm::length( AreaNormal( input, region.Triangles[i] ) );
                    const glm::vec2 u0 = layer.Values.at( layer.Elements[i][0] );
                    const glm::vec2 d1 = layer.Values.at( layer.Elements[i][1] ) - u0;
                    const glm::vec2 d2 = layer.Values.at( layer.Elements[i][2] ) - u0;
                    uv += 0.5f * std::abs( d1.x * d2.y - d2.x * d1.y );
                }
                density.push_back( world > 0.0f && uv > 0.0f ? std::sqrt( uv / world ) : kFallbackUVPerCm );
            }

            // 1. The copies, then the region's triangles go (their interior vertices with them).
            std::unordered_map<int, int> copy;
            for ( const int v : region.Vertices )
                copy[v] = mesh.AppendVertex( moved.at( v ) );
            for ( const int t : region.Triangles )
                if ( const EditResult r = mesh.RemoveTriangle( t, true ); r != EditResult::Ok )
                    return Common::MakeFormattedError<MeshEditOutcome>(
                         "{}: removing region triangle {} refused: {}", options.What, t, ToString( r ) );

            // 2. The region again, on the copies.
            std::vector<int>                 regionNew( region.Triangles.size(), InvalidId );
            std::vector<std::pair<int, int>> flipPairs;
            for ( size_t i = 0; i < region.Triangles.size(); ++i )
            {
                const auto& c = snap.Corners[i];
                if ( const EditResult r =
                          mesh.AppendTriangle( copy.at( c[0] ), copy.at( c[1] ), copy.at( c[2] ), regionNew[i] );
                     r != EditResult::Ok )
                    return Common::MakeFormattedError<MeshEditOutcome>(
                         "{}: re-creating region triangle {} on the moved vertices refused: {}", options.What,
                         region.Triangles[i], ToString( r ) );
                mesh.Attributes().SetPolyGroup( regionNew[i], snap.Groups[i] );
                mesh.Attributes().SetMaterialId( regionNew[i], snap.Materials[i] );
                flipPairs.emplace_back( region.Triangles[i], regionNew[i] );
            }

            // The region's own layers: copied element for element (shared corners stay shared, seams stay
            // seams). Inset's UVs move with the boundary through the triangle's own chart.
            auto copyLayer = [&]( auto& record, auto&& valueOf ) -> Common::BoolResultStr
            {
                if ( record.Layer == nullptr )
                    return Common::MakeSuccess( true );
                for ( size_t i = 0; i < region.Triangles.size(); ++i )
                {
                    const auto& old = record.Elements[i];
                    if ( old[0] == InvalidId )
                        continue;
                    std::array<int, 3> next{};
                    for ( int j = 0; j < 3; ++j )
                    {
                        auto it = record.Copies.find( old[j] );
                        if ( it == record.Copies.end() )
                            it = record.Copies
                                      .emplace( old[j], record.Layer->AppendElement( valueOf( record, i, j ) ) )
                                      .first;
                        next[j] = it->second;
                    }
                    if ( const EditResult r = record.Layer->SetTriangle( mesh, regionNew[i], next );
                         r != EditResult::Ok )
                        return Common::MakeFormattedError<bool>(
                             "{}: region copy of triangle {} refused in a layer: {}", options.What,
                             region.Triangles[i], ToString( r ) );
                }
                return Common::MakeSuccess( true );
            };
            auto same = []( const auto& record, size_t i, int j )
            { return record.Values.at( record.Elements[i][j] ); };
            Common::BoolResultStr step = copyLayer( snap.Tangents, same );
            if ( step.IsSuccess() )
                step = copyLayer( snap.Colors, same );
            for ( auto& layer : snap.UVs )
            {
                if ( !step.IsSuccess() )
                    break;
                if ( options.UV == StripUV::RegionChart )
                    step = copyLayer(
                         layer,
                         [&]( const LayerRecord<glm::vec2>& record, size_t i, int j )
                         {
                             const auto&                    c = snap.Corners[i];
                             const std::array<glm::vec2, 3> uv{ record.Values.at( record.Elements[i][0] ),
                                                                record.Values.at( record.Elements[i][1] ),
                                                                record.Values.at( record.Elements[i][2] ) };
                             return uv[j] + UVShift( input, c, uv, moved.at( c[j] ) - input.GetPosition( c[j] ) );
                         } );
                else
                    step = copyLayer( layer, same );
            }
            if ( !step.IsSuccess() )
                return Common::MakeError<MeshEditOutcome>( step.GetError() );

            // 3. The strip: per loop, runs of boundary edges with one outside polygroup get one new group.
            std::vector<int> strip;
            std::vector<int> stripSource; // the region triangle each strip triangle was stitched to
            for ( const auto& loopIn : region.Loops )
            {
                std::vector<BoundaryEdge> loop         = loopIn;
                auto                      outsideGroup = [&]( const BoundaryEdge& b )
                { return input.Attributes().GetPolyGroup( b.Outside ); };
                // Rotate so a run does not straddle the loop's start.
                for ( size_t r = 0; r < loop.size(); ++r )
                    if ( outsideGroup( loop[r] ) != outsideGroup( loop[( r + loop.size() - 1 ) % loop.size()] ) )
                    {
                        std::rotate( loop.begin(), loop.begin() + static_cast<std::ptrdiff_t>( r ), loop.end() );
                        break;
                    }

                std::vector<float> arc( snap.UVs.size(), 0.0f );
                // Elements of the previous quad's trailing side, per UV layer (ArcLength), for sharing.
                std::vector<std::array<int, 2>>    trailing( snap.UVs.size(), { InvalidId, InvalidId } );
                std::map<std::pair<int, int>, int> colorBottom; // (vertex, old element) -> strip element
                std::vector<std::map<std::pair<int, int>, int>> uvBottom( snap.UVs.size() );
                int                                             group = InvalidId;
                for ( size_t k = 0; k < loop.size(); ++k )
                {
                    const BoundaryEdge& b         = loop[k];
                    const bool          runStarts = k == 0 || outsideGroup( b ) != outsideGroup( loop[k - 1] );
                    if ( runStarts )
                    {
                        group = nextGroup++;
                        std::fill( arc.begin(), arc.end(), 0.0f );
                        for ( auto& tr : trailing )
                            tr = { InvalidId, InvalidId };
                    }
                    const int a  = b.From;
                    const int bb = b.To;
                    const int a2 = copy.at( a );
                    const int b2 = copy.at( bb );
                    // Quad a -> bb -> b2 -> a2: traverses (a, bb) the way the region did, which the outside
                    // triangle now meets in the opposite direction, and (b2, a2) against the region copy.
                    std::array<int, 2>                      quad{ InvalidId, InvalidId };
                    const std::array<std::array<int, 3>, 2> tris{ { { a, bb, b2 }, { a, b2, a2 } } };
                    for ( int q = 0; q < 2; ++q )
                        if ( const EditResult r =
                                  mesh.AppendTriangle( tris[q][0], tris[q][1], tris[q][2], quad[q] );
                             r != EditResult::Ok )
                            return Common::MakeFormattedError<MeshEditOutcome>(
                                 "{}: stitching boundary edge ({}, {}) refused: {}", options.What, a, bb,
                                 ToString( r ) );
                    const int i  = snap.IndexOf.at( b.Triangle );
                    const int ca = TriangleCornerOf( input, b.Triangle, a );
                    const int cb = TriangleCornerOf( input, b.Triangle, bb );
                    const int ra = regionNew[i];
                    for ( const int t : quad )
                    {
                        mesh.Attributes().SetPolyGroup( t, group );
                        mesh.Attributes().SetMaterialId( t, snap.Materials[i] );
                        strip.push_back( t );
                        stripSource.push_back( b.Triangle );
                    }

                    // Bottom corners copy the region's old value at the boundary vertex; top corners share the
                    // region copy's element (continuous across the strip's upper side).
                    auto stitchShared = [&]( auto&                               record,
                                             std::map<std::pair<int, int>, int>& bottom ) -> Common::BoolResultStr
                    {
                        if ( record.Layer == nullptr || record.Elements[i][0] == InvalidId )
                            return Common::MakeSuccess( true );
                        auto bottomOf = [&]( int v, int corner )
                        {
                            const int old = record.Elements[i][corner];
                            auto      it  = bottom.find( { v, old } );
                            if ( it == bottom.end() )
                                it = bottom
                                          .emplace( std::pair{ v, old },
                                                    record.Layer->AppendElement( record.Values.at( old ) ) )
                                          .first;
                            return it->second;
                        };
                        const int ea  = bottomOf( a, ca );
                        const int eb  = bottomOf( bb, cb );
                        const int ea2 = record.Layer->GetTriangle( ra )[ca];
                        const int eb2 = record.Layer->GetTriangle( ra )[cb];
                        if ( const EditResult r = record.Layer->SetTriangle( mesh, quad[0], { ea, eb, eb2 } );
                             r != EditResult::Ok )
                            return Common::MakeFormattedError<bool>( "{}: strip layer refused: {}", options.What,
                                                                     ToString( r ) );
                        if ( const EditResult r = record.Layer->SetTriangle( mesh, quad[1], { ea, eb2, ea2 } );
                             r != EditResult::Ok )
                            return Common::MakeFormattedError<bool>( "{}: strip layer refused: {}", options.What,
                                                                     ToString( r ) );
                        return Common::MakeSuccess( true );
                    };
                    if ( auto r = stitchShared( snap.Colors, colorBottom ); !r.IsSuccess() )
                        return Common::MakeError<MeshEditOutcome>( r.GetError() );
                    for ( size_t l = 0; l < snap.UVs.size(); ++l )
                    {
                        if ( options.UV == StripUV::RegionChart )
                        {
                            if ( auto r = stitchShared( snap.UVs[l], uvBottom[l] ); !r.IsSuccess() )
                                return Common::MakeError<MeshEditOutcome>( r.GetError() );
                            continue;
                        }
                        UVOverlay*         layer = snap.UVs[l].Layer;
                        const float        k     = density[l];
                        const float        len   = glm::length( input.GetPosition( bb ) - input.GetPosition( a ) );
                        const float        ha    = glm::length( mesh.GetPosition( a2 ) - mesh.GetPosition( a ) );
                        const float        hb    = glm::length( mesh.GetPosition( b2 ) - mesh.GetPosition( bb ) );
                        std::array<int, 2> lead  = trailing[l];
                        if ( lead[0] == InvalidId )
                            lead = { layer->AppendElement( glm::vec2( arc[l] * k, 0.0f ) ),
                                     layer->AppendElement( glm::vec2( arc[l] * k, ha * k ) ) };
                        arc[l] += len;
                        const std::array<int, 2> trail{ layer->AppendElement( glm::vec2( arc[l] * k, 0.0f ) ),
                                                        layer->AppendElement( glm::vec2( arc[l] * k, hb * k ) ) };
                        if ( const EditResult r =
                                  layer->SetTriangle( mesh, quad[0], { lead[0], trail[0], trail[1] } );
                             r != EditResult::Ok )
                            return Common::MakeFormattedError<MeshEditOutcome>( "{}: wall UV refused: {}",
                                                                                options.What, ToString( r ) );
                        if ( const EditResult r =
                                  layer->SetTriangle( mesh, quad[1], { lead[0], trail[1], lead[1] } );
                             r != EditResult::Ok )
                            return Common::MakeFormattedError<MeshEditOutcome>( "{}: wall UV refused: {}",
                                                                                options.What, ToString( r ) );
                        trailing[l] = trail;
                    }
                }
            }

            // 4. Normals where anything moved or was created, then the strip's tangents.
            std::vector<int> rebuild;
            for ( const auto& [v, c] : copy )
                rebuild.push_back( c );
            for ( const auto& loop : region.Loops )
                for ( const BoundaryEdge& b : loop )
                    rebuild.push_back( b.From );
            std::sort( rebuild.begin(), rebuild.end() );
            if ( auto r = RebuildNormalsByPolyGroupAt( mesh, rebuild ); !r.IsSuccess() )
                return Common::MakeError<MeshEditOutcome>( std::string( options.What ) + ": " + r.GetError() );
            if ( auto r = ComputeTangentsAt( mesh, strip ); !r.IsSuccess() )
                return Common::MakeError<MeshEditOutcome>( std::string( options.What ) + ": " + r.GetError() );

            // 5. Nothing may turn over: the region against itself, the strip against the region it came from.
            if ( auto r = RequireNoFlip( input, mesh, flipPairs, options.What.c_str(), options.Distance );
                 !r.IsSuccess() )
                return Common::MakeError<MeshEditOutcome>( r.GetError() );
            if ( options.CheckStripFlip )
                for ( size_t s = 0; s < strip.size(); ++s )
                {
                    // An inset ring lies in the region's own surface: a ring triangle facing away from the
                    // region triangle it grew from means the ring folded over (the inset is wider than the
                    // region there).
                    const glm::vec3 was = TriangleNormal( input, stripSource[s] );
                    const glm::vec3 now = TriangleNormal( mesh, strip[s] );
                    if ( glm::length( now ) > 0.0f && glm::dot( was, now ) <= kFlipDot )
                        return Common::MakeFormattedError<MeshEditOutcome>(
                             "{}: at {} cm the ring folds over next to region triangle {} - the inset is wider "
                             "than the region there",
                             options.What, options.Distance, stripSource[s] );
                }
            if ( auto valid = mesh.CheckValidity(); !valid.IsSuccess() )
                return Common::MakeError<MeshEditOutcome>(
                     std::string( options.What ) + ": the result is not a valid mesh: " + valid.GetError() );

            // The operated region, on the result, in the input's mode.
            ElementSelection result( ElementMode::Triangle );
            for ( const int t : regionNew )
                (void)result.Add( mesh, t );
            out.Selection = selection.Mode() == ElementMode::Triangle
                                 ? result
                                 : ConvertSelection( mesh, result, selection.Mode() );
            return Common::MakeSuccess( std::move( out ) );
        }
    } // namespace

    Common::ResultStr<MeshEditOutcome> DeleteSelection( const EditMesh& mesh, const ElementSelection& selection )
    {
        auto built = BuildRegion( mesh, selection, "Delete" );
        if ( !built.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( built.GetError() );
        const Region&   region = built.GetValue();
        MeshEditOutcome out;
        out.Mesh      = mesh;
        out.Selection = ElementSelection( selection.Mode() );
        for ( const int t : region.Triangles )
            if ( const EditResult r = out.Mesh.RemoveTriangle( t, true ); r != EditResult::Ok )
                return Common::MakeFormattedError<MeshEditOutcome>( "Delete: removing triangle {} refused: {}", t,
                                                                    ToString( r ) );
        return Common::MakeSuccess( std::move( out ) );
    }

    namespace
    {
        Outcome Extrude( const EditMesh& mesh, const ElementSelection& selection, float distance,
                         ExtrudeDirection direction, const char* what )
        {
            if ( distance == 0.0f )
                return Common::MakeFormattedError<MeshEditOutcome>( "{}: distance 0 cm changes nothing", what );
            auto built = BuildRegion( mesh, selection, what );
            if ( !built.IsSuccess() )
                return Common::MakeError<MeshEditOutcome>( built.GetError() );
            const Region& region = built.GetValue();
            if ( region.Loops.empty() )
                return Common::MakeFormattedError<MeshEditOutcome>(
                     "{}: the {} selected triangles are a closed piece with no boundary - there is nothing to "
                     "wall; use Offset to move it",
                     what, region.Triangles.size() );
            if ( auto closed = RequireClosedBoundary( region, what ); !closed.IsSuccess() )
                return Common::MakeError<MeshEditOutcome>( closed.GetError() );

            std::unordered_map<int, glm::vec3> moved;
            if ( direction == ExtrudeDirection::RegionNormal )
            {
                glm::vec3 sum( 0.0f );
                for ( const int t : region.Triangles )
                    sum += AreaNormal( mesh, t );
                if ( glm::length( sum ) <= 1e-6f )
                    return Common::MakeFormattedError<MeshEditOutcome>(
                         "{}: the region's normals cancel out (it faces every way equally) - there is no single "
                         "direction to push along",
                         what );
                const glm::vec3 n = glm::normalize( sum );
                for ( const int v : region.Vertices )
                    moved[v] = mesh.GetPosition( v ) + n * distance;
            }
            else
            {
                const auto dirs = RegionVertexDirections( mesh, region );
                for ( const int v : region.Vertices )
                    moved[v] = mesh.GetPosition( v ) + dirs.at( v ) * distance;
            }
            CutOptions options;
            options.What     = what;
            options.Distance = distance;
            options.UV       = StripUV::ArcLength;
            return CutAndStitch( mesh, region, selection, moved, options );
        }

        Outcome Inset( const EditMesh& mesh, const ElementSelection& selection, float distance, const char* what )
        {
            if ( distance == 0.0f )
                return Common::MakeFormattedError<MeshEditOutcome>( "{}: distance 0 cm changes nothing", what );
            auto built = BuildRegion( mesh, selection, what );
            if ( !built.IsSuccess() )
                return Common::MakeError<MeshEditOutcome>( built.GetError() );
            const Region& region = built.GetValue();
            if ( region.Loops.empty() )
                return Common::MakeFormattedError<MeshEditOutcome>(
                     "{}: the {} selected triangles are a closed piece with no boundary - there is no border "
                     "to inset",
                     what, region.Triangles.size() );
            if ( auto closed = RequireClosedBoundary( region, what ); !closed.IsSuccess() )
                return Common::MakeError<MeshEditOutcome>( closed.GetError() );

            std::unordered_map<int, glm::vec3> moved;
            for ( const int v : region.Vertices )
                moved[v] = mesh.GetPosition( v );
            // Each boundary vertex moves along the bisector of its two edges' inward perpendiculars (each in
            // its own triangle's plane), lengthened so both edges move in by the full distance.
            auto inward = [&]( const BoundaryEdge& b )
            {
                const glm::vec3 along = mesh.GetPosition( b.To ) - mesh.GetPosition( b.From );
                const glm::vec3 p     = glm::cross( TriangleNormal( mesh, b.Triangle ), along );
                return glm::length( p ) > 0.0f ? glm::normalize( p ) : glm::vec3( 0.0f );
            };
            for ( const auto& loop : region.Loops )
                for ( size_t k = 0; k < loop.size(); ++k )
                {
                    const glm::vec3 pIn  = inward( loop[( k + loop.size() - 1 ) % loop.size()] );
                    const glm::vec3 pOut = inward( loop[k] );
                    const glm::vec3 sum  = pIn + pOut;
                    if ( glm::length( sum ) <= 1e-6f )
                        return Common::MakeFormattedError<MeshEditOutcome>(
                             "{}: the boundary folds back on itself at vertex {} - there is no inward direction",
                             what, loop[k].From );
                    const glm::vec3 bisector = glm::normalize( sum );
                    const float     scale    = 1.0f / std::max( glm::dot( bisector, pOut ), kMinNormalAgreement );
                    moved[loop[k].From]      = mesh.GetPosition( loop[k].From ) + bisector * ( distance * scale );
                }
            CutOptions options;
            options.What           = what;
            options.Distance       = distance;
            options.UV             = StripUV::RegionChart;
            options.CheckStripFlip = distance > 0.0f;
            return CutAndStitch( mesh, region, selection, moved, options );
        }
    } // namespace

    Common::ResultStr<MeshEditOutcome> ExtrudeSelection( const EditMesh& mesh, const ElementSelection& selection,
                                                         float distance, ExtrudeDirection direction )
    {
        if ( distance < 0.0f )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Extrude: distance {} cm is negative - Push/Pull takes a signed distance", distance );
        return Extrude( mesh, selection, distance, direction, "Extrude" );
    }

    Common::ResultStr<MeshEditOutcome> PushPullSelection( const EditMesh& mesh, const ElementSelection& selection,
                                                          float distance )
    {
        return Extrude( mesh, selection, distance, ExtrudeDirection::RegionNormal, "Push/Pull" );
    }

    Common::ResultStr<MeshEditOutcome> InsetSelection( const EditMesh& mesh, const ElementSelection& selection,
                                                       float distance )
    {
        if ( distance < 0.0f )
            return Common::MakeFormattedError<MeshEditOutcome>( "Inset: distance {} cm is negative - use Outset",
                                                                distance );
        return Inset( mesh, selection, distance, "Inset" );
    }

    Common::ResultStr<MeshEditOutcome> OutsetSelection( const EditMesh& mesh, const ElementSelection& selection,
                                                        float distance )
    {
        if ( distance < 0.0f )
            return Common::MakeFormattedError<MeshEditOutcome>( "Outset: distance {} cm is negative - use Inset",
                                                                distance );
        return Inset( mesh, selection, -distance, "Outset" );
    }

    Common::ResultStr<MeshEditOutcome> OffsetSelection( const EditMesh& mesh, const ElementSelection& selection,
                                                        float distance )
    {
        if ( distance == 0.0f )
            return Common::MakeError<MeshEditOutcome>( "Offset: distance 0 cm changes nothing" );
        auto built = BuildRegion( mesh, selection, "Offset" );
        if ( !built.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( built.GetError() );
        const Region& region = built.GetValue();

        MeshEditOutcome out;
        out.Mesh        = mesh;
        const auto dirs = RegionVertexDirections( mesh, region );
        for ( const int v : region.Vertices )
            out.Mesh.SetPosition( v, mesh.GetPosition( v ) + dirs.at( v ) * distance );

        // Every triangle at a moved vertex changed shape; normals are rebuilt at all of their corners.
        std::vector<int> touched;
        std::vector<int> corners;
        for ( const int v : region.Vertices )
            for ( const int t : mesh.GetVertexTriangles( v ) )
                touched.push_back( t );
        std::sort( touched.begin(), touched.end() );
        touched.erase( std::unique( touched.begin(), touched.end() ), touched.end() );
        std::vector<std::pair<int, int>> pairs;
        for ( const int t : touched )
        {
            pairs.emplace_back( t, t );
            for ( const int v : mesh.GetTriangle( t ) )
                corners.push_back( v );
        }
        std::sort( corners.begin(), corners.end() );
        corners.erase( std::unique( corners.begin(), corners.end() ), corners.end() );
        if ( auto r = RequireNoFlip( mesh, out.Mesh, pairs, "Offset", distance ); !r.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( r.GetError() );
        if ( auto r = RebuildNormalsByPolyGroupAt( out.Mesh, corners ); !r.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( std::string( "Offset: " ) + r.GetError() );
        out.Selection = selection;
        (void)out.Selection.Prune( out.Mesh );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry
