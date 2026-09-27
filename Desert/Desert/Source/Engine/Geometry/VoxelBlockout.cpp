#include <Engine/Geometry/VoxelBlockout.hpp>

#include <Engine/Geometry/GreedyMesher.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <map>
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

        // Face f of `a` and the opposite face of its same-layer neighbour `b` lie in one plane, and when the
        // offset axis runs IN that plane each is a column between two straight lines along it: a ramp's side
        // next to a flat block, the wall of a sloped wall's neighbour. Those two faces overlap only partly, so
        // neither is hidden nor shown whole - the bake shows each one's part the other does not cover
        // (ColumnPieces). Returns that axis, or -1 when the all-or-nothing rule of FaceHidden applies.
        int ColumnAxis( const Cell& a, const Cell& b, int f )
        {
            const bool da = !a.IsFlat();
            const bool db = !b.IsFlat();
            if ( ( !da && !db ) || ( da && db && a.Axis != b.Axis ) )
                return -1;
            const int axis = da ? a.Axis : b.Axis;
            return axis == FaceNormalAxis( f ) ? -1 : axis;
        }

        // The part of face f of `a` (at cell `c`, edge `unit`, FRAME space) that `b`'s opposite face does not
        // cover, as quads in f's winding; `axis` is ColumnAxis. Along the plane's other axis s both faces'
        // bottoms and tops are straight lines, so the uncovered part is at most one piece below `b` and one
        // above it per stretch of s between the points where two of those four lines cross.
        std::vector<std::array<glm::vec3, 4>> ColumnPieces( const glm::ivec3& c, const Cell& a, const Cell& b,
                                                            int f, int axis, float unit )
        {
            const int n    = FaceNormalAxis( f );
            const int sAx  = 3 - n - axis;
            const int nBit = 1 << n;
            const int side = ( kFaceCorner[f][0] & nBit ) != 0 ? nBit : 0;
            auto      off  = []( const Cell& cell, int i )
            { return static_cast<float>( cell.V[i] ) / static_cast<float>( CornerDen ); };
            auto corner = [&]( int s, int t )
            { return side | ( s != 0 ? 1 << sAx : 0 ) | ( t != 0 ? 1 << axis : 0 ); };
            // Lines over s in [0, 1] as (value at 0, value at 1): bottom and top of a, then of b.
            std::array<glm::vec2, 4> line;
            for ( int t = 0; t < 2; ++t )
            {
                line[t]     = { static_cast<float>( t ) + off( a, corner( 0, t ) ),
                                static_cast<float>( t ) + off( a, corner( 1, t ) ) };
                line[2 + t] = { static_cast<float>( t ) + off( b, corner( 0, t ) ^ nBit ),
                                static_cast<float>( t ) + off( b, corner( 1, t ) ^ nBit ) };
            }
            auto at = [&]( int l, float s ) { return glm::mix( line[l].x, line[l].y, s ); };

            std::vector<float> cuts{ 0.0f, 1.0f };
            for ( int i = 0; i < 4; ++i )
                for ( int j = i + 1; j < 4; ++j )
                {
                    const float d0 = line[i].x - line[j].x;
                    const float d1 = line[i].y - line[j].y;
                    if ( ( d0 < 0.0f ) != ( d1 < 0.0f ) && d0 != d1 )
                        if ( const float s = d0 / ( d0 - d1 ); s > 1e-5f && s < 1.0f - 1e-5f )
                            cuts.push_back( s );
                }
            std::ranges::sort( cuts );

            auto point = [&]( float s, float t )
            {
                glm::vec3 p( c );
                p[n] += side != 0 ? 1.0f : 0.0f;
                p[sAx] += s;
                p[axis] += t;
                return p * unit;
            };
            const glm::vec3                       outward = kFace[f][0].N;
            std::vector<std::array<glm::vec3, 4>> pieces;
            for ( size_t k = 0; k + 1 < cuts.size(); ++k )
            {
                const float s0 = cuts[k];
                const float s1 = cuts[k + 1];
                if ( s1 - s0 < 1e-5f )
                    continue;
                const float sm = 0.5f * ( s0 + s1 );
                // Below b: a's bottom up to the lower of a's top and b's bottom. Above b: the higher of a's bottom
                // and b's top up to a's top. No two lines cross inside the stretch, so the line picked at its
                // middle is the bound along all of it.
                const int belowTop = at( 1, sm ) < at( 2, sm ) ? 1 : 2;
                const int aboveBot = at( 0, sm ) > at( 3, sm ) ? 0 : 3;
                for ( const auto& [lo, hi] : { std::pair{ 0, belowTop }, std::pair{ aboveBot, 1 } } )
                {
                    if ( at( hi, sm ) - at( lo, sm ) <= 1e-5f )
                        continue;
                    std::array<glm::vec3, 4> q{ point( s0, at( lo, s0 ) ), point( s1, at( lo, s1 ) ),
                                                point( s1, std::max( at( hi, s1 ), at( lo, s1 ) ) ),
                                                point( s0, std::max( at( hi, s0 ), at( lo, s0 ) ) ) };
                    const glm::vec3          nrm =
                         glm::cross( q[1] - q[0], q[3] - q[0] ) + glm::cross( q[3] - q[2], q[1] - q[2] );
                    if ( glm::dot( nrm, outward ) < 0.0f )
                        std::swap( q[1], q[3] );
                    pieces.push_back( q );
                }
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
            const float eps = 1e-3f * std::min( lu, m_Unit );
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

        auto columnAxis = []( const CellMap& cells, const glm::ivec3& c, const Cell& cell, int f )
        {
            const auto it = cells.find( Pack( c + kNeighbor[f] ) );
            return it == cells.end() ? -1 : ColumnAxis( cell, it->second, f );
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
            out.State = count == 0 ? 0 : ( count == R * R ? 2 : 1 );
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
                            columnAxis( cells, c, it->second, f ) < 0 && coverage( c, f, lu, lf ).State == 0;
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
                    if ( const int axis = columnAxis( cells, c, cell, f ); axis >= 0 )
                    {
                        const Cell& other = cells.at( Pack( c + kNeighbor[f] ) );
                        for ( const auto& piece : ColumnPieces( c, cell, other, f, axis, lu ) )
                            emitQuad( cell.Mat[f], piece.data(), kFace[f][0].N, UvFrame( f ), lf, false );
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
