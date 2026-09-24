#include "EditMeshTopologyOperations.hpp"

#include "EditMeshNormals.hpp"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::Geometry
{
    namespace
    {
        using Outcome = Common::ResultStr<MeshEditOutcome>;

        // A vertex this close to a cutting plane counts as ON it: its edges are not split there (a split
        // 1e-4 cm from an existing vertex would only make a sliver).
        constexpr float kOnPlane = 1e-4f;
        // Two faces within this angle of each other (cos) have no corner to bevel.
        constexpr float kFlatCos = 0.9998f; // ~1.1 degrees
        // A largest angle must beat the runner-up by this much (radians) to name a quad diagonal: the two
        // equal base angles of a fan triangle must not.
        constexpr float kAngleTie = 1e-3f;
        // Texel density for a cap when its neighbours have no UV area to measure: 1 UV per metre.
        constexpr float kFallbackUVPerCm = 0.01f;

        float Distance( const CutPlane& plane, const glm::vec3& p )
        {
            return glm::dot( p - plane.Point, plane.Normal );
        }

        int NewPolyGroup( const EditMesh& mesh )
        {
            int top = -1;
            for ( const int t : mesh.TriangleIds() )
                top = std::max( top, mesh.Attributes().GetPolyGroup( t ) );
            return top + 1;
        }

        Common::ResultStr<ElementSelection> Live( const EditMesh& mesh, const ElementSelection& selection,
                                                  const char* what )
        {
            if ( selection.Empty() )
                return Common::MakeFormattedError<ElementSelection>( "{}: the selection is empty ({} mode)", what,
                                                                     ToString( selection.Mode() ) );
            ElementSelection  live  = selection;
            const PruneReport stale = live.Prune( mesh );
            if ( stale.Total() > 0 )
                return Common::MakeFormattedError<ElementSelection>(
                     "{}: {} of the {} selected {} elements are no longer on this mesh", what, stale.Total(),
                     selection.Size(), ToString( selection.Mode() ) );
            return Common::MakeSuccess( std::move( live ) );
        }

        // ── the plane-cut core ─────────────────────────────────────────────────────────────────────────

        // Splits every edge of the working triangles (inSet, by triangle ID; grows with the splits) whose
        // ends lie strictly on opposite sides of the plane, until none does. A split triangle's two halves
        // stay in the set; a neighbour outside it is split too (its halves stay outside).
        Common::BoolResultStr SplitAlongPlane( EditMesh& mesh, std::vector<char>& inSet, const CutPlane& plane,
                                               const char* what )
        {
            bool changed = true;
            while ( changed )
            {
                changed = false;
                for ( int t = 0; t < mesh.MaxTriangleId(); ++t )
                {
                    if ( !mesh.IsTriangle( t ) || t >= static_cast<int>( inSet.size() ) || !inSet[t] )
                        continue;
                    for ( const int e : mesh.GetTriangleEdges( t ) )
                    {
                        const auto& ends = mesh.GetEdgeVertices( e );
                        const float d0   = Distance( plane, mesh.GetPosition( ends[0] ) );
                        const float d1   = Distance( plane, mesh.GetPosition( ends[1] ) );
                        if ( !( ( d0 > kOnPlane && d1 < -kOnPlane ) || ( d0 < -kOnPlane && d1 > kOnPlane ) ) )
                            continue;
                        const std::array<int, 2> sides = mesh.GetEdgeTriangles( e );
                        SplitEdgeInfo            info;
                        if ( const EditResult r = mesh.SplitEdge( e, d0 / ( d0 - d1 ), info );
                             r != EditResult::Ok )
                            return Common::MakeFormattedError<bool>(
                                 "{}: splitting edge {} on the plane refused: {}", what, e, ToString( r ) );
                        inSet.resize( static_cast<size_t>( mesh.MaxTriangleId() ), 0 );
                        for ( int s = 0; s < 2; ++s )
                            if ( sides[s] != InvalidId && info.NewTriangles[s] != InvalidId )
                                inSet[info.NewTriangles[s]] = inSet[sides[s]];
                        changed = true;
                        break; // t's edges changed; look at it again on the next pass
                    }
                }
            }
            return Common::MakeSuccess( true );
        }

        // +1 / -1 for a triangle wholly on one side (corners on the plane do not vote), 0 for one lying in it.
        int SideOf( const EditMesh& mesh, int t, const CutPlane& plane )
        {
            int side = 0;
            for ( const int v : mesh.GetTriangle( t ) )
            {
                const float d = Distance( plane, mesh.GetPosition( v ) );
                if ( d > kOnPlane )
                    side = 1;
                else if ( d < -kOnPlane )
                    side = -1;
            }
            return side;
        }

        // ── bevel ──────────────────────────────────────────────────────────────────────────────────────

        // The UV scale (UV units per cm) of the triangles around the cap, from their own UV and world area.
        float TexelDensity( const EditMesh& mesh, const UVOverlay& uv, const std::vector<int>& triangles )
        {
            double world = 0.0;
            double tex   = 0.0;
            for ( const int t : triangles )
            {
                if ( !uv.IsSetTriangle( t ) )
                    continue;
                const auto& c = mesh.GetTriangle( t );
                const auto& u = uv.GetTriangle( t );
                world += glm::length( glm::cross( mesh.GetPosition( c[1] ) - mesh.GetPosition( c[0] ),
                                                  mesh.GetPosition( c[2] ) - mesh.GetPosition( c[0] ) ) );
                const glm::vec2 a = uv.GetElement( u[1] ) - uv.GetElement( u[0] );
                const glm::vec2 b = uv.GetElement( u[2] ) - uv.GetElement( u[0] );
                tex += std::abs( a.x * b.y - a.y * b.x );
            }
            return world > 0.0 && tex > 0.0 ? static_cast<float>( std::sqrt( tex / world ) ) : kFallbackUVPerCm;
        }

        // Ear-clips the planar polygon `loop` (vertex IDs, counter-clockwise seen from where `normal` points)
        // into triangles.
        Common::ResultStr<std::vector<std::array<int, 3>>> Triangulate( const EditMesh&         mesh,
                                                                        const std::vector<int>& loop,
                                                                        const glm::vec3& normal, const char* what )
        {
            const glm::vec3 n  = glm::normalize( normal );
            const glm::vec3 ax = glm::normalize( mesh.GetPosition( loop[1] ) - mesh.GetPosition( loop[0] ) );
            const glm::vec3 ay = glm::cross( n, ax );
            std::vector<glm::vec2> p;
            for ( const int v : loop )
            {
                const glm::vec3 d = mesh.GetPosition( v ) - mesh.GetPosition( loop[0] );
                p.emplace_back( glm::dot( d, ax ), glm::dot( d, ay ) );
            }
            auto cross2 = []( const glm::vec2& a, const glm::vec2& b, const glm::vec2& c )
            { return ( b.x - a.x ) * ( c.y - a.y ) - ( b.y - a.y ) * ( c.x - a.x ); };
            std::vector<int>                corner( loop.size() );
            std::vector<std::array<int, 3>> out;
            std::iota( corner.begin(), corner.end(), 0 );
            while ( corner.size() > 3 )
            {
                bool clipped = false;
                for ( size_t i = 0; i < corner.size() && !clipped; ++i )
                {
                    const int a = corner[( i + corner.size() - 1 ) % corner.size()];
                    const int b = corner[i];
                    const int c = corner[( i + 1 ) % corner.size()];
                    if ( cross2( p[a], p[b], p[c] ) <= 1e-9f )
                        continue;
                    bool inside = false;
                    for ( const int o : corner )
                        if ( o != a && o != b && o != c && cross2( p[a], p[b], p[o] ) >= 0.0f &&
                             cross2( p[b], p[c], p[o] ) >= 0.0f && cross2( p[c], p[a], p[o] ) >= 0.0f )
                            inside = true;
                    if ( inside )
                        continue;
                    out.push_back( { loop[a], loop[b], loop[c] } );
                    corner.erase( corner.begin() + static_cast<std::ptrdiff_t>( i ) );
                    clipped = true;
                }
                if ( !clipped )
                    return Common::MakeFormattedError<std::vector<std::array<int, 3>>>(
                         "{}: the {}-corner cap does not triangulate (it folds over itself in its plane)", what,
                         loop.size() );
            }
            if ( cross2( p[corner[0]], p[corner[1]], p[corner[2]] ) <= 0.0f )
                return Common::MakeFormattedError<std::vector<std::array<int, 3>>>(
                     "{}: the cap's last triangle has no area or faces inwards", what );
            out.push_back( { loop[corner[0]], loop[corner[1]], loop[corner[2]] } );
            return Common::MakeSuccess( std::move( out ) );
        }

        // Cuts the corner of `mesh` around `targets` off with `plane` (the corner on its positive side) and
        // closes the hole with a flat cap of a new polygroup. Returns that group.
        Common::ResultStr<int> CutCorner( EditMesh& mesh, const std::vector<int>& targets, const CutPlane& plane,
                                          const char* what )
        {
            std::vector<char> inSet( static_cast<size_t>( mesh.MaxTriangleId() ), 0 );
            std::set<int>     targetSet( targets.begin(), targets.end() );
            for ( const int v : targets )
                for ( const int t : mesh.GetVertexTriangles( v ) )
                {
                    inSet[t] = 1;
                    for ( const int o : mesh.GetTriangle( t ) )
                    {
                        if ( targetSet.count( o ) )
                            continue;
                        const float d = Distance( plane, mesh.GetPosition( o ) );
                        if ( d > -kOnPlane )
                            return Common::MakeFormattedError<int>(
                                 "{}: the cut reaches vertex {} ({:.3f} cm past it) - the width is larger than "
                                 "the faces around vertex {} allow",
                                 what, o, d / glm::length( plane.Normal ), v );
                    }
                }
            auto cut = CutAwayPositiveSide( mesh, inSet, plane, true, what );
            if ( !cut.IsSuccess() )
                return Common::MakeError<int>( cut.GetError() );
            if ( cut.GetValue().CapLoops != 1 )
                return Common::MakeFormattedError<int>(
                     "{}: the cut outline is {} loops - the corner is not a simple cap", what,
                     cut.GetValue().CapLoops );
            return Common::MakeSuccess( cut.GetValue().Group );
        }

        Common::ResultStr<CutPlane> EdgeBevelPlane( const EditMesh& mesh, int e, float width )
        {
            const auto& tris = mesh.GetEdgeTriangles( e );
            if ( tris[1] == InvalidId )
                return Common::MakeFormattedError<CutPlane>( "Bevel: edge {} is on the mesh's open border", e );
            const auto&     ends = mesh.GetEdgeVertices( e );
            const glm::vec3 pu   = mesh.GetPosition( ends[0] );
            const glm::vec3 dir  = glm::normalize( mesh.GetPosition( ends[1] ) - pu );
            const glm::vec3 nA   = TriangleNormal( mesh, tris[0] );
            const glm::vec3 nB   = TriangleNormal( mesh, tris[1] );
            if ( glm::dot( nA, nB ) > kFlatCos )
                return Common::MakeFormattedError<CutPlane>(
                     "Bevel: edge {} is flat (its faces differ by {:.2f} degrees) - there is no corner to cut", e,
                     glm::degrees( std::acos( std::clamp( glm::dot( nA, nB ), -1.0f, 1.0f ) ) ) );
            // In face A, perpendicular to the edge, pointing away from it into the face.
            glm::vec3 dA = glm::normalize( glm::cross( nA, dir ) );
            for ( const int v : mesh.GetTriangle( tris[0] ) )
                if ( v != ends[0] && v != ends[1] && glm::dot( dA, mesh.GetPosition( v ) - pu ) < 0.0f )
                    dA = -dA;
            if ( glm::dot( nB, dA ) > -1e-4f )
                return Common::MakeFormattedError<CutPlane>(
                     "Bevel: edge {} is concave - a chamfer cuts a convex corner off, a concave one would need "
                     "material added",
                     e );
            // The plane through both offset lines: by symmetry its normal is the faces' average.
            return Common::MakeSuccess( CutPlane{ pu + width * dA, glm::normalize( nA + nB ) } );
        }

        Common::ResultStr<CutPlane> VertexBevelPlane( const EditMesh& mesh, int v, float width )
        {
            glm::vec3 sum( 0.0f );
            for ( const int t : mesh.GetVertexTriangles( v ) )
                sum += TriangleNormal( mesh, t ) * CornerAngle( mesh, t, TriangleCornerOf( mesh, t, v ) );
            if ( glm::length( sum ) <= 0.0f || mesh.IsBoundaryVertex( v ) )
                return Common::MakeFormattedError<CutPlane>( "Bevel: vertex {} has no closed corner to cut", v );
            const glm::vec3 n = glm::normalize( sum );
            const glm::vec3 p = mesh.GetPosition( v );
            // The width is measured along the corner's FEATURE edges - those between two polygroups, the
            // edges a user sees; a face diagonal through the corner is not one (on a box corner it would pull
            // the cut 18 % shallower). A smooth vertex (no feature edge) uses all its edges.
            float depth    = 0.0f;
            int   count    = 0;
            float depthAll = 0.0f;
            int   countAll = 0;
            for ( const int e : mesh.GetVertexEdges( v ) )
            {
                const auto& ends  = mesh.GetEdgeVertices( e );
                const int   o     = ends[0] == v ? ends[1] : ends[0];
                const float along = glm::dot( -n, glm::normalize( mesh.GetPosition( o ) - p ) );
                if ( along <= 0.05f )
                    return Common::MakeFormattedError<CutPlane>(
                         "Bevel: vertex {} is not a convex corner (its edge to vertex {} runs level with or above "
                         "its normal)",
                         v, o );
                const auto& tris = mesh.GetEdgeTriangles( e );
                if ( mesh.Attributes().GetPolyGroup( tris[0] ) != mesh.Attributes().GetPolyGroup( tris[1] ) )
                {
                    depth += along;
                    ++count;
                }
                depthAll += along;
                ++countAll;
            }
            if ( count == 0 )
            {
                depth = depthAll;
                count = countAll;
            }
            return Common::MakeSuccess( CutPlane{ p - n * ( width * depth / static_cast<float>( count ) ), n } );
        }

        // ── insert edge loop ───────────────────────────────────────────────────────────────────────────

        // The edge of t opposite its single largest angle, InvalidId when two angles tie for largest.
        int LongSide( const EditMesh& mesh, int t )
        {
            std::array<float, 3> angle{};
            for ( int j = 0; j < 3; ++j )
                angle[j] = CornerAngle( mesh, t, j );
            const int big = static_cast<int>( std::max_element( angle.begin(), angle.end() ) - angle.begin() );
            for ( int j = 0; j < 3; ++j )
                if ( j != big && angle[big] - angle[j] < kAngleTie )
                    return InvalidId;
            return mesh.GetTriangleEdges( t )[( big + 1 ) % 3]; // edge j joins corners j and j+1
        }

        // The other half of t's quad, InvalidId when t is not half of one. Returns the diagonal too.
        int QuadPartner( const EditMesh& mesh, int t, int& diagonal )
        {
            diagonal = LongSide( mesh, t );
            if ( diagonal == InvalidId )
                return InvalidId;
            const auto& tris  = mesh.GetEdgeTriangles( diagonal );
            const int   other = tris[0] == t ? tris[1] : tris[0];
            if ( other == InvalidId || LongSide( mesh, other ) != diagonal ||
                 mesh.Attributes().GetPolyGroup( other ) != mesh.Attributes().GetPolyGroup( t ) )
                return InvalidId;
            return other;
        }

        struct RingStep
        {
            int Edge = InvalidId;
            int AEnd = InvalidId; // the end the position is measured from
        };

        bool SharesVertex( const EditMesh& mesh, int e, int f )
        {
            const auto& a = mesh.GetEdgeVertices( e );
            const auto& b = mesh.GetEdgeVertices( f );
            return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
        }

        // Walks the ring from `start` into triangle `t`. Appends the ring edges after start and the quads'
        // diagonals; returns how the walk ended.
        enum class RingEnd : uint8_t
        {
            Closed,
            Border,
            Triangle,
        };

        RingEnd WalkRing( const EditMesh& mesh, RingStep start, int t, std::vector<RingStep>& ring,
                          std::vector<int>& diagonals, int& stopTriangle )
        {
            RingStep at = start;
            while ( true )
            {
                int       diagonal = InvalidId;
                const int partner  = QuadPartner( mesh, t, diagonal );
                // Entering a quad through its own diagonal is entering a triangle: the ring crosses quads
                // between opposite SIDES.
                if ( partner == InvalidId || diagonal == at.Edge )
                {
                    stopTriangle = t;
                    return RingEnd::Triangle;
                }
                std::vector<int> sides;
                for ( const int q : { t, partner } )
                    for ( const int e : mesh.GetTriangleEdges( q ) )
                        if ( e != diagonal && e != at.Edge )
                            sides.push_back( e );
                int opposite = InvalidId;
                for ( const int e : sides )
                    if ( !SharesVertex( mesh, e, at.Edge ) )
                        opposite = e;
                // The quad side from the A end reaches the opposite edge's A end.
                int aEnd = InvalidId;
                for ( const int e : sides )
                {
                    if ( e == opposite )
                        continue;
                    const auto& s = mesh.GetEdgeVertices( e );
                    if ( s[0] == at.AEnd || s[1] == at.AEnd )
                        aEnd = s[0] == at.AEnd ? s[1] : s[0];
                }
                if ( std::find( diagonals.begin(), diagonals.end(), diagonal ) != diagonals.end() )
                    return RingEnd::Closed; // came round onto a quad already walked from the other side
                diagonals.push_back( diagonal );
                if ( opposite == ring.front().Edge )
                    return RingEnd::Closed;
                at = { opposite, aEnd };
                ring.push_back( at );
                const auto& tris = mesh.GetEdgeTriangles( opposite );
                const int   next = tris[0] == t || tris[0] == partner ? tris[1] : tris[0];
                if ( next == InvalidId )
                    return RingEnd::Border;
                t = next;
            }
        }

        const char* RingEndName( RingEnd end )
        {
            switch ( end )
            {
                case RingEnd::Closed:
                    return "closed";
                case RingEnd::Border:
                    return "the open border";
                case RingEnd::Triangle:
                    return "a triangle";
            }
            return "?";
        }

        // ── clean ──────────────────────────────────────────────────────────────────────────────────────

        int Find( std::vector<int>& parent, int v )
        {
            while ( parent[v] != v )
            {
                parent[v] = parent[parent[v]];
                v         = parent[v];
            }
            return v;
        }

        template <typename T>
        Common::BoolResultStr CopyLayer( const EditMeshOverlay<T>* from, EditMesh& target, EditMeshOverlay<T>* to,
                                         const std::vector<std::pair<int, int>>& triangles )
        {
            if ( from == nullptr || to == nullptr )
                return Common::MakeSuccess( true );
            std::unordered_map<int, int> element;
            for ( const auto& [s, t] : triangles )
            {
                if ( !from->IsSetTriangle( s ) )
                    continue;
                std::array<int, 3> next{};
                for ( int j = 0; j < 3; ++j )
                {
                    const int e  = from->GetTriangle( s )[j];
                    auto      it = element.find( e );
                    if ( it == element.end() )
                        it = element.emplace( e, to->AppendElement( from->GetElement( e ) ) ).first;
                    next[j] = it->second;
                }
                if ( const EditResult r = to->SetTriangle( target, t, next ); r != EditResult::Ok )
                    return Common::MakeFormattedError<bool>(
                         "Clean: carrying the attribute elements of triangle {} over refused: {} (the weld joined "
                         "two corners of one element's vertex)",
                         s, ToString( r ) );
            }
            return Common::MakeSuccess( true );
        }
    } // namespace

    Common::BoolResultStr SplitMeshAlongPlane( EditMesh& mesh, std::vector<char>& inSet, const CutPlane& plane,
                                               const char* what )
    {
        return SplitAlongPlane( mesh, inSet, plane, what );
    }

    Common::ResultStr<PlaneCutCap> CutAwayPositiveSide( EditMesh& mesh, std::vector<char>& inSet,
                                                        const CutPlane& plane, bool fillHole, const char* what )
    {
        if ( auto split = SplitAlongPlane( mesh, inSet, plane, what ); !split.IsSuccess() )
            return Common::MakeError<PlaneCutCap>( split.GetError() );

        // What goes: the working triangles on the positive side, and a triangle lying IN the plane whose face
        // looks against the normal - the solid behind it is the removed side, so it is that side's wall. One
        // facing along the normal already closes the kept side and stays.
        std::vector<char> gone( static_cast<size_t>( mesh.MaxTriangleId() ), 0 );
        std::vector<int>  removed;
        for ( int t = 0; t < mesh.MaxTriangleId(); ++t )
        {
            if ( !mesh.IsTriangle( t ) || t >= static_cast<int>( inSet.size() ) || !inSet[t] )
                continue;
            const int side = SideOf( mesh, t, plane );
            if ( side == 0 )
            {
                const auto&     c    = mesh.GetTriangle( t );
                const glm::vec3 face = glm::cross( mesh.GetPosition( c[1] ) - mesh.GetPosition( c[0] ),
                                                   mesh.GetPosition( c[2] ) - mesh.GetPosition( c[0] ) );
                if ( glm::dot( face, plane.Normal ) >= 0.0f )
                    continue;
            }
            else if ( side < 0 )
                continue;
            gone[t] = 1;
            removed.push_back( t );
        }
        PlaneCutCap result;
        result.RemovedTriangles = static_cast<int>( removed.size() );

        // The rim: each edge between a removed and a kept triangle, directed the way the CAP must run it
        // (against the kept triangle). An edge of a removed triangle on the open border is no rim.
        std::map<int, int> next;   // cap vertex -> the next one round its loop
        std::map<int, int> keptAt; // cap vertex -> a kept triangle at it (attribute source)
        if ( fillHole )
            for ( const int t : removed )
                for ( const int e : mesh.GetTriangleEdges( t ) )
                {
                    const auto& tris  = mesh.GetEdgeTriangles( e );
                    const int   other = tris[0] == t ? tris[1] : tris[0];
                    if ( other == InvalidId || gone[other] )
                        continue;
                    const auto& c     = mesh.GetTriangle( other );
                    const auto& edges = mesh.GetTriangleEdges( other );
                    const int   j = static_cast<int>( std::find( edges.begin(), edges.end(), e ) - edges.begin() );
                    // `other` runs c[j] -> c[j+1]; the cap runs it back.
                    const int from = c[( j + 1 ) % 3];
                    const int to   = c[j];
                    if ( next.count( from ) )
                        return Common::MakeFormattedError<PlaneCutCap>(
                             "{}: the cut outline passes vertex {} twice - the section is not a simple outline",
                             what, from );
                    next[from]   = to;
                    keptAt[from] = other;
                    keptAt[to]   = other;
                }

        std::vector<std::vector<int>> loops;
        std::set<int>                 visited;
        for ( const auto& [start, unused] : next )
        {
            if ( visited.count( start ) )
                continue;
            std::vector<int> loop{ start };
            visited.insert( start );
            while ( true )
            {
                const auto it = next.find( loop.back() );
                if ( it == next.end() )
                    return Common::MakeFormattedError<PlaneCutCap>(
                         "{}: the cut outline is open at vertex {} - the mesh's open border crosses the plane "
                         "there, so there is no solid to cap (cut without filling the hole)",
                         what, loop.back() );
                if ( it->second == loop.front() )
                    break;
                if ( !visited.insert( it->second ).second )
                    return Common::MakeFormattedError<PlaneCutCap>(
                         "{}: the cut outline passes vertex {} twice - the section is not a simple outline", what,
                         it->second );
                loop.push_back( it->second );
            }
            if ( loop.size() < 3 )
                return Common::MakeFormattedError<PlaneCutCap>(
                     "{}: a cut outline at vertex {} has {} corners - nothing to cap", what, start, loop.size() );
            // Newell's area along the normal: an outline around solid runs counter-clockwise seen from the
            // removed side; a clockwise one is a HOLE inside another outline (a tube cut across).
            glm::vec3 area( 0.0f );
            for ( size_t i = 0; i < loop.size(); ++i )
                area +=
                     glm::cross( mesh.GetPosition( loop[i] ), mesh.GetPosition( loop[( i + 1 ) % loop.size()] ) );
            if ( !( glm::dot( area, plane.Normal ) > 0.0f ) )
                return Common::MakeFormattedError<PlaneCutCap>(
                     "{}: the {}-corner cut outline at vertex {} runs clockwise - it is a hole inside another "
                     "outline (a tube or ring cut across), and a cap with holes is not built (cut without "
                     "filling the hole)",
                     what, loop.size(), start );
            loops.push_back( std::move( loop ) );
        }

        const int material = keptAt.empty() ? 0 : mesh.Attributes().GetMaterialId( keptAt.begin()->second );
        for ( const int t : removed )
            if ( const EditResult r = mesh.RemoveTriangle( t, true ); r != EditResult::Ok )
                return Common::MakeFormattedError<PlaneCutCap>( "{}: removing triangle {} refused: {}", what, t,
                                                                ToString( r ) );
        if ( loops.empty() )
            return Common::MakeSuccess( result );

        result.Group                   = NewPolyGroup( mesh );
        result.CapLoops                = static_cast<int>( loops.size() );
        EditMeshAttributes& attributes = mesh.Attributes();
        std::vector<int>    around;
        for ( const auto& [v, t] : keptAt )
            around.push_back( t );
        std::vector<int> cap;
        std::vector<int> rimVertices;
        const glm::vec3  n = glm::normalize( plane.Normal );
        for ( const std::vector<int>& loop : loops )
        {
            auto tris = Triangulate( mesh, loop, plane.Normal, what );
            if ( !tris.IsSuccess() )
                return Common::MakeError<PlaneCutCap>( tris.GetError() );
            std::vector<int> loopCap;
            for ( const auto& tri : tris.GetValue() )
            {
                int t = InvalidId;
                if ( const EditResult r = mesh.AppendTriangle( tri[0], tri[1], tri[2], t ); r != EditResult::Ok )
                    return Common::MakeFormattedError<PlaneCutCap>(
                         "{}: the cap triangle ({}, {}, {}) refused: {}", what, tri[0], tri[1], tri[2],
                         ToString( r ) );
                attributes.SetPolyGroup( t, result.Group );
                attributes.SetMaterialId( t, material );
                loopCap.push_back( t );
            }

            // UVs: a planar projection in the cap's plane, anchored at the loop's first vertex's UV on its kept
            // neighbour, at the neighbours' texel density.
            const glm::vec3 ax = glm::normalize( mesh.GetPosition( loop[1] ) - mesh.GetPosition( loop[0] ) );
            const glm::vec3 ay = glm::cross( n, ax );
            for ( int layer = 0; layer < attributes.UVLayerCount(); ++layer )
            {
                UVOverlay&         uv      = *attributes.UV( layer );
                const float        density = TexelDensity( mesh, uv, around );
                const int          anchorT = keptAt.at( loop[0] );
                const glm::vec2    origin  = uv.IsSetTriangle( anchorT )
                                                  ? uv.GetElement( uv.GetElementAtVertex( mesh, anchorT, loop[0] ) )
                                                  : glm::vec2( 0.0f );
                std::map<int, int> element;
                for ( const int v : loop )
                {
                    const glm::vec3 d = mesh.GetPosition( v ) - mesh.GetPosition( loop[0] );
                    element[v] =
                         uv.AppendElement( origin + density * glm::vec2( glm::dot( d, ax ), glm::dot( d, ay ) ) );
                }
                for ( const int t : loopCap )
                {
                    const auto& c = mesh.GetTriangle( t );
                    if ( const EditResult r =
                              uv.SetTriangle( mesh, t, { element[c[0]], element[c[1]], element[c[2]] } );
                         r != EditResult::Ok )
                        return Common::MakeFormattedError<PlaneCutCap>( "{}: cap UVs on triangle {} refused: {}",
                                                                        what, t, ToString( r ) );
                }
            }
            if ( ColorOverlay* colors = attributes.Colors() )
            {
                std::map<int, int> element;
                for ( const int v : loop )
                {
                    const int       t     = keptAt.at( v );
                    const glm::vec4 value = colors->IsSetTriangle( t )
                                                 ? colors->GetElement( colors->GetElementAtVertex( mesh, t, v ) )
                                                 : glm::vec4( 1.0f );
                    element[v]            = colors->AppendElement( value );
                }
                for ( const int t : loopCap )
                {
                    const auto& c = mesh.GetTriangle( t );
                    if ( const EditResult r =
                              colors->SetTriangle( mesh, t, { element[c[0]], element[c[1]], element[c[2]] } );
                         r != EditResult::Ok )
                        return Common::MakeFormattedError<PlaneCutCap>(
                             "{}: cap colours on triangle {} refused: {}", what, t, ToString( r ) );
                }
            }
            cap.insert( cap.end(), loopCap.begin(), loopCap.end() );
            rimVertices.insert( rimVertices.end(), loop.begin(), loop.end() );
        }
        result.CapTriangles = static_cast<int>( cap.size() );
        if ( auto r = RebuildNormalsByPolyGroupAt( mesh, rimVertices ); !r.IsSuccess() )
            return Common::MakeFormattedError<PlaneCutCap>( "{}: {}", what, r.GetError() );
        if ( auto r = ComputeTangentsAt( mesh, cap ); !r.IsSuccess() )
            return Common::MakeFormattedError<PlaneCutCap>( "{}: {}", what, r.GetError() );
        return Common::MakeSuccess( result );
    }

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

    Outcome BevelSelection( const EditMesh& mesh, const ElementSelection& selection, float width )
    {
        auto live = Live( mesh, selection, "Bevel" );
        if ( !live.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( live.GetError() );
        const ElementMode mode = selection.Mode();
        if ( mode != ElementMode::Edge && mode != ElementMode::Vertex )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Bevel: works on Edge or Vertex selections, not {} - select the edges or corners to cut",
                 ToString( mode ) );
        if ( !( width > 0.0f ) )
            return Common::MakeFormattedError<MeshEditOutcome>( "Bevel: the width must be above 0 cm, not {}",
                                                                width );
        const std::vector<int> ids( live.GetValue().Ids().begin(), live.GetValue().Ids().end() );
        if ( mode == ElementMode::Edge )
        {
            std::map<int, int> owner;
            for ( const int e : ids )
                for ( const int v : mesh.GetEdgeVertices( e ) )
                    if ( auto [it, fresh] = owner.emplace( v, e ); !fresh )
                        return Common::MakeFormattedError<MeshEditOutcome>(
                             "Bevel: edges {} and {} share vertex {} - a chained bevel needs mitred corners, "
                             "which "
                             "a one-segment cut per edge cannot give; bevel them one at a time",
                             it->second, e, v );
        }

        MeshEditOutcome  out{ mesh, ElementSelection( ElementMode::PolyGroup ), {} };
        std::vector<int> groups;
        for ( const int id : ids )
        {
            // IDs survive the earlier cuts: nothing is compacted, and the refusal above keeps every earlier
            // cut away from this element's vertices.
            auto plane = mode == ElementMode::Edge ? EdgeBevelPlane( out.Mesh, id, width )
                                                   : VertexBevelPlane( out.Mesh, id, width );
            if ( !plane.IsSuccess() )
                return Common::MakeError<MeshEditOutcome>( plane.GetError() );
            std::vector<int> targets;
            if ( mode == ElementMode::Edge )
                targets.assign( out.Mesh.GetEdgeVertices( id ).begin(), out.Mesh.GetEdgeVertices( id ).end() );
            else
                targets.push_back( id );
            auto group = CutCorner( out.Mesh, targets, plane.GetValue(), "Bevel" );
            if ( !group.IsSuccess() )
                return Common::MakeFormattedError<MeshEditOutcome>( "{} (at {} {}, width {} cm)", group.GetError(),
                                                                    ToString( mode ), id, width );
            groups.push_back( group.GetValue() );
        }
        for ( const int g : groups )
            if ( auto r = out.Selection.Add( out.Mesh, g ); !r.IsSuccess() )
                return Common::MakeFormattedError<MeshEditOutcome>( "Bevel: selecting cap {}: {}", g,
                                                                    r.GetError() );
        if ( auto valid = out.Mesh.CheckValidity(); !valid.IsSuccess() )
            return Common::MakeFormattedError<MeshEditOutcome>( "Bevel: the result is not a valid mesh: {}",
                                                                valid.GetError() );
        return Common::MakeSuccess( std::move( out ) );
    }

    Outcome InsertEdgeLoop( const EditMesh& mesh, const ElementSelection& selection, float position )
    {
        auto live = Live( mesh, selection, "Insert Edge Loop" );
        if ( !live.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( live.GetError() );
        if ( selection.Mode() != ElementMode::Edge || live.GetValue().Size() != 1 )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Insert Edge Loop: select exactly one edge (the ring crosses it), not {} {} elements",
                 live.GetValue().Size(), ToString( selection.Mode() ) );
        if ( !( position > 0.0f && position < 1.0f ) )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Insert Edge Loop: the position must lie strictly between 0 and 1, not {}", position );
        const int e0 = live.GetValue().Ids()[0];

        std::vector<RingStep> ring{ { e0, mesh.GetEdgeVertices( e0 )[0] } };
        std::vector<int>      diagonals;
        const auto&           sides = mesh.GetEdgeTriangles( e0 );
        int                   stop0 = InvalidId;
        int                   stop1 = InvalidId;
        const RingEnd         end0  = WalkRing( mesh, ring.front(), sides[0], ring, diagonals, stop0 );
        RingEnd               end1  = RingEnd::Closed;
        if ( end0 != RingEnd::Closed )
        {
            if ( sides[1] == InvalidId )
                end1 = RingEnd::Border;
            else
            {
                // The second walk grows the ring at its front: walk into a scratch list and prepend it.
                std::vector<RingStep> back{ ring.front() };
                end1 = WalkRing( mesh, ring.front(), sides[1], back, diagonals, stop1 );
                ring.insert( ring.begin(), back.rbegin(), back.rend() - 1 );
            }
        }
        if ( diagonals.empty() )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Insert Edge Loop: edge {} has no quad on either side (triangles {} and {} are not halves of "
                 "quads) - there is no ring to cut",
                 e0, sides[0], sides[1] );

        MeshEditOutcome out{ mesh, ElementSelection( ElementMode::Edge ), {} };
        for ( const RingStep& step : ring )
        {
            const auto&   ends = out.Mesh.GetEdgeVertices( step.Edge );
            const float   t    = step.AEnd == ends[0] ? position : 1.0f - position;
            SplitEdgeInfo info;
            if ( const EditResult r = out.Mesh.SplitEdge( step.Edge, t, info ); r != EditResult::Ok )
                return Common::MakeFormattedError<MeshEditOutcome>(
                     "Insert Edge Loop: splitting ring edge {} refused: {}", step.Edge, ToString( r ) );
        }
        // Each quad's diagonal now joins the two triangles between the loop's new vertices: its flip is the
        // loop's edge across that quad.
        for ( const int d : diagonals )
        {
            FlipEdgeInfo info;
            if ( const EditResult r = out.Mesh.FlipEdge( d, info ); r != EditResult::Ok )
                return Common::MakeFormattedError<MeshEditOutcome>(
                     "Insert Edge Loop: turning quad diagonal {} into the loop refused: {}", d, ToString( r ) );
            if ( auto r = out.Selection.Add( out.Mesh, d ); !r.IsSuccess() )
                return Common::MakeFormattedError<MeshEditOutcome>( "Insert Edge Loop: {}", r.GetError() );
        }
        for ( const int t : out.Mesh.TriangleIds() )
            if ( glm::length( TriangleNormal( out.Mesh, t ) ) <= 0.0f )
                return Common::MakeFormattedError<MeshEditOutcome>(
                     "Insert Edge Loop: triangle {} of the result has no area - a quad in the ring is not convex",
                     t );
        if ( end0 == RingEnd::Closed )
            out.Report = fmt::format( "closed ring of {} quads", diagonals.size() );
        else
            out.Report = fmt::format( "open ring of {} quads, ends at {}{} and {}{}", diagonals.size(),
                                      RingEndName( end0 ), stop0 != InvalidId ? fmt::format( " {}", stop0 ) : "",
                                      RingEndName( end1 ), stop1 != InvalidId ? fmt::format( " {}", stop1 ) : "" );
        return Common::MakeSuccess( std::move( out ) );
    }

    Outcome CutSelection( const EditMesh& mesh, const ElementSelection& selection, const CutPlane& plane )
    {
        auto live = Live( mesh, selection, "Cut" );
        if ( !live.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( live.GetError() );
        if ( glm::length( plane.Normal ) <= 0.0f )
            return Common::MakeError<MeshEditOutcome>( "Cut: the plane has no normal" );
        const ElementSelection triangles = ConvertSelection( mesh, live.GetValue(), ElementMode::Triangle );
        if ( triangles.Empty() )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Cut: the {} selected {} elements cover no whole triangle", selection.Size(),
                 ToString( selection.Mode() ) );
        std::set<int> groups;
        for ( const int t : triangles.Ids() )
            groups.insert( mesh.Attributes().GetPolyGroup( t ) );

        MeshEditOutcome   out{ mesh, ElementSelection( ElementMode::PolyGroup ), {} };
        std::vector<char> inSet( static_cast<size_t>( out.Mesh.MaxTriangleId() ), 0 );
        for ( const int t : out.Mesh.TriangleIds() )
            inSet[t] = groups.count( out.Mesh.Attributes().GetPolyGroup( t ) ) ? 1 : 0;
        if ( auto split = SplitAlongPlane( out.Mesh, inSet, plane, "Cut" ); !split.IsSuccess() )
            return Common::MakeError<MeshEditOutcome>( split.GetError() );

        std::map<int, std::array<std::vector<int>, 2>> halves; // group -> (negative, positive) triangles
        for ( const int t : out.Mesh.TriangleIds() )
        {
            if ( t >= static_cast<int>( inSet.size() ) || !inSet[t] )
                continue;
            const int side = SideOf( out.Mesh, t, plane );
            if ( side != 0 )
                halves[out.Mesh.Attributes().GetPolyGroup( t )][side > 0 ? 1 : 0].push_back( t );
        }
        int              next = NewPolyGroup( out.Mesh );
        std::vector<int> onCut;
        std::vector<int> cutGroups;
        for ( auto& [group, parts] : halves )
        {
            if ( parts[0].empty() || parts[1].empty() )
                continue;
            for ( const int t : parts[1] )
            {
                out.Mesh.Attributes().SetPolyGroup( t, next );
                for ( const int v : out.Mesh.GetTriangle( t ) )
                    if ( std::abs( Distance( plane, out.Mesh.GetPosition( v ) ) ) <= kOnPlane )
                        onCut.push_back( v );
            }
            cutGroups.push_back( group );
            cutGroups.push_back( next++ );
        }
        if ( cutGroups.empty() )
            return Common::MakeFormattedError<MeshEditOutcome>(
                 "Cut: the plane crosses none of the {} selected polygroups", groups.size() );
        std::sort( onCut.begin(), onCut.end() );
        onCut.erase( std::unique( onCut.begin(), onCut.end() ), onCut.end() );
        if ( auto r = RebuildNormalsByPolyGroupAt( out.Mesh, onCut ); !r.IsSuccess() )
            return Common::MakeFormattedError<MeshEditOutcome>( "Cut: {}", r.GetError() );
        for ( const int g : cutGroups )
            if ( auto r = out.Selection.Add( out.Mesh, g ); !r.IsSuccess() )
                return Common::MakeFormattedError<MeshEditOutcome>( "Cut: selecting group {}: {}", g,
                                                                    r.GetError() );
        out.Report = fmt::format( "{} of {} selected polygroups cut", cutGroups.size() / 2, groups.size() );
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<CleanOutcome> CleanMesh( const EditMesh& mesh, float weldTolerance )
    {
        if ( !( weldTolerance >= 0.0f ) )
            return Common::MakeFormattedError<CleanOutcome>( "Clean: the weld tolerance must be >= 0 cm, not {}",
                                                             weldTolerance );
        CleanOutcome out;
        // Weld clusters by a grid of tolerance-sized cells: a vertex only needs its 27 neighbouring cells.
        std::vector<int> parent( static_cast<size_t>( mesh.MaxVertexId() ) );
        std::iota( parent.begin(), parent.end(), 0 );
        if ( weldTolerance > 0.0f )
        {
            std::map<std::array<long long, 3>, std::vector<int>> cells;
            auto                                                 cellOf = [&]( const glm::vec3& p )
            {
                return std::array<long long, 3>{ static_cast<long long>( std::floor( p.x / weldTolerance ) ),
                                                 static_cast<long long>( std::floor( p.y / weldTolerance ) ),
                                                 static_cast<long long>( std::floor( p.z / weldTolerance ) ) };
            };
            for ( const int v : mesh.VertexIds() )
            {
                const glm::vec3 p = mesh.GetPosition( v );
                const auto      c = cellOf( p );
                for ( long long dx = -1; dx <= 1; ++dx )
                    for ( long long dy = -1; dy <= 1; ++dy )
                        for ( long long dz = -1; dz <= 1; ++dz )
                        {
                            const auto it = cells.find( { c[0] + dx, c[1] + dy, c[2] + dz } );
                            if ( it == cells.end() )
                                continue;
                            for ( const int o : it->second )
                                if ( glm::length( mesh.GetPosition( o ) - p ) <= weldTolerance )
                                {
                                    const int a              = Find( parent, v );
                                    const int b              = Find( parent, o );
                                    parent[std::max( a, b )] = std::min( a, b );
                                }
                        }
                cells[c].push_back( v );
            }
        }
        for ( const int v : mesh.VertexIds() )
            if ( Find( parent, v ) != v )
                ++out.Counts.WeldedVertices;

        EditMesh&                 result = out.Edit.Mesh;
        const EditMeshAttributes& from   = mesh.Attributes();
        EditMeshAttributes&       to     = result.Attributes();
        if ( from.Normals() )
            to.EnableNormals();
        if ( from.Tangents() )
            to.EnableTangents();
        if ( from.Colors() )
            to.EnableColors();
        if ( !to.SetUVLayerCount( from.UVLayerCount() ) )
            return Common::MakeFormattedError<CleanOutcome>( "Clean: {} UV layers could not be set up",
                                                             from.UVLayerCount() );
        std::vector<int> map( static_cast<size_t>( mesh.MaxVertexId() ), InvalidId );
        auto             vertexOf = [&]( int v )
        {
            const int r = Find( parent, v );
            if ( map[r] == InvalidId )
                map[r] = result.AppendVertex( mesh.GetPosition( r ) );
            return map[r];
        };
        std::vector<std::pair<int, int>> kept; // (source triangle, result triangle)
        for ( const int t : mesh.TriangleIds() )
        {
            const auto& c = mesh.GetTriangle( t );
            const int   a = Find( parent, c[0] );
            const int   b = Find( parent, c[1] );
            const int   d = Find( parent, c[2] );
            if ( a == b || b == d || a == d )
            {
                ++out.Counts.CollapsedRemoved;
                continue;
            }
            int        n = InvalidId;
            const auto r = result.AppendTriangle( vertexOf( c[0] ), vertexOf( c[1] ), vertexOf( c[2] ), n );
            if ( r != EditResult::Ok )
                return Common::MakeFormattedError<CleanOutcome>(
                     "Clean: after welding at {} cm triangle {} cannot join the surface ({}) - it would put a "
                     "third triangle on an edge or duplicate a coincident face; weld with a smaller tolerance",
                     weldTolerance, t, ToString( r ) );
            to.SetPolyGroup( n, from.GetPolyGroup( t ) );
            to.SetMaterialId( n, from.GetMaterialId( t ) );
            kept.emplace_back( t, n );
        }
        for ( const int v : mesh.VertexIds() )
            if ( mesh.GetVertexEdges( v ).empty() )
                ++out.Counts.IsolatedRemoved;
        // A vertex whose every triangle the weld collapsed is isolated now: it was never appended.
        for ( const int v : mesh.VertexIds() )
            if ( Find( parent, v ) == v && map[v] == InvalidId && !mesh.GetVertexEdges( v ).empty() )
                ++out.Counts.IsolatedRemoved;

        Common::BoolResultStr copied = CopyLayer( from.Normals(), result, to.Normals(), kept );
        if ( copied.IsSuccess() )
            copied = CopyLayer( from.Tangents(), result, to.Tangents(), kept );
        if ( copied.IsSuccess() )
            copied = CopyLayer( from.Colors(), result, to.Colors(), kept );
        for ( int layer = 0; layer < from.UVLayerCount() && copied.IsSuccess(); ++layer )
            copied = CopyLayer( from.UV( layer ), result, to.UV( layer ), kept );
        if ( !copied.IsSuccess() )
            return Common::MakeError<CleanOutcome>( copied.GetError() );

        // Zero area with three distinct corners: one corner lies on the opposite edge. Flipping that (the
        // longest) edge hands the area to the neighbour's two new triangles and leaves no sliver.
        std::vector<int> slivers;
        for ( const int t : result.TriangleIds() )
            if ( glm::length( TriangleNormal( result, t ) ) <= 0.0f )
                slivers.push_back( t );
        for ( const int t : slivers )
        {
            if ( !result.IsTriangle( t ) || glm::length( TriangleNormal( result, t ) ) > 0.0f )
                continue;
            int   longest = InvalidId;
            float length  = -1.0f;
            for ( const int e : result.GetTriangleEdges( t ) )
            {
                const auto& ends = result.GetEdgeVertices( e );
                const float l    = glm::length( result.GetPosition( ends[1] ) - result.GetPosition( ends[0] ) );
                if ( l > length )
                {
                    length  = l;
                    longest = e;
                }
            }
            FlipEdgeInfo info;
            if ( result.FlipEdge( longest, info ) == EditResult::Ok )
                ++out.Counts.ZeroAreaFlipped;
            else
                ++out.Counts.ZeroAreaKept;
        }
        const CleanCounts& n = out.Counts;
        out.Edit.Report      = fmt::format(
             "welded {} vertices, removed {} collapsed triangles and {} isolated vertices, "
                  "flipped {} zero-area triangles ({} kept)",
             n.WeldedVertices, n.CollapsedRemoved, n.IsolatedRemoved, n.ZeroAreaFlipped, n.ZeroAreaKept );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry
