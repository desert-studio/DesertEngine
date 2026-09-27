#include <Engine/Geometry/VoxelBlockout.hpp>

#include <Engine/Geometry/GreedyMesher.hpp>

#include <algorithm>
#include <cmath>
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

    const RectPost kPosts[4] = {
         { false, false, 2 }, { true, false, 2 | 4 }, { false, true, 2 | 1 }, { true, true, 2 | 1 | 4 } };

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
        p.y += static_cast<float>( cell.V[i] ) / static_cast<float>( CornerDen ) * unit;
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
                if ( data.V[i] != it->second.V[i ^ bit] )
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
            std::vector<glm::ivec3> removed;
            for ( int u = sel.UMin; u <= sel.UMax; ++u )
                for ( int v = sel.VMin; v <= sel.VMax; ++v )
                {
                    glm::ivec3 c{ 0 };
                    c[na] = plane.Cell - sign;
                    c[ua] = u;
                    c[va] = v;
                    for ( int s = 0; s < height; ++s, c[na] -= sign )
                        if ( m_Cells.erase( Pack( c ) ) != 0 )
                            removed.push_back( c );
                }
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

    bool Volume::ApplyCornerHeights( const WorkPlane& plane, const Rect& sel, const CornerHeights& heights )
    {
        if ( plane.Na != 1 || plane.Sign <= 0 )
            return false;
        const int ua      = ( plane.Na + 1 ) % 3;
        const int va      = ( plane.Na + 2 ) % 3;
        const int topCell = plane.Cell - 1; // the cell whose top face is the work-plane

        const auto spanU = static_cast<float>( sel.UMax + 1 - sel.UMin );
        const auto spanV = static_cast<float>( sel.VMax + 1 - sel.VMin );
        if ( spanU <= 0.0f || spanV <= 0.0f )
            return false;

        auto heightAt = [&]( int lu, int lv )
        {
            const float fu = ( static_cast<float>( lu - sel.UMin ) ) / spanU;
            const float fv = ( static_cast<float>( lv - sel.VMin ) ) / spanV;
            const float a  = glm::mix( static_cast<float>( heights[0] ), static_cast<float>( heights[1] ), fu );
            const float b  = glm::mix( static_cast<float>( heights[2] ), static_cast<float>( heights[3] ), fu );
            return static_cast<int16_t>( std::lround( glm::mix( a, b, fv ) ) );
        };

        for ( int uu = sel.UMin; uu <= sel.UMax; ++uu )
            for ( int vv = sel.VMin; vv <= sel.VMax; ++vv )
            {
                glm::ivec3 c{ 0 };
                c[plane.Na] = topCell;
                c[ua]       = uu;
                c[va]       = vv;
                auto it     = m_Cells.find( Pack( c ) );
                if ( it == m_Cells.end() )
                    continue; // nothing pushed out under this column yet
                for ( int i = 0; i < 8; ++i )
                {
                    if ( ( i & 2 ) == 0 ) // bottom corners stay on the lattice so the cell below still meets it
                        continue;
                    it->second.V[i] =
                         heightAt( uu + ( ( ( i & 4 ) != 0 ) ? 1 : 0 ), vv + ( ( ( i & 1 ) != 0 ) ? 1 : 0 ) );
                }
            }
        return true;
    }

    CornerHeights Volume::ReadCornerHeights( const WorkPlane& plane, const Rect& sel ) const
    {
        CornerHeights h{};
        if ( plane.Na != 1 || plane.Sign <= 0 )
            return h;
        const int ua = ( plane.Na + 1 ) % 3;
        const int va = ( plane.Na + 2 ) % 3;
        for ( int k = 0; k < 4; ++k )
        {
            glm::ivec3 c{ 0 };
            c[plane.Na] = plane.Cell - 1;
            c[ua]       = kPosts[k].AtUMax ? sel.UMax : sel.UMin;
            c[va]       = kPosts[k].AtVMax ? sel.VMax : sel.VMin;
            if ( auto it = m_Cells.find( Pack( c ) ); it != m_Cells.end() )
                h[k] = it->second.V[kPosts[k].Corner];
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
            glm::vec3                N;
            FaceUvFrame              Uv;
            GridFrame                Frame;
        };
        std::vector<PendingQuad> quads;
        float                    minUnit = std::numeric_limits<float>::max();
        auto emitQuad = [&]( int material, const glm::vec3 p[4], const glm::vec3& nrm, const FaceUvFrame& uv,
                             const GridFrame& frame )
        { quads.push_back( { material, { p[0], p[1], p[2], p[3] }, nrm, uv, frame } ); };

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
                     return it != cells.end() && !FaceHidden( cells, c, it->second, f, lu, lf );
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
                emitQuad( cells.at( Pack( q.Cell ) ).Mat[q.Face], p, kFace[q.Face][0].N, uv, lf );
            }

            for ( const auto& [key, cell] : cells )
            {
                if ( cell.IsFlat() )
                    continue; // already meshed above

                const glm::ivec3 c = Unpack( key );
                for ( int f = 0; f < 6; ++f )
                {
                    if ( FaceHidden( cells, c, cell, f, lu, lf ) ) // only Solid/Empty borders
                        continue;
                    // Corner Mode can slant a quad, so the normal comes from the actual corners.
                    glm::vec3 p[4];
                    for ( int k = 0; k < 4; ++k )
                        p[k] = CornerPos( c, cell, kFaceCorner[f][k], lu );
                    glm::vec3 nrm =
                         glm::cross( p[1] - p[0], p[3] - p[0] ) + glm::cross( p[3] - p[2], p[1] - p[2] );
                    nrm = glm::dot( nrm, nrm ) > 1e-12f ? glm::normalize( nrm ) : kFace[f][0].N;

                    emitQuad( cell.Mat[f], p, nrm, UvFrame( f ), lf );
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
            for ( int k = 0; k < 4; ++k )
            {
                const glm::vec3& la = q.P[k];
                const glm::vec3& lb = q.P[( k + 1 ) % 4];
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
            if ( ring.size() == 4 )
            {
                for ( const glm::vec3& p : ring )
                    vertex( p );
                b.Inds.push_back( { base + 0, base + 1, base + 2 } );
                b.Inds.push_back( { base + 2, base + 3, base + 0 } );
                continue;
            }
            // A split quad: a fan from its centre keeps every outline segment an edge of its own triangle, in
            // the quad's winding.
            vertex( ( q.P[0] + q.P[1] + q.P[2] + q.P[3] ) * 0.25f );
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
