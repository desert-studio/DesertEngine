#include <Engine/Geometry/VoxelBlockout.hpp>

#include <Engine/Geometry/GreedyMesher.hpp>

#include <algorithm>
#include <cmath>

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

    glm::vec3 CornerPos( const glm::ivec3& c, const Cell& cell, int i, float unit, const glm::vec3& origin )
    {
        glm::vec3 p( c.x + ( i & 1 ? 1 : 0 ), c.y + ( i & 2 ? 1 : 0 ), c.z + ( i & 4 ? 1 : 0 ) );
        p *= unit;
        p.y += static_cast<float>( cell.V[i] ) / static_cast<float>( CornerDen ) * unit;
        return p + origin;
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

    bool Volume::SolidAt( const glm::ivec3& c, float unit, const glm::vec3& origin ) const
    {
        // Layer units are always commensurate in practice (the base only ever halves), so a coarser layer maps
        // with one FloorDiv. A layer FINER than `unit`, or one built in a DIFFERENT grid frame (its lattice
        // doesn't line up at all), is skipped - conservative: it can only ever leave a hidden interior face.
        auto inLayer = [&]( const CellMap& cells, float lu, const glm::vec3& lo )
        {
            if ( cells.empty() || lu <= 0.0f ||
                 glm::any( glm::greaterThan( glm::abs( lo - origin ), glm::vec3( 1e-3f ) ) ) )
                return false;
            const int R = static_cast<int>( std::lround( lu / unit ) );
            if ( R < 1 || std::abs( lu - static_cast<float>( R ) * unit ) > 0.001f * unit )
                return false;
            return cells.count( Pack( { FloorDiv( c.x, R ), FloorDiv( c.y, R ), FloorDiv( c.z, R ) } ) ) > 0;
        };
        if ( inLayer( Cells, Unit, Origin ) )
            return true;
        for ( const Layer& l : Frozen )
            if ( inLayer( l.Cells, l.Unit, l.Origin ) )
                return true;
        return false;
    }

    bool Volume::FaceHidden( const CellMap& cells, const glm::ivec3& c, const Cell& data, int f, float unit,
                             const glm::vec3& origin ) const
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
        return data.IsFlat() && SolidAt( n, unit, origin );
    }

    void Volume::Refine( int F )
    {
        CellMap out;
        out.reserve( Cells.size() * static_cast<size_t>( F * F * F ) );
        for ( const auto& [k, cell] : Cells )
        {
            const glm::ivec3 c = Unpack( k );
            for ( int dx = 0; dx < F; ++dx )
                for ( int dy = 0; dy < F; ++dy )
                    for ( int dz = 0; dz < F; ++dz )
                        out[Pack( { c.x * F + dx, c.y * F + dy, c.z * F + dz } )] = Cell{};
        }
        Cells = std::move( out );
        Unit /= static_cast<float>( F );
    }

    bool Volume::Freeze()
    {
        if ( Cells.empty() )
            return false;
        Layer l;
        l.Cells  = std::move( Cells );
        l.Unit   = Unit;
        l.Origin = Origin;
        Frozen.push_back( std::move( l ) );
        Cells.clear();
        Unit = -1.0f; // the next Block Size becomes the base of a brand-new volume
        return true;
    }

    void Volume::PushPull( WorkPlane& plane, const Rect& sel, int dir, int height )
    {
        auto      occ  = [&]( const glm::ivec3& c ) { return SolidAt( c, Unit, Origin ); };
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
                        Cells[Pack( c )] = Cell{};
                }
            plane.Cell += sign * height;
        }
        else // Push in: remove `height` base cells just inside the plane, per column
        {
            for ( int u = sel.UMin; u <= sel.UMax; ++u )
                for ( int v = sel.VMin; v <= sel.VMax; ++v )
                {
                    glm::ivec3 c{ 0 };
                    c[na] = plane.Cell - sign;
                    c[ua] = u;
                    c[va] = v;
                    for ( int s = 0; s < height; ++s, c[na] -= sign )
                        Cells.erase( Pack( c ) );
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

        const float spanU = static_cast<float>( sel.UMax + 1 - sel.UMin );
        const float spanV = static_cast<float>( sel.VMax + 1 - sel.VMin );
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
                auto it     = Cells.find( Pack( c ) );
                if ( it == Cells.end() )
                    continue; // nothing pushed out under this column yet
                for ( int i = 0; i < 8; ++i )
                {
                    if ( !( i & 2 ) ) // bottom corners stay on the lattice so the cell below still meets it
                        continue;
                    it->second.V[i] = heightAt( uu + ( ( i & 4 ) ? 1 : 0 ), vv + ( ( i & 1 ) ? 1 : 0 ) );
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
            if ( auto it = Cells.find( Pack( c ) ); it != Cells.end() )
                h[k] = it->second.V[kPosts[k].Corner];
        }
        return h;
    }

    RenderMeshData Volume::Bake() const
    {
        RenderMeshData       out;
        std::vector<Vertex>& verts = out.Vertices;
        std::vector<Index>&  inds  = out.Indices;

        // One quad -> 4 vertices + 2 triangles, with world-aligned UVs and a tangent frame that matches them
        // (so a normal map on a merged quad lines up with the texture it is paired with).
        auto emitQuad = [&]( const glm::vec3 p[4], const glm::vec3& nrm, const FaceUvFrame& uv )
        {
            const uint32_t base = static_cast<uint32_t>( verts.size() );
            for ( int k = 0; k < 4; ++k )
            {
                Vertex v;
                v.Position  = p[k];
                v.Normal    = nrm;
                v.Tangent   = uv.T;
                v.Bitangent = uv.B;
                v.TexCoord  = { glm::dot( p[k], uv.T ) * kUvPerUnit, glm::dot( p[k], uv.B ) * kUvPerUnit };
                verts.push_back( v );
            }
            inds.push_back( { base + 0, base + 1, base + 2 } );
            inds.push_back( { base + 2, base + 3, base + 0 } );
        };

        auto emitLayer = [&]( const CellMap& cells, float lu, const glm::vec3& lo )
        {
            // FLAT cells go through greedy meshing: a 20x8 blockout wall becomes ONE quad instead of 160.
            // Corner-deformed cells keep their own per-face quads - their corners are not coplanar with a
            // neighbour's, so merging them would flatten the ramp.
            std::vector<glm::ivec3> flat;
            flat.reserve( cells.size() );
            for ( const auto& [key, cell] : cells )
                if ( cell.IsFlat() )
                    flat.push_back( Unpack( key ) );

            const auto merged =
                 GreedyMeshFaces( flat,
                                  [&]( const glm::ivec3& c, int f )
                                  {
                                      const auto it = cells.find( Pack( c ) );
                                      return it != cells.end() && !FaceHidden( cells, c, it->second, f, lu, lo );
                                  } );

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
                    p[k] = g * lu + lo;
                }
                emitQuad( p, kFace[q.Face][0].N, uv );
            }

            for ( const auto& [key, cell] : cells )
            {
                if ( cell.IsFlat() )
                    continue; // already meshed above

                const glm::ivec3 c = Unpack( key );
                for ( int f = 0; f < 6; ++f )
                {
                    if ( FaceHidden( cells, c, cell, f, lu, lo ) ) // only Solid/Empty borders
                        continue;
                    // Corner Mode can slant a quad, so the normal comes from the actual corners.
                    glm::vec3 p[4];
                    for ( int k = 0; k < 4; ++k )
                        p[k] = CornerPos( c, cell, kFaceCorner[f][k], lu, lo );
                    glm::vec3 nrm =
                         glm::cross( p[1] - p[0], p[3] - p[0] ) + glm::cross( p[3] - p[2], p[1] - p[2] );
                    nrm = glm::dot( nrm, nrm ) > 1e-12f ? glm::normalize( nrm ) : kFace[f][0].N;

                    emitQuad( p, nrm, UvFrame( f ) );
                }
            }
        };
        emitLayer( Cells, Unit, Origin );
        for ( const Layer& l : Frozen )
            emitLayer( l.Cells, l.Unit, l.Origin );
        return out;
    }
} // namespace Desert::Geometry::VoxelBlockout
