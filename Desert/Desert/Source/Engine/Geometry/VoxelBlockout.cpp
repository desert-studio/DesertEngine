#include <Engine/Geometry/VoxelBlockout.hpp>

#include <Engine/Geometry/GreedyMesher.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <string>

namespace Desert::Geometry::VoxelBlockout
{
    namespace
    {
        constexpr int OFF = 1 << 20;

        // World-aligned ("triplanar-ish") UVs: one UV unit per metre of world space, so a 4x1 wall and a 1x1
        // block share a texel density and a MERGED quad tiles instead of stretching one 0..1 patch over it.
        // This is what makes greedy meshing safe to turn on - per-face 0..1 UVs would smear.
        constexpr float kUvPerUnit = 1.0f / 100.0f;

        // The UV axes of a face, in world space. Vertical faces put V on world +Y (a wall's texture stays
        // upright whichever way it faces); the two horizontal faces fall back to X/Z.
        struct FaceUvFrame
        {
            glm::vec3 T, B;
        };
        FaceUvFrame UvFrame( int f )
        {
            const glm::vec3 n = kFace[f][0].N;
            if ( std::abs( n.y ) > 0.5f )
                return { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
            const glm::vec3 t = glm::cross( glm::vec3( 0.0f, 1.0f, 0.0f ), n );
            return { glm::normalize( t ), { 0.0f, 1.0f, 0.0f } };
        }
    } // namespace

    const FaceVert kFace[6][4] = {
         // Front (+Z)
         { { { -0.5f, -0.5f, 0.5f }, { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0 } },
           { { 0.5f, -0.5f, 0.5f }, { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 0 } },
           { { 0.5f, 0.5f, 0.5f }, { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1 } },
           { { -0.5f, 0.5f, 0.5f }, { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 1 } } },
         // Back (-Z)
         { { { -0.5f, -0.5f, -0.5f }, { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 1, 0 } },
           { { -0.5f, 0.5f, -0.5f }, { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 1, 1 } },
           { { 0.5f, 0.5f, -0.5f }, { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, 1 } },
           { { 0.5f, -0.5f, -0.5f }, { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, 0 } } },
         // Top (+Y)
         { { { -0.5f, 0.5f, -0.5f }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1 } },
           { { -0.5f, 0.5f, 0.5f }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 0 } },
           { { 0.5f, 0.5f, 0.5f }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 }, { 1, 0 } },
           { { 0.5f, 0.5f, -0.5f }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 }, { 1, 1 } } },
         // Bottom (-Y)
         { { { -0.5f, -0.5f, -0.5f }, { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 1, 1 } },
           { { 0.5f, -0.5f, -0.5f }, { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 1 } },
           { { 0.5f, -0.5f, 0.5f }, { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0 } },
           { { -0.5f, -0.5f, 0.5f }, { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0 } } },
         // Left (-X)
         { { { -0.5f, -0.5f, -0.5f }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 0 } },
           { { -0.5f, -0.5f, 0.5f }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0 } },
           { { -0.5f, 0.5f, 0.5f }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 1, 1 } },
           { { -0.5f, 0.5f, -0.5f }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1 } } },
         // Right (+X)
         { { { 0.5f, -0.5f, -0.5f }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0 } },
           { { 0.5f, 0.5f, -0.5f }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 1, 1 } },
           { { 0.5f, 0.5f, 0.5f }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, 1 } },
           { { 0.5f, -0.5f, 0.5f }, { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, 0 } } },
    };

    const glm::ivec3 kNeighbor[6] = { { 0, 0, 1 },  { 0, 0, -1 }, { 0, 1, 0 },
                                      { 0, -1, 0 }, { -1, 0, 0 }, { 1, 0, 0 } };

    const int kFaceCorner[6][4] = {
         { 4, 5, 7, 6 }, // Front  (+Z)
         { 0, 2, 3, 1 }, // Back   (-Z)
         { 2, 6, 7, 3 }, // Top    (+Y)
         { 0, 1, 5, 4 }, // Bottom (-Y)
         { 0, 4, 6, 2 }, // Left   (-X)
         { 1, 3, 7, 5 }, // Right  (+X)
    };
    const int kFaceAxisBit[6] = { 4, 4, 2, 2, 1, 1 };

    const RectPost kPosts[4] = { { false, false }, { true, false }, { false, true }, { true, true } };

    int PostCorner( const WorkPlane& plane, int k )
    {
        const int ua = ( plane.Na + 1 ) % 3;
        const int va = ( plane.Na + 2 ) % 3;
        return ( plane.Sign > 0 ? 1 << plane.Na : 0 ) | ( kPosts[k].AtUMax ? 1 << ua : 0 ) |
               ( kPosts[k].AtVMax ? 1 << va : 0 );
    }

    bool SplitsAlong13( const Cell& cell, int f )
    {
        bool welded[4];
        for ( int k = 0; k < 4; ++k )
            welded[k] = cell.V[kFaceCorner[f][k]] == 0;
        const bool across13 = welded[1] != welded[3] || ( !welded[1] && welded[0] && welded[2] );
        return across13 != cell.Crosswise;
    }

    namespace
    {
        // The lattice axis face f looks along.
        int FaceNormalAxis( int f )
        {
            return f < 2 ? 2 : ( f < 4 ? 1 : 0 );
        }

        // Is corner i of `a` where corner j of `b` is? Offsets along different axes only agree at zero.
        bool SameCorner( const Cell& a, int i, const Cell& b, int j )
        {
            return a.V[i] == b.V[j] && ( a.V[i] == 0 || a.Axis == b.Axis );
        }

        // A convex polygon in a face's plane coordinates: in-plane axes (n+1)%3 and (n+2)%3, cell-relative.
        using Poly2 = std::vector<glm::vec2>;

        float SignedArea( const Poly2& p )
        {
            float a = 0.0f;
            for ( size_t i = 0; i < p.size(); ++i )
            {
                const glm::vec2& u = p[i];
                const glm::vec2& w = p[( i + 1 ) % p.size()];
                a += u.x * w.y - w.x * u.y;
            }
            return 0.5f * a;
        }

        // The part of convex `p` on the left of the line through e0 -> e1 (or on its right).
        Poly2 ClipHalfPlane( const Poly2& p, const glm::vec2& e0, const glm::vec2& e1, bool keepLeft )
        {
            const glm::vec2 e    = e1 - e0;
            auto            side = [&]( const glm::vec2& x )
            {
                const glm::vec2 d = x - e0;
                const float     s = e.x * d.y - e.y * d.x;
                return keepLeft ? s : -s;
            };
            Poly2 out;
            for ( size_t i = 0; i < p.size(); ++i )
            {
                const glm::vec2& a  = p[i];
                const glm::vec2& b  = p[( i + 1 ) % p.size()];
                const float      sa = side( a );
                const float      sb = side( b );
                if ( sa >= 0.0f )
                    out.push_back( a );
                if ( ( sa > 0.0f && sb < 0.0f ) || ( sa < 0.0f && sb > 0.0f ) )
                    out.push_back( a + ( b - a ) * ( sa / ( sa - sb ) ) );
            }
            return out;
        }

        // Face f of `a` and the opposite face of its same-layer neighbour `b`, when no corner of either has moved
        // off their common plane and at least one of them is deformed: the two faces then overlap only partly -
        // a ramp's side next to a flat block, the wall of a sloped wall's neighbour, two slopes along different
        // axes side by side - so neither is hidden nor shown whole, and each shows the part the other does not
        // cover. Both faces are convex (each corner moves along one axis, a face spans two straight lines), so
        // a's face minus b's is a's face cut by the half-planes of b's edges: one convex piece outside each edge
        // and inside the ones before it. Returns those pieces, or nullopt when the all-or-nothing rule of
        // FaceHidden applies (both flat, or a corner off the plane: the faces do not lie on each other).
        std::optional<std::vector<Poly2>> UncoveredPieces( const Cell& a, const Cell& b, int f )
        {
            if ( a.IsFlat() && b.IsFlat() )
                return std::nullopt;
            const int n        = FaceNormalAxis( f );
            const int a0       = ( n + 1 ) % 3;
            const int a1       = ( n + 2 ) % 3;
            const int bit      = kFaceAxisBit[f];
            auto      facePoly = [&]( const Cell& cell, bool neighbour, Poly2& out )
            {
                for ( int k = 0; k < 4; ++k )
                {
                    const int   i   = neighbour ? ( kFaceCorner[f][k] ^ bit ) : kFaceCorner[f][k];
                    const float off = static_cast<float>( cell.V[i] ) / static_cast<float>( CornerDen );
                    if ( off != 0.0f && cell.Axis == n )
                        return false;
                    glm::vec2 q( static_cast<float>( ( i >> a0 ) & 1 ), static_cast<float>( ( i >> a1 ) & 1 ) );
                    if ( off != 0.0f )
                        q[cell.Axis == a0 ? 0 : 1] += off;
                    if ( out.empty() || glm::length( q - out.back() ) > 1e-5f )
                        out.push_back( q );
                }
                if ( out.size() > 1 && glm::length( out.back() - out.front() ) <= 1e-5f )
                    out.pop_back();
                if ( SignedArea( out ) < 0.0f )
                    std::ranges::reverse( out );
                return true;
            };
            Poly2 A;
            Poly2 B;
            if ( !facePoly( a, false, A ) || !facePoly( b, true, B ) )
                return std::nullopt;

            constexpr float    kEps = 1e-6f;
            std::vector<Poly2> pieces;
            if ( A.size() < 3 || SignedArea( A ) <= kEps )
                return pieces;
            if ( B.size() < 3 || SignedArea( B ) <= kEps )
            {
                pieces.push_back( A );
                return pieces;
            }
            Poly2 inside = A;
            for ( size_t i = 0; i < B.size() && inside.size() >= 3; ++i )
            {
                const glm::vec2& e0  = B[i];
                const glm::vec2& e1  = B[( i + 1 ) % B.size()];
                Poly2            out = ClipHalfPlane( inside, e0, e1, false );
                if ( out.size() >= 3 && SignedArea( out ) > kEps )
                    pieces.push_back( std::move( out ) );
                inside = ClipHalfPlane( inside, e0, e1, true );
                if ( SignedArea( inside ) <= kEps )
                    break;
            }
            return pieces;
        }
    } // namespace

    uint64_t Pack( const glm::ivec3& c )
    {
        const uint64_t x = static_cast<uint64_t>( c.x + OFF ) & 0x1FFFFF;
        const uint64_t y = static_cast<uint64_t>( c.y + OFF ) & 0x1FFFFF;
        const uint64_t z = static_cast<uint64_t>( c.z + OFF ) & 0x1FFFFF;
        return x | ( y << 21 ) | ( z << 42 );
    }

    glm::ivec3 Unpack( uint64_t k )
    {
        return { static_cast<int>( k & 0x1FFFFF ) - OFF, static_cast<int>( ( k >> 21 ) & 0x1FFFFF ) - OFF,
                 static_cast<int>( ( k >> 42 ) & 0x1FFFFF ) - OFF };
    }

    int FloorDiv( int a, int b )
    {
        return a >= 0 ? a / b : -( ( -a + b - 1 ) / b );
    }

    bool GridFrame::SameAxes( const GridFrame& o ) const
    {
        return std::abs( glm::dot( Rotation, o.Rotation ) ) > 1.0f - 1e-6f;
    }

    bool GridFrame::SameAs( const GridFrame& o ) const
    {
        return SameAxes( o ) && !glm::any( glm::greaterThan( glm::abs( Origin - o.Origin ), glm::vec3( 1e-3f ) ) );
    }

    GridFrame MakeGridFrame( const glm::vec3& origin, const glm::vec3& eulerDegrees )
    {
        return GridFrame{ origin, glm::normalize( glm::quat( glm::radians( eulerDegrees ) ) ) };
    }

    glm::vec3 NearestFaceCorner( const GridFrame& frame, const glm::ivec3& cell, const glm::ivec3& normal,
                                 float unit, const glm::vec3& rayOrigin, const glm::vec3& rayDir )
    {
        // The face's axis and the two in-plane ones; the face sits on the cell's far side along +normal.
        const int na = normal.x != 0 ? 0 : normal.y != 0 ? 1 : 2;
        const int ua = ( na + 1 ) % 3;
        const int va = ( na + 2 ) % 3;

        // Grid-space ray, as UE does: the distance ranking is the same in any frame a rotation reaches.
        const glm::vec3 o      = frame.ToFramePoint( rayOrigin );
        const glm::vec3 d      = glm::normalize( frame.ToFrameVector( rayDir ) );
        auto            distSq = [&]( const glm::vec3& p )
        {
            const float     t = std::max( 0.0f, glm::dot( p - o, d ) );
            const glm::vec3 q = o + d * t - p;
            return glm::dot( q, q );
        };

        glm::vec3 best( 0.0f );
        float     bestD = std::numeric_limits<float>::max();
        for ( int k = 0; k < 4; ++k )
        {
            glm::vec3 p( cell );
            p[na] += normal[na] > 0 ? 1.0f : 0.0f;
            p[ua] += static_cast<float>( k & 1 );
            p[va] += static_cast<float>( ( k >> 1 ) & 1 );
            p *= unit;
            const float dd = distSq( p );
            if ( dd < bestD )
            {
                bestD = dd;
                best  = p;
            }
        }
        return frame.ToWorldPoint( best );
    }

    glm::vec3 CornerPos( const glm::ivec3& c, const Cell& cell, int i, float unit )
    {
        glm::vec3 p( c.x + ( ( ( i & 1 ) != 0 ) ? 1 : 0 ), c.y + ( ( ( i & 2 ) != 0 ) ? 1 : 0 ),
                     c.z + ( ( ( i & 4 ) != 0 ) ? 1 : 0 ) );
        p *= unit;
        p[cell.Axis] += static_cast<float>( cell.V[i] ) / static_cast<float>( CornerDen ) * unit;
        return p;
    }

    void RescaleSelection( WorkPlane& plane, glm::ivec2& anchor, Rect& sel, float oldUnit, float newUnit, int K )
    {
        const float s = oldUnit / newUnit;
        auto toNewMin = [&]( int c ) { return static_cast<int>( std::floor( static_cast<float>( c ) * s ) ); };
        auto toNewMax = [&]( int c )
        { return static_cast<int>( std::ceil( static_cast<float>( c + 1 ) * s ) ) - 1; };

        const float planeW = static_cast<float>( plane.Cell + ( plane.Sign > 0 ? 0 : 1 ) ) * oldUnit;
        plane.Cell         = static_cast<int>( std::lround( planeW / newUnit ) ) - ( plane.Sign > 0 ? 0 : 1 );
        anchor             = { toNewMin( anchor.x ), toNewMin( anchor.y ) };

        const int uMin = toNewMin( sel.UMin );
        const int uMax = toNewMax( sel.UMax );
        const int vMin = toNewMin( sel.VMin );
        const int vMax = toNewMax( sel.VMax );
        sel.UMin       = FloorDiv( uMin, K ) * K;
        sel.UMax       = FloorDiv( uMax, K ) * K + K - 1;
        sel.VMin       = FloorDiv( vMin, K ) * K;
        sel.VMax       = FloorDiv( vMax, K ) * K + K - 1;
    }

    bool Volume::SolidAt( const glm::ivec3& c, float unit, const GridFrame& frame ) const
    {
        // Layer units are always commensurate in practice (the base only ever halves), so a coarser layer maps
        // with one FloorDiv. A layer FINER than `unit`, or one built in a DIFFERENT grid frame (its lattice
        // doesn't line up at all), is skipped - conservative: it can only ever leave a hidden interior face.
        auto inLayer = [&]( const CellMap& cells, float lu, const GridFrame& lf )
        {
            if ( cells.empty() || lu <= 0.0f || !lf.SameAs( frame ) )
                return false;
            const int R = static_cast<int>( std::lround( lu / unit ) );
            if ( R < 1 || std::abs( lu - static_cast<float>( R ) * unit ) > 0.001f * unit )
                return false;
            return cells.contains( Pack( { FloorDiv( c.x, R ), FloorDiv( c.y, R ), FloorDiv( c.z, R ) } ) );
        };
        if ( inLayer( m_Cells, m_Unit, m_Frame ) )
            return true;
        return std::ranges::any_of( m_Frozen,
                                    [&inLayer]( const Layer& l ) { return inLayer( l.Cells, l.Unit, l.Frame ); } );
    }

    bool Volume::FaceHidden( const CellMap& cells, const glm::ivec3& c, const Cell& data, int f, float unit,
                             const GridFrame& frame ) const
    {
        const glm::ivec3 n  = c + kNeighbor[f];
        const auto       it = cells.find( Pack( n ) );
        if ( it != cells.end() )
        {
            // Same layer: the shared quad only exists if all four corners line up on both boxes - that is what
            // keeps a ramp's slanted face visible while the flat faces under it stay culled.
            const int bit = kFaceAxisBit[f];
            for ( int k = 0; k < 4; ++k )
            {
                const int i = kFaceCorner[f][k];
                if ( !SameCorner( data, i, it->second, i ^ bit ) )
                    return false;
            }
            return true;
        }
        // Another layer's solid can only hide a face of an UNDEFORMED cell (we don't know its corners).
        return data.IsFlat() && SolidAt( n, unit, frame );
    }

    void Volume::Refine( int F )
    {
        CellMap out;
        out.reserve( m_Cells.size() * static_cast<size_t>( F * F * F ) );
        for ( const auto& [k, cell] : m_Cells )
        {
            const glm::ivec3 c = Unpack( k );
            for ( int dx = 0; dx < F; ++dx )
                for ( int dy = 0; dy < F; ++dy )
                    for ( int dz = 0; dz < F; ++dz )
                    {
                        // The children keep the parent's face materials; the corner offsets are not carried
                        // (a deformed piece is frozen before a refine, see the tool).
                        Cell child;
                        std::copy( std::begin( cell.Mat ), std::end( cell.Mat ), std::begin( child.Mat ) );
                        out[Pack( { c.x * F + dx, c.y * F + dy, c.z * F + dz } )] = child;
                    }
        }
        m_Cells = std::move( out );
        m_Unit /= static_cast<float>( F );
    }

    bool Volume::Freeze()
    {
        if ( m_Cells.empty() )
            return false;
        Layer l;
        l.Cells  = std::move( m_Cells );
        l.Unit   = m_Unit;
        l.Frame  = m_Frame;
        m_Frozen.push_back( std::move( l ) );
        m_Cells.clear();
        m_Unit = -1.0f; // the next Block Size becomes the base of a brand-new volume
        return true;
    }

    void Volume::PushPull( WorkPlane& plane, const Rect& sel, int dir, int height, uint8_t material )
    {
        Cell fresh;
        std::fill( std::begin( fresh.Mat ), std::end( fresh.Mat ), material );
        auto      occ  = [&]( const glm::ivec3& c ) { return SolidAt( c, m_Unit, m_Frame ); };
        const int na   = plane.Na;
        const int sign = plane.Sign;
        const int ua   = ( na + 1 ) % 3;
        const int va   = ( na + 2 ) % 3;

        if ( dir > 0 ) // Extrude out: fill `height` base cells from the first empty cell, per column
        {
            for ( int u = sel.UMin; u <= sel.UMax; ++u )
                for ( int v = sel.VMin; v <= sel.VMax; ++v )
                {
                    glm::ivec3 c{ 0 };
                    c[na] = plane.Cell;
                    c[ua] = u;
                    c[va] = v;
                    while ( occ( c ) )
                        c[na] += sign;
                    for ( int s = 0; s < height; ++s, c[na] += sign )
                        m_Cells[Pack( c )] = fresh;
                }
            plane.Cell += sign * height;
        }
        else // Push in: remove `height` base cells just inside the plane, per column
        {
            std::vector<glm::ivec3> cut;
            for ( int u = sel.UMin; u <= sel.UMax; ++u )
                for ( int v = sel.VMin; v <= sel.VMax; ++v )
                {
                    glm::ivec3 c{ 0 };
                    c[na] = plane.Cell - sign;
                    c[ua] = u;
                    c[va] = v;
                    for ( int s = 0; s < height; ++s, c[na] -= sign )
                        cut.push_back( c );
                }
            ThawUnder( cut );
            std::vector<glm::ivec3> removed;
            for ( const glm::ivec3& c : cut )
                if ( m_Cells.erase( Pack( c ) ) != 0 )
                    removed.push_back( c );
            // The walls and floor of the hole are new faces of the cells around it: they take the op's
            // material. A removed cell's neighbour sees it through the opposite face (f ^ 1 in kFace order).
            for ( const glm::ivec3& r : removed )
                for ( int f = 0; f < 6; ++f )
                {
                    const auto it = m_Cells.find( Pack( r + kNeighbor[f] ) );
                    if ( it != m_Cells.end() )
                        it->second.Mat[f ^ 1] = material;
                }
            plane.Cell -= sign * height;
        }
    }

    Common::BoolResultStr Volume::ApplyCornerHeights( const WorkPlane& plane, const Rect& sel,
                                                      const CornerHeights& heights, bool crosswise )
    {
        const int na     = plane.Na;
        const int ua     = ( na + 1 ) % 3;
        const int va     = ( na + 2 ) % 3;
        const int inside = plane.Cell - plane.Sign; // the cell row whose outer face is the work-plane (PushPull)
        const int outer  = plane.Sign > 0 ? 1 << na : 0;

        const auto spanU = static_cast<float>( sel.UMax + 1 - sel.UMin );
        const auto spanV = static_cast<float>( sel.VMax + 1 - sel.VMin );
        if ( spanU <= 0.0f || spanV <= 0.0f )
            return Common::MakeFormattedError<bool>(
                 "CubeGrid Corner Mode: the selection {}..{} x {}..{} is empty", sel.UMin, sel.UMax, sel.VMin,
                 sel.VMax );

        // The blend never leaves the posts' range, so checking the posts checks every corner it writes.
        for ( const int h : heights )
            if ( h < std::numeric_limits<int16_t>::min() || h > std::numeric_limits<int16_t>::max() )
                return Common::MakeFormattedError<bool>(
                     "CubeGrid Corner Mode: post height {} does not fit 16 bits", h );
        auto heightAt = [&]( int lu, int lv )
        {
            const float fu = ( static_cast<float>( lu - sel.UMin ) ) / spanU;
            const float fv = ( static_cast<float>( lv - sel.VMin ) ) / spanV;
            const float a  = glm::mix( static_cast<float>( heights[0] ), static_cast<float>( heights[1] ), fu );
            const float b  = glm::mix( static_cast<float>( heights[2] ), static_cast<float>( heights[3] ), fu );
            return static_cast<int>( std::lround( glm::mix( a, b, fv ) ) );
        };

        ThawUnder( InsideRow( plane, sel ) );
        std::vector<std::pair<glm::ivec3, Cell*>> under;
        for ( int uu = sel.UMin; uu <= sel.UMax; ++uu )
            for ( int vv = sel.VMin; vv <= sel.VMax; ++vv )
            {
                glm::ivec3 c{ 0 };
                c[na]   = inside;
                c[ua]   = uu;
                c[va]   = vv;
                auto it = m_Cells.find( Pack( c ) );
                if ( it == m_Cells.end() )
                    continue; // nothing pushed out under this column yet
                if ( !it->second.IsFlat() && it->second.Axis != na )
                    return Common::MakeFormattedError<bool>(
                         "CubeGrid Corner Mode: cell ({}, {}, {}) already slopes along axis {}; a cell slopes "
                         "along one axis, and this face moves its corners along axis {}",
                         c.x, c.y, c.z, it->second.Axis, na );
                under.emplace_back( c, &it->second );
            }

        for ( auto& [c, cell] : under )
        {
            cell->Axis      = static_cast<uint8_t>( na );
            cell->Crosswise = crosswise;
            for ( int i = 0; i < 8; ++i )
            {
                if ( ( i & ( 1 << na ) ) != outer ) // the inner corners stay on the lattice, so the cell behind
                    continue;                       // the sloped row still meets it
                cell->V[i] =
                     static_cast<int16_t>( plane.Sign * heightAt( c[ua] + ( ( i & ( 1 << ua ) ) != 0 ? 1 : 0 ),
                                                                  c[va] + ( ( i & ( 1 << va ) ) != 0 ? 1 : 0 ) ) );
            }
        }
        return Common::MakeSuccess( true );
    }

    std::vector<glm::ivec3> Volume::InsideRow( const WorkPlane& plane, const Rect& sel )
    {
        std::vector<glm::ivec3> row;
        const int               ua = ( plane.Na + 1 ) % 3;
        const int               va = ( plane.Na + 2 ) % 3;
        for ( int uu = sel.UMin; uu <= sel.UMax; ++uu )
            for ( int vv = sel.VMin; vv <= sel.VMax; ++vv )
            {
                glm::ivec3 c{ 0 };
                c[plane.Na] = plane.Cell - plane.Sign;
                c[ua]       = uu;
                c[va]       = vv;
                row.push_back( c );
            }
        return row;
    }

    CornerHeights Volume::ReadCornerHeights( const WorkPlane& plane, const Rect& sel )
    {
        ThawUnder( InsideRow( plane, sel ) );
        CornerHeights h{};
        const int     ua = ( plane.Na + 1 ) % 3;
        const int     va = ( plane.Na + 2 ) % 3;
        for ( int k = 0; k < 4; ++k )
        {
            glm::ivec3 c{ 0 };
            c[plane.Na] = plane.Cell - plane.Sign;
            c[ua]       = kPosts[k].AtUMax ? sel.UMax : sel.UMin;
            c[va]       = kPosts[k].AtVMax ? sel.VMax : sel.VMin;
            if ( auto it = m_Cells.find( Pack( c ) ); it != m_Cells.end() && it->second.Axis == plane.Na )
                h[k] = plane.Sign * it->second.V[PostCorner( plane, k )];
        }
        return h;
    }

    int Volume::PaintFaces( const WorkPlane& plane, const Rect& sel, uint8_t material )
    {
        if ( m_Unit <= 0.0f )
            return 0;
        const int na   = plane.Na;
        const int ua   = ( na + 1 ) % 3;
        const int va   = ( na + 2 ) % 3;
        const int sign = plane.Sign;
        int       face = 0; // the face whose outward normal is the plane's
        for ( int f = 0; f < 6; ++f )
            if ( kNeighbor[f][na] == sign )
                face = f;

        // The plane and the rectangle in the ACTIVE frame: every layer has its own cell size and grid frame,
        // and the selection (in the active base) may sit on a face of any of them.
        const float planeW = static_cast<float>( plane.Cell + ( sign > 0 ? 0 : 1 ) ) * m_Unit;
        const float uLo    = static_cast<float>( sel.UMin ) * m_Unit;
        const float uHi    = static_cast<float>( sel.UMax + 1 ) * m_Unit;
        const float vLo    = static_cast<float>( sel.VMin ) * m_Unit;
        const float vHi    = static_cast<float>( sel.VMax + 1 ) * m_Unit;

        int  painted    = 0;
        auto paintLayer = [&]( CellMap& cells, float lu, const GridFrame& lf )
        {
            // A turned layer's faces never lie on this lattice's planes; a shifted one's do, offset by `lo`.
            if ( !lf.SameAxes( m_Frame ) )
                return;
            const glm::vec3 lo  = m_Frame.ToFramePoint( lf.Origin );
            const float     eps = 1e-3f * std::min( lu, m_Unit );
            for ( auto& [key, cell] : cells )
            {
                const glm::ivec3 c     = Unpack( key );
                const float      faceW = static_cast<float>( c[na] + ( sign > 0 ? 1 : 0 ) ) * lu + lo[na];
                if ( std::abs( faceW - planeW ) > eps )
                    continue;
                const float cu = ( static_cast<float>( c[ua] ) + 0.5f ) * lu + lo[ua];
                const float cv = ( static_cast<float>( c[va] ) + 0.5f ) * lu + lo[va];
                if ( cu < uLo || cu > uHi || cv < vLo || cv > vHi )
                    continue;
                if ( FaceHidden( cells, c, cell, face, lu, lf ) )
                    continue;
                cell.Mat[face] = material;
                ++painted;
            }
        };
        paintLayer( m_Cells, m_Unit, m_Frame );
        for ( Layer& l : m_Frozen )
            paintLayer( l.Cells, l.Unit, l.Frame );
        return painted;
    }

    RenderMeshData Volume::Bake() const
    {
        // Quads are collected per material ID and laid out one contiguous submesh per ID afterwards
        // (ascending - std::map), because a submesh is a vertex range + an index range.
        struct Bucket
        {
            std::vector<Vertex> Verts;
            std::vector<Index>  Inds; // bucket-local, which is what submesh-local indices are
        };
        std::map<int, Bucket> buckets;

        // Quads are collected first and written out once every layer is meshed: an edge can only be checked
        // for T-junctions against the corners of ALL quads (see below). `P` and `N` are in the layer's FRAME.
        struct PendingQuad
        {
            int                      Material;
            std::array<glm::vec3, 4> P;
            int                      Count;  // 3 when two corners of the quad fell together
            bool                     Diag13; // split along corners 1-3 instead of 0-2 (SplitsAlong13)
            glm::vec3                N;
            FaceUvFrame              Uv;
            GridFrame                Frame;
        };
        std::vector<PendingQuad> quads;
        float                    minUnit = std::numeric_limits<float>::max();
        // A corner pulled onto its neighbour (a slope down to nothing, a piece ending in a point) leaves a
        // triangle; a doubled corner would make a zero-area triangle whose edges have no partner.
        auto emitQuad = [&]( int material, const glm::vec3 p[4], const glm::vec3& nrm, const FaceUvFrame& uv,
                             const GridFrame& frame, bool diag13 )
        {
            PendingQuad q{ material, {}, 0, diag13, nrm, uv, frame };
            for ( int k = 0; k < 4; ++k )
                if ( q.Count == 0 || glm::length( p[k] - q.P[q.Count - 1] ) > 1e-4f )
                    q.P[q.Count++] = p[k];
            if ( q.Count > 1 && glm::length( q.P[q.Count - 1] - q.P[0] ) <= 1e-4f )
                --q.Count;
            if ( q.Count >= 3 )
                quads.push_back( q );
        };

        auto uncovered = []( const CellMap& cells, const glm::ivec3& c, const Cell& cell,
                             int f ) -> std::optional<std::vector<Poly2>>
        {
            const auto it = cells.find( Pack( c + kNeighbor[f] ) );
            if ( it == cells.end() )
                return std::nullopt;
            return UncoveredPieces( cell, it->second, f );
        };
        // One piece of UncoveredPieces as quads (and a last triangle) fanned from its first corner, facing out.
        auto emitPiece =
             [&]( const glm::ivec3& c, int f, const Poly2& piece, float lu, const GridFrame& lf, int material )
        {
            const int n  = FaceNormalAxis( f );
            const int a0 = ( n + 1 ) % 3;
            const int a1 = ( n + 2 ) % 3;
            auto      at = [&]( const glm::vec2& q )
            {
                glm::vec3 p( c );
                p[n] += ( kFaceCorner[f][0] & ( 1 << n ) ) != 0 ? 1.0f : 0.0f;
                p[a0] += q.x;
                p[a1] += q.y;
                return p * lu;
            };
            // Counter-clockwise in (a0, a1) faces +n; a face on the low side looks along -n.
            const bool flip = kFace[f][0].N[n] < 0.0f;
            for ( size_t k = 1; k + 1 < piece.size(); k += 2 )
            {
                const size_t k2 = std::min( k + 2, piece.size() - 1 );
                glm::vec3    p[4]{ at( piece[0] ), at( piece[k] ), at( piece[k + 1] ), at( piece[k2] ) };
                if ( flip )
                    std::swap( p[1], p[3] );
                emitQuad( material, p, kFace[f][0].N, UvFrame( f ), lf, false );
            }
        };
        // Mixed Block Sizes (M10c). A coarse face that a FINER layer's blocks sit against is neither hidden nor
        // shown whole: SolidAt reads the coarser layers only, so the fine blocks' faces towards it are culled
        // while it stayed whole underneath them - doubled where they touch, and their side walls ending on its
        // inside with no partner edge. UE's Cube Grid keeps one grid per piece and joins pieces at commit
        // through a mesh boolean (CubeGridBooleanOp); on our lattices that union is exact without one: the
        // finer lattice divides the coarse one, so the coarse face is cut into fine squares and only the
        // squares no fine flat block covers are shown. The T-junction pass below then conforms the edges.
        struct LayerView
        {
            const CellMap* Cells;
            float          Unit;
            GridFrame      Frame;
        };
        std::vector<LayerView> layers;
        layers.push_back( { &m_Cells, m_Unit, m_Frame } );
        for ( const Layer& l : m_Frozen )
            layers.push_back( { &l.Cells, l.Unit, l.Frame } );

        // Face f of the flat cell c of a layer (edge lu, frame lf), split into R x R squares on the finest
        // lattice that divides lu among the other layers of that frame: `covered` gets one flag per square
        // (U fastest). Returns 0 when nothing finer touches the face, 1 when part of it is covered, 2 when all
        // of it is. A fine DEFORMED block covers nothing - its face towards us is not culled either.
        struct Coverage
        {
            int               State = 0;
            int               R     = 1;
            std::vector<bool> Covered;
        };
        auto coverage = [&]( const glm::ivec3& c, int f, float lu, const GridFrame& lf )
        {
            Coverage out;
            float    finest = lu;
            for ( const LayerView& l : layers )
                if ( !l.Cells->empty() && l.Unit > 0.0f && l.Unit < lu * 0.999f && l.Frame.SameAs( lf ) )
                    finest = std::min( finest, l.Unit );
            if ( finest >= lu )
                return out;
            const int R = static_cast<int>( std::lround( lu / finest ) );
            if ( std::abs( lu - static_cast<float>( R ) * finest ) > 0.001f * lu )
                return out;
            out.R = R;
            out.Covered.assign( static_cast<size_t>( R ) * R, false );

            const VoxelFaceAxes ax    = FaceAxes( f );
            const glm::vec3     unitP = kFace[f][0].P + 0.5f;
            glm::ivec3          g     = c * R;
            g[ax.Normal]              = unitP[ax.Normal] > 0.5f ? ( c[ax.Normal] + 1 ) * R : c[ax.Normal] * R - 1;
            int count                 = 0;
            for ( int j = 0; j < R; ++j )
                for ( int i = 0; i < R; ++i )
                {
                    glm::ivec3 q = g;
                    q[ax.U] += i;
                    q[ax.V] += j;
                    for ( const LayerView& l : layers )
                    {
                        if ( l.Cells->empty() || l.Unit >= lu * 0.999f || !l.Frame.SameAs( lf ) )
                            continue;
                        const int Rl = static_cast<int>( std::lround( l.Unit / finest ) );
                        if ( Rl < 1 || std::abs( l.Unit - static_cast<float>( Rl ) * finest ) > 0.001f * l.Unit )
                            continue;
                        const auto it = l.Cells->find(
                             Pack( { FloorDiv( q.x, Rl ), FloorDiv( q.y, Rl ), FloorDiv( q.z, Rl ) } ) );
                        if ( it != l.Cells->end() && it->second.IsFlat() )
                        {
                            out.Covered[static_cast<size_t>( j ) * R + i] = true;
                            ++count;
                            break;
                        }
                    }
                }
            if ( count == 0 )
                out.State = 0;
            else if ( count == R * R )
                out.State = 2;
            else
                out.State = 1;
            return out;
        };
        // The uncovered squares of a partly covered face, one quad per run along U in each row of V.
        auto emitUncovered =
             [&]( const glm::ivec3& c, int f, const Coverage& cov, float lu, const GridFrame& lf, int material )
        {
            const VoxelFaceAxes ax = FaceAxes( f );
            const int           R  = cov.R;
            for ( int j = 0; j < R; ++j )
                for ( int i = 0; i < R; )
                {
                    if ( cov.Covered[static_cast<size_t>( j ) * R + i] )
                    {
                        ++i;
                        continue;
                    }
                    int end = i;
                    while ( end < R && !cov.Covered[static_cast<size_t>( j ) * R + end] )
                        ++end;
                    glm::vec3 p[4];
                    for ( int k = 0; k < 4; ++k )
                    {
                        const glm::vec3 unitP = kFace[f][k].P + 0.5f;
                        glm::vec3       gp( c );
                        gp[ax.Normal] += unitP[ax.Normal];
                        gp[ax.U] += ( static_cast<float>( i ) + unitP[ax.U] * static_cast<float>( end - i ) ) /
                                    static_cast<float>( R );
                        gp[ax.V] += ( static_cast<float>( j ) + unitP[ax.V] ) / static_cast<float>( R );
                        p[k] = gp * lu;
                    }
                    emitQuad( material, p, kFace[f][0].N, UvFrame( f ), lf, false );
                    i = end;
                }
        };

        auto emitLayer = [&]( const CellMap& cells, float lu, const GridFrame& lf )
        {
            // FLAT cells go through greedy meshing: a 20x8 blockout wall becomes ONE quad instead of 160.
            // Corner-deformed cells keep their own per-face quads - their corners are not coplanar with a
            // neighbour's, so merging them would flatten the ramp.
            std::vector<glm::ivec3> flat;
            flat.reserve( cells.size() );
            for ( const auto& [key, cell] : cells )
                if ( cell.IsFlat() )
                    flat.push_back( Unpack( key ) );

            const auto merged = GreedyMeshFaces(
                 flat,
                 [&]( const glm::ivec3& c, int f )
                 {
                     const auto it = cells.find( Pack( c ) );
                     return it != cells.end() && !FaceHidden( cells, c, it->second, f, lu, lf ) &&
                            !uncovered( cells, c, it->second, f ) && coverage( c, f, lu, lf ).State == 0;
                 },
                 [&]( const glm::ivec3& c, int f ) -> uint64_t { return cells.at( Pack( c ) ).Mat[f]; } );

            for ( const auto& q : merged )
            {
                const VoxelFaceAxes ax = FaceAxes( q.Face );
                const FaceUvFrame   uv = UvFrame( q.Face );

                // The face's unit-cube corners, stretched over the merged run: the 0/1 offset along each
                // in-plane axis becomes 0/Size, which keeps the table's winding (and so the normal).
                glm::vec3 p[4];
                for ( int k = 0; k < 4; ++k )
                {
                    const glm::vec3 unitP = kFace[q.Face][k].P + 0.5f; // 0 or 1 per axis
                    glm::vec3       g( q.Cell );
                    g[ax.Normal] += unitP[ax.Normal];
                    g[ax.U] += unitP[ax.U] * static_cast<float>( q.SizeU );
                    g[ax.V] += unitP[ax.V] * static_cast<float>( q.SizeV );
                    p[k] = g * lu;
                }
                emitQuad( cells.at( Pack( q.Cell ) ).Mat[q.Face], p, kFace[q.Face][0].N, uv, lf, false );
            }

            for ( const auto& [key, cell] : cells )
            {
                const glm::ivec3 c = Unpack( key );
                for ( int f = 0; f < 6; ++f )
                {
                    if ( FaceHidden( cells, c, cell, f, lu, lf ) ) // only Solid/Empty borders
                        continue;
                    if ( const auto pieces = uncovered( cells, c, cell, f ) )
                    {
                        for ( const Poly2& piece : *pieces )
                            emitPiece( c, f, piece, lu, lf, cell.Mat[f] );
                        continue;
                    }
                    if ( cell.IsFlat() )
                    {
                        // Greedy-meshed above, unless finer blocks cover part of it.
                        if ( const Coverage cov = coverage( c, f, lu, lf ); cov.State == 1 )
                            emitUncovered( c, f, cov, lu, lf, cell.Mat[f] );
                        continue;
                    }
                    // Corner Mode can slant a quad, so the normal comes from the actual corners.
                    glm::vec3 p[4];
                    for ( int k = 0; k < 4; ++k )
                        p[k] = CornerPos( c, cell, kFaceCorner[f][k], lu );
                    glm::vec3 nrm =
                         glm::cross( p[1] - p[0], p[3] - p[0] ) + glm::cross( p[3] - p[2], p[1] - p[2] );
                    nrm = glm::dot( nrm, nrm ) > 1e-12f ? glm::normalize( nrm ) : kFace[f][0].N;

                    emitQuad( cell.Mat[f], p, nrm, UvFrame( f ), lf, SplitsAlong13( cell, f ) );
                }
            }
        };
        if ( !m_Cells.empty() )
            minUnit = m_Unit;
        emitLayer( m_Cells, m_Unit, m_Frame );
        for ( const Layer& l : m_Frozen )
        {
            if ( !l.Cells.empty() )
                minUnit = std::min( minUnit, l.Unit );
            emitLayer( l.Cells, l.Unit, l.Frame );
        }

        // T-junctions. Greedy merging, and a layer meshed beside another, leave quad corners lying INSIDE a
        // neighbouring quad's edge: the floor ring around a block pushed out of it is the everyday case (its
        // long strip runs past the block's corners). Welded by position, such an edge has a triangle on one
        // side only and its end vertices are bowties, which the editable mesh refuses
        // (DynamicMesh3::CheckValidity: a vertex's triangle count must equal its edge count, or one less).
        // So every quad edge is split at each corner of any quad lying on it, and a quad with a split edge is
        // fanned from its centre - the surface and its attributes stay exactly the same, only conforming.
        std::vector<glm::vec3> corners; // world, sorted by x for a range lookup per edge
        corners.reserve( quads.size() * 4 );
        for ( const PendingQuad& q : quads )
            for ( const glm::vec3& p : q.P )
                corners.push_back( q.Frame.ToWorldPoint( p ) );
        std::ranges::sort( corners, []( const glm::vec3& a, const glm::vec3& b ) { return a.x < b.x; } );
        const float eps = 1e-3f * minUnit;

        std::vector<glm::vec3> ring;  // frame-space outline of the quad being written, split points included
        std::vector<float>     split; // parameters along one edge
        for ( const PendingQuad& q : quads )
        {
            ring.clear();
            for ( int k = 0; k < q.Count; ++k )
            {
                const glm::vec3& la = q.P[k];
                const glm::vec3& lb = q.P[( k + 1 ) % q.Count];
                const glm::vec3  a  = q.Frame.ToWorldPoint( la );
                const glm::vec3  d  = q.Frame.ToWorldPoint( lb ) - a;
                const float      l2 = glm::dot( d, d );
                ring.push_back( la );
                if ( l2 <= eps * eps )
                    continue;
                const float tEps = eps / std::sqrt( l2 );
                split.clear();
                const float xLo = std::min( a.x, a.x + d.x ) - eps;
                const float xHi = std::max( a.x, a.x + d.x ) + eps;
                auto it = std::ranges::lower_bound( corners, xLo, {}, []( const glm::vec3& c ) { return c.x; } );
                for ( ; it != corners.end() && it->x <= xHi; ++it )
                {
                    const float t = glm::dot( *it - a, d ) / l2;
                    if ( t <= tEps || t >= 1.0f - tEps || glm::length( a + d * t - *it ) > eps )
                        continue;
                    split.push_back( t );
                }
                std::ranges::sort( split );
                float last = 0.0f;
                for ( const float t : split )
                    if ( t - last > tEps ) // the same corner appears once per quad that owns it
                    {
                        ring.push_back( glm::mix( la, lb, t ) );
                        last = t;
                    }
            }

            // UVs are taken in the frame (frame-aligned, so the texture turns with the grid) with a tangent
            // frame that matches them (a normal map on a merged quad lines up with its texture); everything
            // is then carried into the world by the frame.
            Bucket&    b      = buckets[q.Material];
            const auto base   = static_cast<uint32_t>( b.Verts.size() );
            auto       vertex = [&]( const glm::vec3& p )
            {
                Vertex v{};
                v.Position  = q.Frame.ToWorldPoint( p );
                v.Normal    = q.Frame.ToWorldVector( q.N );
                v.Tangent   = q.Frame.ToWorldVector( q.Uv.T );
                v.Bitangent = q.Frame.ToWorldVector( q.Uv.B );
                v.TexCoord  = { glm::dot( p, q.Uv.T ) * kUvPerUnit, glm::dot( p, q.Uv.B ) * kUvPerUnit };
                b.Verts.push_back( v );
            };
            if ( ring.size() == static_cast<size_t>( q.Count ) )
            {
                for ( const glm::vec3& p : ring )
                    vertex( p );
                if ( q.Count == 3 )
                    b.Inds.push_back( { base + 0, base + 1, base + 2 } );
                else if ( q.Diag13 )
                {
                    b.Inds.push_back( { base + 0, base + 1, base + 3 } );
                    b.Inds.push_back( { base + 1, base + 2, base + 3 } );
                }
                else
                {
                    b.Inds.push_back( { base + 0, base + 1, base + 2 } );
                    b.Inds.push_back( { base + 2, base + 3, base + 0 } );
                }
                continue;
            }
            // A split quad: a fan from its centre keeps every outline segment an edge of its own triangle, in
            // the quad's winding.
            glm::vec3 centre( 0.0f );
            for ( int k = 0; k < q.Count; ++k )
                centre += q.P[k];
            vertex( centre / static_cast<float>( q.Count ) );
            for ( const glm::vec3& p : ring )
                vertex( p );
            const auto n = static_cast<uint32_t>( ring.size() );
            for ( uint32_t i = 0; i < n; ++i )
                b.Inds.push_back( { base, base + 1 + i, base + 1 + ( i + 1 ) % n } );
        }

        RenderMeshData out;
        for ( auto& [material, b] : buckets )
        {
            Submesh sm{};
            sm.Name         = "MaterialID " + std::to_string( material );
            sm.VertexOffset = static_cast<uint32_t>( out.Vertices.size() );
            sm.VertexCount  = static_cast<uint32_t>( b.Verts.size() );
            sm.IndexOffset  = static_cast<uint32_t>( out.Indices.size() * 3 ); // uint32 units
            sm.IndexCount   = static_cast<uint32_t>( b.Inds.size() * 3 );
            sm.Transform    = glm::mat4( 1.0f );
            glm::vec3 lo( std::numeric_limits<float>::max() ), hi( std::numeric_limits<float>::lowest() );
            for ( const Vertex& v : b.Verts )
                lo = glm::min( lo, v.Position ), hi = glm::max( hi, v.Position );
            sm.BoundingBox.Min = lo;
            sm.BoundingBox.Max = hi;
            out.Vertices.insert( out.Vertices.end(), b.Verts.begin(), b.Verts.end() );
            out.Indices.insert( out.Indices.end(), b.Inds.begin(), b.Inds.end() );
            out.Submeshes.push_back( std::move( sm ) );
            out.SubmeshMaterialIds.push_back( material );
        }
        return out;
    }

    void Volume::ThawUnder( const std::vector<glm::ivec3>& cells )
    {
        if ( cells.empty() || m_Unit <= 0.0f )
            return;
        for ( auto it = m_Frozen.begin(); it != m_Frozen.end(); )
        {
            Layer& l     = *it;
            bool   thawn = false;
            if ( l.Unit > 0.0f && !l.Cells.empty() && l.Frame.SameAs( m_Frame ) )
            {
                const int R = static_cast<int>( std::lround( l.Unit / m_Unit ) ); // layer cell = R base cells
                const int D = static_cast<int>( std::lround( m_Unit / l.Unit ) ); // base cell = D layer cells
                if ( R >= 1 && std::abs( l.Unit - static_cast<float>( R ) * m_Unit ) <= 0.001f * m_Unit )
                {
                    const bool reached = std::ranges::any_of(
                         cells,
                         [&]( const glm::ivec3& c ) {
                             return l.Cells.contains(
                                  Pack( { FloorDiv( c.x, R ), FloorDiv( c.y, R ), FloorDiv( c.z, R ) } ) );
                         } );
                    const bool splittable = R == 1 || std::ranges::all_of( l.Cells, []( const auto& kv )
                                                                           { return kv.second.IsFlat(); } );
                    if ( reached && splittable )
                    {
                        // The active cells win where both hold one: they are the newer edit.
                        for ( const auto& [k, cell] : l.Cells )
                        {
                            if ( R == 1 )
                            {
                                m_Cells.try_emplace( k, cell );
                                continue;
                            }
                            const glm::ivec3 p = Unpack( k ) * R;
                            Cell             child;
                            std::copy( std::begin( cell.Mat ), std::end( cell.Mat ), std::begin( child.Mat ) );
                            for ( int dx = 0; dx < R; ++dx )
                                for ( int dy = 0; dy < R; ++dy )
                                    for ( int dz = 0; dz < R; ++dz )
                                        m_Cells.try_emplace( Pack( p + glm::ivec3( dx, dy, dz ) ), child );
                        }
                        thawn = true;
                    }
                }
                else if ( D >= 2 && std::abs( m_Unit - static_cast<float>( D ) * l.Unit ) <= 0.001f * l.Unit )
                {
                    for ( const glm::ivec3& c : cells )
                        for ( int dx = 0; dx < D; ++dx )
                            for ( int dy = 0; dy < D; ++dy )
                                for ( int dz = 0; dz < D; ++dz )
                                    l.Cells.erase( Pack( c * D + glm::ivec3( dx, dy, dz ) ) );
                    thawn = l.Cells.empty();
                }
            }
            it = thawn ? m_Frozen.erase( it ) : std::next( it );
        }
    }

    void Volume::CompactMaterials( const std::vector<int>& usedAscending )
    {
        // An ID no visible face uses (a hidden face's) folds to 0: every ID must stay a valid slot, and a
        // hidden face takes the op's material anyway the moment a push-in exposes it.
        std::array<uint8_t, 256> remap{};
        for ( size_t i = 0; i < usedAscending.size(); ++i )
            if ( usedAscending[i] >= 0 && usedAscending[i] < 256 )
                remap[static_cast<size_t>( usedAscending[i] )] = static_cast<uint8_t>( i );
        auto apply = [&remap]( CellMap& cells )
        {
            for ( auto& [k, cell] : cells )
                for ( uint8_t& m : cell.Mat )
                    m = remap[m];
        };
        apply( m_Cells );
        for ( Layer& l : m_Frozen )
            apply( l.Cells );
    }

    Volume Reframed( const Volume& v, const GridFrame& parent )
    {
        Volume out  = v;
        out.m_Frame = parent.Compose( v.m_Frame );
        for ( Layer& l : out.m_Frozen )
            l.Frame = parent.Compose( l.Frame );
        return out;
    }

    SavedBlockout Save( const Volume& v, uint64_t meshKey )
    {
        SavedBlockout saved;
        auto          put = [&saved]( const CellMap& cells, float unit, const GridFrame& frame )
        {
            if ( cells.empty() )
                return;
            SavedLayer l;
            l.Unit     = unit;
            l.Origin   = { frame.Origin.x, frame.Origin.y, frame.Origin.z };
            l.Rotation = { frame.Rotation.w, frame.Rotation.x, frame.Rotation.y, frame.Rotation.z };
            // Sorted, so the same volume always writes the same scene text.
            std::map<uint64_t, const Cell*> ordered;
            for ( const auto& [k, cell] : cells )
                ordered.emplace( k, &cell );
            for ( const auto& [k, cell] : ordered )
            {
                const glm::ivec3      c   = Unpack( k );
                std::vector<int32_t>& out = cell->IsFlat() ? l.Flat : l.Deformed;
                out.insert( out.end(), { c.x, c.y, c.z } );
                for ( const uint8_t m : cell->Mat )
                    out.push_back( m );
                if ( !cell->IsFlat() )
                {
                    for ( const int16_t o : cell->V )
                        out.push_back( o );
                    out.insert( out.end(), { cell->Axis, cell->Crosswise ? 1 : 0 } );
                }
            }
            saved.Layers.push_back( std::move( l ) );
        };
        for ( const Layer& l : v.m_Frozen )
            put( l.Cells, l.Unit, l.Frame );
        put( v.m_Cells, v.m_Unit, v.m_Frame );
        saved.MeshKey = std::format( "{:016x}", meshKey );
        return saved;
    }

    Common::ResultStr<Volume> Load( const SavedBlockout& saved )
    {
        Volume v;
        for ( size_t li = 0; li < saved.Layers.size(); ++li )
        {
            const SavedLayer& s = saved.Layers[li];
            if ( !( s.Unit > 0.0f ) || !std::isfinite( s.Unit ) )
                return Common::MakeFormattedError<Volume>( "CubeGrid layer {}: cell size {} is not positive", li,
                                                           s.Unit );
            if ( s.Flat.size() % kSavedFlatStride != 0 || s.Deformed.size() % kSavedDeformedStride != 0 )
                return Common::MakeFormattedError<Volume>(
                     "CubeGrid layer {}: Flat holds {} values and Deformed {}, not whole cells of {} / {}", li,
                     s.Flat.size(), s.Deformed.size(), kSavedFlatStride, kSavedDeformedStride );
            const glm::quat q( s.Rotation[0], s.Rotation[1], s.Rotation[2], s.Rotation[3] );
            if ( !std::isfinite( glm::length( q ) ) || std::abs( glm::length( q ) - 1.0f ) > 1e-3f )
                return Common::MakeFormattedError<Volume>(
                     "CubeGrid layer {}: rotation of length {} is not a rotation", li, glm::length( q ) );
            Layer l;
            l.Unit    = s.Unit;
            l.Frame   = { { s.Origin[0], s.Origin[1], s.Origin[2] }, glm::normalize( q ) };
            auto read = [&]( const std::vector<int32_t>& values, size_t stride ) -> Common::BoolResultStr
            {
                for ( size_t at = 0; at < values.size(); at += stride )
                {
                    const glm::ivec3 c( values[at], values[at + 1], values[at + 2] );
                    if ( glm::any( glm::lessThan( c, glm::ivec3( -OFF ) ) ) ||
                         glm::any( glm::greaterThanEqual( c, glm::ivec3( OFF ) ) ) )
                        return Common::MakeFormattedError<bool>(
                             "CubeGrid layer {}: cell ({}, {}, {}) is outside +-{}", li, c.x, c.y, c.z, OFF );
                    Cell cell;
                    for ( int f = 0; f < 6; ++f )
                    {
                        const int32_t m = values[at + 3 + static_cast<size_t>( f )];
                        if ( m < 0 || m > 255 )
                            return Common::MakeFormattedError<bool>(
                                 "CubeGrid layer {}: cell ({}, {}, {}) material {} is not 0..255", li, c.x, c.y,
                                 c.z, m );
                        cell.Mat[f] = static_cast<uint8_t>( m );
                    }
                    if ( stride == kSavedDeformedStride )
                        for ( int i = 0; i < 8; ++i )
                        {
                            const int32_t o = values[at + 9 + static_cast<size_t>( i )];
                            if ( o < std::numeric_limits<int16_t>::min() ||
                                 o > std::numeric_limits<int16_t>::max() )
                                return Common::MakeFormattedError<bool>(
                                     "CubeGrid layer {}: cell ({}, {}, {}) corner offset {} does not fit 16 bits",
                                     li, c.x, c.y, c.z, o );
                            cell.V[i] = static_cast<int16_t>( o );
                        }
                    if ( stride == kSavedDeformedStride )
                    {
                        const int32_t axis      = values[at + 17];
                        const int32_t crosswise = values[at + 18];
                        if ( axis < 0 || axis > 2 || crosswise < 0 || crosswise > 1 )
                            return Common::MakeFormattedError<bool>(
                                 "CubeGrid layer {}: cell ({}, {}, {}) axis {} / crosswise {} is not 0..2 / 0..1",
                                 li, c.x, c.y, c.z, axis, crosswise );
                        cell.Axis      = static_cast<uint8_t>( axis );
                        cell.Crosswise = crosswise != 0;
                    }
                    if ( !l.Cells.emplace( Pack( c ), cell ).second )
                        return Common::MakeFormattedError<bool>(
                             "CubeGrid layer {}: cell ({}, {}, {}) is stored twice", li, c.x, c.y, c.z );
                }
                return Common::MakeSuccess( true );
            };
            if ( auto r = read( s.Flat, kSavedFlatStride ); !r.IsSuccess() )
                return Common::MakeError<Volume>( r.GetError() );
            if ( auto r = read( s.Deformed, kSavedDeformedStride ); !r.IsSuccess() )
                return Common::MakeError<Volume>( r.GetError() );
            v.m_Frozen.push_back( std::move( l ) );
        }
        return Common::MakeSuccess( std::move( v ) );
    }

    std::optional<uint64_t> ParseMeshKey( const std::string& text )
    {
        if ( text.size() != 16 )
            return std::nullopt;
        uint64_t key = 0;
        for ( const char ch : text )
        {
            const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
            if ( d < 0 )
                return std::nullopt;
            key = ( key << 4 ) | static_cast<uint64_t>( d );
        }
        return key;
    }

    uint64_t MeshKey( std::vector<glm::vec3> positions, int triangleCount )
    {
        std::vector<std::array<int64_t, 3>> q;
        q.reserve( positions.size() );
        for ( const glm::vec3& p : positions )
            q.push_back(
                 { std::llround( p.x * 100.0 ), std::llround( p.y * 100.0 ), std::llround( p.z * 100.0 ) } );
        std::ranges::sort( q );
        q.erase( std::unique( q.begin(), q.end() ), q.end() );
        // FNV-1a over the triangle count and the sorted distinct positions.
        uint64_t h   = 1469598103934665603ull;
        auto     mix = [&h]( int64_t value )
        {
            for ( int b = 0; b < 8; ++b )
            {
                h ^= static_cast<uint64_t>( value >> ( 8 * b ) ) & 0xFFu;
                h *= 1099511628211ull;
            }
        };
        mix( triangleCount );
        for ( const auto& p : q )
            for ( const int64_t c : p )
                mix( c );
        return h;
    }

    namespace
    {
        // One lattice square of a face plane, as FromBoxMesh sums it.
        struct SquareCover
        {
            double Signed   = 0.0; // covered area, + where the triangles face the axis's + direction
            double BestArea = 0.0; // the largest single triangle's share of the square
            int    Material = 0;   // that triangle's material
        };
        using CoverMap = std::unordered_map<uint64_t, SquareCover>;

        // A face plane's whole-square verdict: the direction it faces (+1 / -1) and its material.
        struct PlaneFace
        {
            int Dir      = 0;
            int Material = 0;
        };
        using FaceMap = std::unordered_map<uint64_t, PlaneFace>;

        // Area of the 2D triangle `tri` (lattice units) inside the unit square whose low corner is (su, sv):
        // Sutherland-Hodgman against the square's four sides, then the shoelace.
        double AreaInSquare( const std::array<glm::dvec2, 3>& tri, int64_t su, int64_t sv )
        {
            std::vector<glm::dvec2> poly( tri.begin(), tri.end() );
            const double            low[2] = { static_cast<double>( su ), static_cast<double>( sv ) };
            for ( int side = 0; side < 4; ++side )
            {
                const int    axis   = side / 2;
                const bool   upper  = ( side % 2 ) == 1;
                const double edge   = low[axis] + ( upper ? 1.0 : 0.0 );
                auto         inside = [axis, upper, edge]( const glm::dvec2& p )
                { return upper ? p[axis] <= edge : p[axis] >= edge; };
                std::vector<glm::dvec2> clipped;
                for ( size_t i = 0; i < poly.size(); ++i )
                {
                    const glm::dvec2& a  = poly[i];
                    const glm::dvec2& b  = poly[( i + 1 ) % poly.size()];
                    const bool        ia = inside( a );
                    if ( ia )
                        clipped.push_back( a );
                    if ( ia != inside( b ) )
                        clipped.push_back( a + ( b - a ) * ( ( edge - a[axis] ) / ( b[axis] - a[axis] ) ) );
                }
                poly = std::move( clipped );
                if ( poly.size() < 3 )
                    return 0.0;
            }
            double twice = 0.0;
            for ( size_t i = 0; i < poly.size(); ++i )
            {
                const glm::dvec2& a = poly[i];
                const glm::dvec2& b = poly[( i + 1 ) % poly.size()];
                twice += a.x * b.y - b.x * a.y;
            }
            return std::abs( twice ) * 0.5;
        }

        const char* AxisName( int a )
        {
            return a == 0 ? "X" : a == 1 ? "Y" : "Z";
        }
    } // namespace

    Common::ResultStr<Volume> FromBoxMesh( const std::vector<glm::dvec3>&         positions,
                                           const std::vector<std::array<int, 3>>& triangles,
                                           const std::vector<int>& materials, float minUnit )
    {
        if ( triangles.empty() )
            return Common::MakeError<Volume>( "it has no triangles" );
        if ( materials.size() != triangles.size() )
            return Common::MakeError<Volume>( std::format( "{} materials for {} triangles: one per triangle",
                                                           materials.size(), triangles.size() ) );
        for ( size_t t = 0; t < triangles.size(); ++t )
            for ( const int v : triangles[t] )
                if ( v < 0 || static_cast<size_t>( v ) >= positions.size() )
                    return Common::MakeError<Volume>(
                         std::format( "triangle {} names vertex {} of {}", t, v, positions.size() ) );

        // The lattice: origin at the lowest corner, step = the gcd of every lattice point's offset from it.
        glm::dvec3 low( std::numeric_limits<double>::max() );
        for ( const glm::dvec3& p : positions )
            low = glm::min( low, p );
        std::vector<std::array<int64_t, 3>> q( positions.size() );
        for ( size_t i = 0; i < positions.size(); ++i )
            for ( int a = 0; a < 3; ++a )
                q[i][a] = std::llround( ( positions[i][a] - low[a] ) * 100.0 );

        // A point inside one flat face of one material is not a lattice point: the bake fans a quad whose edge
        // a neighbour's corner splits from the quad's CENTRE (Volume::Bake, T-junctions), and the centre of an
        // odd run of blocks sits half a block off the lattice. Such a point is where every triangle touching it
        // lies in one axis plane, faces one way, has one material, and closes a full fan around it (each edge
        // from it is walked once out and once back). The faces' outlines - where the surface turns, or the
        // material changes - are what the blocks are made of, so the step is taken from those points only.
        std::map<std::array<int64_t, 3>, int> weld;
        std::vector<int>                      welded( positions.size() );
        for ( size_t i = 0; i < positions.size(); ++i )
            welded[i] = weld.try_emplace( q[i], static_cast<int>( weld.size() ) ).first->second;
        const auto normalOf = [&]( const std::array<int, 3>& tri )
        {
            std::array<glm::dvec3, 3> c;
            for ( int k = 0; k < 3; ++k )
            {
                const auto& v = q[static_cast<size_t>( tri[k] )];
                c[k]          = glm::dvec3( static_cast<double>( v[0] ), static_cast<double>( v[1] ),
                                            static_cast<double>( v[2] ) );
            }
            return glm::cross( c[1] - c[0], c[2] - c[0] ); // integer coordinates: exact
        };
        struct Star
        {
            glm::dvec3         Normal{ 0.0 };
            int                Material = -1;
            bool               Flat     = true;
            std::map<int, int> Turns; // neighbour -> (edges out) - (edges back)
        };
        std::vector<Star> stars( weld.size() );
        for ( size_t t = 0; t < triangles.size(); ++t )
        {
            const glm::dvec3 n = normalOf( triangles[t] );
            if ( n == glm::dvec3( 0.0 ) )
                continue; // a zero-area sliver bounds nothing
            const int        nonZero = ( n.x != 0.0 ) + ( n.y != 0.0 ) + ( n.z != 0.0 );
            const glm::dvec3 dir     = glm::sign( n );
            for ( int k = 0; k < 3; ++k )
            {
                Star& s = stars[static_cast<size_t>( welded[static_cast<size_t>( triangles[t][k] )] )];
                if ( s.Material < 0 )
                {
                    s.Normal   = dir;
                    s.Material = materials[t];
                }
                s.Flat = s.Flat && nonZero == 1 && s.Normal == dir && s.Material == materials[t];
                ++s.Turns[welded[static_cast<size_t>( triangles[t][( k + 1 ) % 3] )]];
                --s.Turns[welded[static_cast<size_t>( triangles[t][( k + 2 ) % 3] )]];
            }
        }
        std::vector<bool> latticePoint( weld.size(), true );
        for ( size_t w = 0; w < stars.size(); ++w )
        {
            const Star& s   = stars[w];
            latticePoint[w] = s.Material < 0 || !s.Flat ||
                              !std::ranges::all_of( s.Turns, []( const auto& turn ) { return turn.second == 0; } );
        }
        int64_t step = 0;
        for ( const auto& [at, w] : weld )
            if ( latticePoint[static_cast<size_t>( w )] )
                for ( int a = 0; a < 3; ++a )
                    step = std::gcd( step, at[a] );
        if ( step == 0 )
            return Common::MakeError<Volume>( "every vertex sits at one point: it encloses no block" );
        const double unit = static_cast<double>( step ) / 100.0;
        if ( unit < static_cast<double>( minUnit ) )
            return Common::MakeError<Volume>(
                 std::format( "its vertices lie on a {:.2f} cm lattice, finer than the smallest Block Size "
                              "({:.2f} cm): it is not built of whole blocks",
                              unit, minUnit ) );
        for ( const auto& c : q )
            for ( int a = 0; a < 3; ++a )
                if ( c[a] / step > OFF - 2 )
                    return Common::MakeError<Volume>(
                         std::format( "it spans {} blocks of {:.2f} cm along {}, more than a grid holds ({})",
                                      c[a] / step, unit, AxisName( a ), OFF - 2 ) );

        // Every triangle's signed area onto the lattice squares of its face plane.
        std::array<CoverMap, 3> cover;
        for ( size_t t = 0; t < triangles.size(); ++t )
        {
            if ( materials[t] < 0 || materials[t] > std::numeric_limits<uint8_t>::max() )
                return Common::MakeError<Volume>(
                     std::format( "triangle {} has material {}, outside a cell face's 0..{}", t, materials[t],
                                  static_cast<int>( std::numeric_limits<uint8_t>::max() ) ) );
            std::array<glm::dvec3, 3> p;
            for ( int k = 0; k < 3; ++k )
            {
                const auto& c = q[static_cast<size_t>( triangles[t][k] )];
                // In blocks; a point inside a flat face (a fan centre) may sit between lattice planes.
                p[k] = glm::dvec3( static_cast<double>( c[0] ), static_cast<double>( c[1] ),
                                   static_cast<double>( c[2] ) ) /
                       static_cast<double>( step );
            }
            // From the integer coordinates: the cross product is exact, so "axis-aligned" is an exact test.
            const glm::dvec3 n = normalOf( triangles[t] );
            int              a = 0;
            for ( int k = 1; k < 3; ++k )
                if ( std::abs( n[k] ) > std::abs( n[a] ) )
                    a = k;
            if ( n[a] == 0.0 )
                continue; // a zero-area sliver covers no square
            const int u = ( a + 1 ) % 3;
            const int v = ( a + 2 ) % 3;
            if ( n[u] != 0.0 || n[v] != 0.0 )
            {
                const glm::dvec3 w = glm::normalize( n );
                return Common::MakeError<Volume>(
                     std::format( "triangle {} is not axis-aligned (normal {:.3f}, {:.3f}, {:.3f}): a Corner "
                                  "Mode slope or a mesh not built of blocks has no voxels to recover",
                                  t, w.x, w.y, w.z ) );
            }
            const std::array<glm::dvec2, 3> flat = {
                 glm::dvec2( p[0][u], p[0][v] ), glm::dvec2( p[1][u], p[1][v] ), glm::dvec2( p[2][u], p[2][v] ) };
            const glm::dvec2 lo2   = glm::min( glm::min( flat[0], flat[1] ), flat[2] );
            const glm::dvec2 hi2   = glm::max( glm::max( flat[0], flat[1] ), flat[2] );
            const double     sign  = n[a] > 0.0 ? 1.0 : -1.0;
            const int64_t    plane = q[static_cast<size_t>( triangles[t][0] )][a];
            if ( plane % step != 0 )
                return Common::MakeError<Volume>(
                     std::format( "triangle {} lies between the {:.2f} cm block planes along {}: its faces are "
                                  "not whole blocks",
                                  t, static_cast<double>( step ) / 100.0, AxisName( a ) ) );
            for ( auto su = static_cast<int64_t>( std::floor( lo2.x ) );
                  su < static_cast<int64_t>( std::ceil( hi2.x ) ); ++su )
                for ( auto sv = static_cast<int64_t>( std::floor( lo2.y ) );
                      sv < static_cast<int64_t>( std::ceil( hi2.y ) ); ++sv )
                {
                    const double area = AreaInSquare( flat, su, sv );
                    if ( area <= 0.0 )
                        continue;
                    glm::ivec3 key;
                    key[a]         = static_cast<int>( plane / step );
                    key[u]         = static_cast<int>( su );
                    key[v]         = static_cast<int>( sv );
                    SquareCover& c = cover[a][Pack( key )];
                    c.Signed += sign * area;
                    if ( area > c.BestArea )
                    {
                        c.BestArea = area;
                        c.Material = materials[t];
                    }
                }
        }

        // Each square is a whole face or none: the triangles over it must sum to -1, 0 or +1.
        std::array<FaceMap, 3> faces;
        for ( int a = 0; a < 3; ++a )
            for ( const auto& [key, c] : cover[a] )
            {
                const double     whole = std::round( c.Signed );
                const glm::ivec3 at    = Unpack( key );
                if ( std::abs( c.Signed - whole ) > 1e-6 )
                    return Common::MakeError<Volume>(
                         std::format( "its {} faces cover {:.3f} of the block square at ({}, {}, {}): its faces "
                                      "are not whole blocks",
                                      AxisName( a ), std::abs( c.Signed ), at.x, at.y, at.z ) );
                if ( std::abs( whole ) > 1.0 )
                    return Common::MakeError<Volume>(
                         std::format( "{} of its {} faces overlap on the block square at ({}, {}, {})",
                                      static_cast<int>( std::abs( whole ) ), AxisName( a ), at.x, at.y, at.z ) );
                if ( whole != 0.0 )
                    faces[a][key] = PlaneFace{ whole > 0.0 ? 1 : -1, c.Material };
            }

        // Sweep the X faces column by column: a face looking -X opens a run of solid cells, one looking +X
        // closes it.
        std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> columns;
        for ( const auto& [key, f] : faces[0] )
        {
            const glm::ivec3 at = Unpack( key );
            columns[{ at.y, at.z }].emplace_back( at.x, f.Dir );
        }
        CellMap cells;
        for ( auto& [yz, events] : columns )
        {
            std::ranges::sort( events );
            int occupied = 0;
            int from     = 0;
            for ( const auto& [x, dir] : events )
            {
                if ( occupied == 1 )
                    for ( int cx = from; cx < x; ++cx )
                        cells[Pack( { cx, yz.first, yz.second } )] = Cell{};
                occupied -= dir;
                if ( occupied < 0 || occupied > 1 )
                    return Common::MakeError<Volume>(
                         std::format( "it is not a closed volume: the column at block (y {}, z {}) {} twice at "
                                      "x {}",
                                      yz.first, yz.second, dir < 0 ? "enters" : "leaves", x ) );
                from = x;
            }
            if ( occupied != 0 )
                return Common::MakeError<Volume>(
                     std::format( "it is not a closed volume: the column at block (y {}, z {}) never leaves",
                                  yz.first, yz.second ) );
        }
        if ( cells.empty() )
            return Common::MakeError<Volume>( "it encloses no block" );

        // Every exposed face of the swept cells must be a face of the mesh, facing out, and every face of the
        // mesh one of those: then the cells ARE the mesh's volume. The face's material goes onto the cell.
        std::array<size_t, 3> matched{};
        for ( auto& [key, cell] : cells )
        {
            const glm::ivec3 c = Unpack( key );
            for ( int f = 0; f < 6; ++f )
            {
                if ( cells.contains( Pack( c + kNeighbor[f] ) ) )
                    continue;
                const int  a     = kNeighbor[f].x != 0 ? 0 : kNeighbor[f].y != 0 ? 1 : 2;
                const int  dir   = kNeighbor[f][a];
                glm::ivec3 plane = c;
                plane[a] += dir > 0 ? 1 : 0;
                const auto it = faces[a].find( Pack( plane ) );
                if ( it == faces[a].end() || it->second.Dir != dir )
                    return Common::MakeError<Volume>(
                         std::format( "it is not a closed volume: block ({}, {}, {}) has no {}{} face", c.x, c.y,
                                      c.z, dir > 0 ? "+" : "-", AxisName( a ) ) );
                cell.Mat[f] = static_cast<uint8_t>( it->second.Material );
                ++matched[a];
            }
        }
        for ( int a = 0; a < 3; ++a )
            if ( matched[a] != faces[a].size() )
                return Common::MakeError<Volume>(
                     std::format( "{} of its {} faces lie inside or outside the volume its X faces close",
                                  faces[a].size() - matched[a], AxisName( a ) ) );

        Volume v;
        Layer  layer;
        layer.Cells        = std::move( cells );
        layer.Unit         = static_cast<float>( unit );
        layer.Frame.Origin = glm::vec3( low );
        v.m_Frozen.push_back( std::move( layer ) );
        return Common::MakeSuccess( std::move( v ) );
    }

    void SlideSelection( WorkPlane& plane, int baseCells )
    {
        plane.Cell += plane.Sign * baseCells;
    }

    float LineParameterClosestToRay( const glm::vec3& lineOrigin, const glm::vec3& lineDir,
                                     const glm::vec3& rayOrigin, const glm::vec3& rayDir )
    {
        const glm::vec3 rd   = glm::normalize( rayDir );
        const glm::vec3 diff = lineOrigin - rayOrigin;
        const float     a01  = -glm::dot( lineDir, rd );
        const float     b0   = glm::dot( diff, lineDir );
        const float     det  = std::abs( 1.0f - a01 * a01 );
        if ( det >= 1e-6f )
        {
            const float b1 = -glm::dot( diff, rd );
            if ( a01 * b0 - b1 >= 0.0f ) // the closest ray point is interior: both parameters are free
                return ( a01 * b1 - b0 ) / det;
        }
        // Parallel, or the closest ray point is its origin: project the origin onto the line.
        return -b0;
    }

    int DragExtrudeBlocks( float paramDelta, float blockSize, int blocksPerStep )
    {
        const int   bps  = std::max( 1, blocksPerStep );
        const float step = blockSize * static_cast<float>( bps );
        if ( step <= 0.0f )
            return 0;
        return static_cast<int>( std::lround( paramDelta / step ) ) * bps;
    }
} // namespace Desert::Geometry::VoxelBlockout
