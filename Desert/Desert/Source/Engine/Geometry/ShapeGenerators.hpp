#pragma once

#include <Engine/Geometry/MeshTypes.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Desert::Geometry
{
    // Parametric shape generators for the Modeling mode's Create palette (UE's Box / Sphere / Cylinder /
    // Cone / Stairs). Pure CPU: vertices + triangles in, nothing GPU, nothing ECS — so they are unit
    // testable and reusable by anything that needs a primitive (thumbnails, colliders, tests).
    // Today the only caller is the MeshLOD suite: the first Create tool never compiled and was deleted,
    // and the palette is being rebuilt on EditMesh.
    //
    // Conventions, shared by every generator here:
    //   * world units are CENTIMETRES (see docs/UNITS.md) and sizes are FULL extents, not radii-from-centre;
    //   * the shape is centred on X/Z and sits with its base at Y = 0, the way you place a prop on a floor;
    //   * every quad owns its vertices (hard normals — these are blockout shapes, not organic surfaces),
    //     except the sphere, which is smooth-shaded because a faceted ball is never what anyone wants;
    //   * UVs are world-aligned at one unit per metre, matching the CubeGrid bake, so a shape dropped next
    //     to a blockout shows the same texel density.

    struct ShapeMesh
    {
        std::vector<Vertex> Vertices;
        std::vector<Index>  Indices;

        [[nodiscard]] Common::Math::AABB Bounds() const
        {
            Common::Math::AABB box;
            if ( Vertices.empty() )
                return box;
            box.Min = glm::vec3( 1.0e30f );
            box.Max = glm::vec3( -1.0e30f );
            for ( const auto& v : Vertices )
            {
                box.Min = glm::min( box.Min, v.Position );
                box.Max = glm::max( box.Max, v.Position );
            }
            return box;
        }
    };

    namespace Detail
    {
        inline constexpr float kUvPerUnit = 1.0f / 100.0f; // one UV unit per metre, like the CubeGrid bake

        // Smallest extent any shape is allowed to have, in world units (= 1 mm). A zero or negative size
        // is a slider being dragged to its end, not an intent to make an infinitely thin object: clamping
        // to something VISIBLE keeps the mesh well-formed (a 0.001 cm face has a numerically degenerate
        // area) and the shape recoverable by dragging the slider back.
        inline constexpr float kMinExtent = 0.1f;

        // Appends one flat quad (p0..p3 wound CCW as seen from outside). UVs are the corners projected on
        // the quad's own tangent frame, so a 4 m wall tiles four times instead of stretching once.
        inline void AddQuad( ShapeMesh& m, const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                             const glm::vec3& p3 )
        {
            glm::vec3   n   = glm::cross( p1 - p0, p3 - p0 );
            const float len = glm::length( n );
            if ( len < 1e-9f )
                return; // degenerate (a zero-size step / cap) — skip instead of emitting NaNs
            n /= len;

            glm::vec3 t = p1 - p0;
            if ( glm::dot( t, t ) < 1e-12f )
                t = p3 - p0;
            t                 = glm::normalize( t );
            const glm::vec3 b = glm::normalize( glm::cross( n, t ) );

            const uint32_t base = static_cast<uint32_t>( m.Vertices.size() );
            for ( const glm::vec3& p : { p0, p1, p2, p3 } )
            {
                Vertex v;
                v.Position  = p;
                v.Normal    = n;
                v.Tangent   = t;
                v.Bitangent = b;
                v.TexCoord  = { glm::dot( p, t ) * kUvPerUnit, glm::dot( p, b ) * kUvPerUnit };
                m.Vertices.push_back( v );
            }
            m.Indices.push_back( { base + 0, base + 1, base + 2 } );
            m.Indices.push_back( { base + 2, base + 3, base + 0 } );
        }

        inline void AddTriangle( ShapeMesh& m, const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2 )
        {
            glm::vec3   n   = glm::cross( p1 - p0, p2 - p0 );
            const float len = glm::length( n );
            if ( len < 1e-9f )
                return;
            n /= len;

            const glm::vec3 t = glm::normalize( p1 - p0 );
            const glm::vec3 b = glm::normalize( glm::cross( n, t ) );

            const uint32_t base = static_cast<uint32_t>( m.Vertices.size() );
            for ( const glm::vec3& p : { p0, p1, p2 } )
            {
                Vertex v;
                v.Position  = p;
                v.Normal    = n;
                v.Tangent   = t;
                v.Bitangent = b;
                v.TexCoord  = { glm::dot( p, t ) * kUvPerUnit, glm::dot( p, b ) * kUvPerUnit };
                m.Vertices.push_back( v );
            }
            m.Indices.push_back( { base + 0, base + 1, base + 2 } );
        }
    } // namespace Detail

    // Box with per-axis subdivisions (UE's Width / Depth / Height + Subdivisions). Subdivisions exist so a
    // later deform / displace has geometry to work with; 1 each is a plain 12-triangle box.
    inline ShapeMesh MakeBox( const glm::vec3& size, const glm::ivec3& subdivisions = glm::ivec3( 1 ) )
    {
        ShapeMesh        m;
        const glm::vec3  s( std::max( size.x, Detail::kMinExtent ), std::max( size.y, Detail::kMinExtent ),
                            std::max( size.z, Detail::kMinExtent ) );
        const glm::ivec3 n = glm::max( subdivisions, glm::ivec3( 1 ) );

        const glm::vec3 mn( -s.x * 0.5f, 0.0f, -s.z * 0.5f );
        const glm::vec3 mx( s.x * 0.5f, s.y, s.z * 0.5f );

        // Each face is a grid of quads: walk the two in-plane axes, keep the third pinned to a side.
        const auto face = [&]( int axis, bool positive )
        {
            const int u = ( axis + 1 ) % 3;
            const int v = ( axis + 2 ) % 3;

            const int   nu  = n[u];
            const int   nv  = n[v];
            const float fix = positive ? mx[axis] : mn[axis];
            const float du  = ( mx[u] - mn[u] ) / static_cast<float>( nu );
            const float dv  = ( mx[v] - mn[v] ) / static_cast<float>( nv );

            for ( int i = 0; i < nu; ++i )
            {
                for ( int j = 0; j < nv; ++j )
                {
                    const float u0 = mn[u] + du * i;
                    const float u1 = u0 + du;
                    const float v0 = mn[v] + dv * j;
                    const float v1 = v0 + dv;

                    const auto P = [&]( float uu, float vv )
                    {
                        glm::vec3 p;
                        p[axis] = fix;
                        p[u]    = uu;
                        p[v]    = vv;
                        return p;
                    };
                    // Winding flips with the side so every face points outward.
                    if ( positive )
                        Detail::AddQuad( m, P( u0, v0 ), P( u1, v0 ), P( u1, v1 ), P( u0, v1 ) );
                    else
                        Detail::AddQuad( m, P( u0, v0 ), P( u0, v1 ), P( u1, v1 ), P( u1, v0 ) );
                }
            }
        };

        for ( int axis = 0; axis < 3; ++axis )
        {
            face( axis, true );
            face( axis, false );
        }
        return m;
    }

    // UV sphere. Smooth-shaded (the normal IS the direction from the centre) — a faceted ball is never what
    // anyone wants — with a spherical UV map rather than the world-aligned one.
    inline ShapeMesh MakeSphere( float diameter, int slices = 24, int stacks = 16 )
    {
        ShapeMesh   m;
        const float r = std::max( diameter, Detail::kMinExtent ) * 0.5f;
        slices        = std::max( slices, 3 );
        stacks        = std::max( stacks, 2 );

        const glm::vec3 centre( 0.0f, r, 0.0f ); // resting on the floor like every other shape here

        const auto push = [&]( int stack, int slice )
        {
            const float phi = glm::pi<float>() * static_cast<float>( stack ) / static_cast<float>( stacks );
            const float theta =
                 2.0f * glm::pi<float>() * static_cast<float>( slice ) / static_cast<float>( slices );
            const glm::vec3 dir( std::sin( phi ) * std::cos( theta ), std::cos( phi ),
                                 std::sin( phi ) * std::sin( theta ) );

            Vertex v;
            v.Position = centre + dir * r;
            v.Normal   = dir;
            // Tangent along +theta, so the tangent frame follows the UV seams.
            v.Tangent   = glm::normalize( glm::vec3( -std::sin( theta ), 0.0f, std::cos( theta ) ) );
            v.Bitangent = glm::normalize( glm::cross( v.Normal, v.Tangent ) );
            v.TexCoord  = { static_cast<float>( slice ) / static_cast<float>( slices ),
                            static_cast<float>( stack ) / static_cast<float>( stacks ) };
            m.Vertices.push_back( v );
            return static_cast<uint32_t>( m.Vertices.size() - 1 );
        };

        // A full grid of (stacks+1) x (slices+1) vertices: the seam column is duplicated so its UVs can
        // wrap to 1.0 instead of snapping back to 0.
        std::vector<std::vector<uint32_t>> grid( stacks + 1 );
        for ( int i = 0; i <= stacks; ++i )
        {
            grid[i].resize( slices + 1 );
            for ( int j = 0; j <= slices; ++j )
                grid[i][j] = push( i, j );
        }

        for ( int i = 0; i < stacks; ++i )
        {
            for ( int j = 0; j < slices; ++j )
            {
                const uint32_t a = grid[i][j];
                const uint32_t b = grid[i + 1][j];
                const uint32_t c = grid[i + 1][j + 1];
                const uint32_t d = grid[i][j + 1];
                if ( i != 0 ) // the pole row degenerates to a triangle
                    m.Indices.push_back( { a, b, d } );
                if ( i != stacks - 1 )
                    m.Indices.push_back( { b, c, d } );
            }
        }
        return m;
    }

    // Cylinder standing on its base. `capped` closes both ends with a fan.
    inline ShapeMesh MakeCylinder( float diameter, float height, int slices = 24, bool capped = true )
    {
        ShapeMesh   m;
        const float r = std::max( diameter, Detail::kMinExtent ) * 0.5f;
        const float h = std::max( height, Detail::kMinExtent );
        slices        = std::max( slices, 3 );

        const auto ring = [&]( int i, float y )
        {
            const float a = 2.0f * glm::pi<float>() * static_cast<float>( i ) / static_cast<float>( slices );
            return glm::vec3( std::cos( a ) * r, y, std::sin( a ) * r );
        };

        for ( int i = 0; i < slices; ++i )
        {
            const glm::vec3 b0 = ring( i, 0.0f );
            const glm::vec3 b1 = ring( i + 1, 0.0f );
            Detail::AddQuad( m, b0, b1, ring( i + 1, h ), ring( i, h ) );
        }

        if ( capped )
        {
            for ( int i = 0; i < slices; ++i )
            {
                Detail::AddTriangle( m, glm::vec3( 0.0f, h, 0.0f ), ring( i, h ), ring( i + 1, h ) );
                Detail::AddTriangle( m, glm::vec3( 0.0f, 0.0f, 0.0f ), ring( i + 1, 0.0f ), ring( i, 0.0f ) );
            }
        }
        return m;
    }

    // Cone standing on its base (a closed disc + a fan to the apex).
    inline ShapeMesh MakeCone( float diameter, float height, int slices = 24 )
    {
        ShapeMesh   m;
        const float r = std::max( diameter, Detail::kMinExtent ) * 0.5f;
        const float h = std::max( height, Detail::kMinExtent );
        slices        = std::max( slices, 3 );

        const auto ring = [&]( int i )
        {
            const float a = 2.0f * glm::pi<float>() * static_cast<float>( i ) / static_cast<float>( slices );
            return glm::vec3( std::cos( a ) * r, 0.0f, std::sin( a ) * r );
        };

        const glm::vec3 apex( 0.0f, h, 0.0f );
        for ( int i = 0; i < slices; ++i )
        {
            Detail::AddTriangle( m, ring( i ), ring( i + 1 ), apex );
            Detail::AddTriangle( m, glm::vec3( 0.0f ), ring( i + 1 ), ring( i ) ); // base
        }
        return m;
    }

    // A straight staircase climbing along +Z: `steps` boxes, each one step deeper and higher. The whole
    // flight is centred on X and starts at Z = 0, so its footprint is width x (steps * stepDepth).
    inline ShapeMesh MakeStairs( float width, float stepDepth, float stepHeight, int steps = 8 )
    {
        ShapeMesh   m;
        const float w  = std::max( width, Detail::kMinExtent );
        const float sd = std::max( stepDepth, Detail::kMinExtent );
        const float sh = std::max( stepHeight, Detail::kMinExtent );
        steps          = std::max( steps, 1 );

        // Each step is a separate closed box from the ground up. Every box is watertight on its own, but
        // the FLIGHT is not one manifold: neighbouring steps each emit their own face on the wall they
        // share, so the lower part of that wall is two overlapping, opposite-facing quads and the union
        // has interior faces and T-junctions.
        // Fine for rendering and for a per-box collider; a boolean or a manifold-only consumer needs the
        // single-shell flight, which does not exist yet.
        for ( int i = 0; i < steps; ++i )
        {
            const float z0 = static_cast<float>( i ) * sd;
            const float z1 = z0 + sd;
            const float y1 = static_cast<float>( i + 1 ) * sh;

            const glm::vec3 a( -w * 0.5f, 0.0f, z0 );
            const glm::vec3 b( w * 0.5f, 0.0f, z0 );
            const glm::vec3 c( w * 0.5f, 0.0f, z1 );
            const glm::vec3 d( -w * 0.5f, 0.0f, z1 );
            const glm::vec3 A( a.x, y1, a.z );
            const glm::vec3 B( b.x, y1, b.z );
            const glm::vec3 C( c.x, y1, c.z );
            const glm::vec3 D( d.x, y1, d.z );

            Detail::AddQuad( m, A, B, C, D ); // tread (top)
            Detail::AddQuad( m, a, d, c, b ); // bottom
            Detail::AddQuad( m, a, b, B, A ); // riser (front, -Z)
            Detail::AddQuad( m, c, d, D, C ); // back (+Z)
            Detail::AddQuad( m, b, c, C, B ); // right (+X)
            Detail::AddQuad( m, d, a, A, D ); // left (-X)
        }
        return m;
    }
} // namespace Desert::Geometry
