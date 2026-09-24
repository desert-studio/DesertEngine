#pragma once

#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>

#include <Common/Core/Math/AABB.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/Units.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace Desert::Geometry
{
    // THE ONE SOURCE OF PRIMITIVE GEOMETRY. Every shape the engine draws without an asset comes from here:
    // the scene's `Primitive` meshes (PrimitiveMeshFactory is a GPU cache over MakePrimitive), and the
    // Modeling Create tool's shapes (ShapeToEditMesh). The pattern is UE's UAddPrimitiveTool: Box, Sphere,
    // Cylinder, Cone, Capsule, Pyramid, Stairs, each with a polygroup mode and a pivot.
    //
    // Pure CPU: vertices + triangles + one polygroup per triangle, nothing GPU, nothing ECS.
    //
    // Conventions, shared by every generator here:
    //   * world units are CENTIMETRES and sizes are FULL extents (a diameter, not a radius);
    //   * +Y is up, the shape is centred on X/Z, and ShapeOptions::Pivot says where Y = 0 falls;
    //   * every closed shape is ONE closed, two-manifold, outward-wound shell once its coincident corners
    //     are welded (ShapeToEditMesh) - the ShapeGenerators suite checks that for every shape and mode;
    //   * flat faces have hard normals and a 0..1 UV square over the whole logical face; round surfaces
    //     are smooth-shaded and wrap U once around the axis, with V running 0 at the top to 1 at the bottom.

    // How triangles are gathered into polygroups (UE's EMakeMeshPolygroupMode).
    enum class ShapePolygroupMode
    {
        PerFace, // one group per logical face: a box has 6, a cylinder 3 (bottom, side, top)
        PerQuad, // one group per quad, and per triangle where a row closes on a pole or an apex
        Single,  // the whole shape is one group
    };

    // Where the shape's origin sits on its vertical extent (UE's pivot Base / Centre / Top).
    enum class ShapePivot
    {
        Base,
        Centre,
        Top,
    };

    struct ShapeOptions
    {
        ShapePolygroupMode Groups = ShapePolygroupMode::PerFace;
        ShapePivot         Pivot  = ShapePivot::Base;
    };

    constexpr const char* ToString( ShapePolygroupMode mode )
    {
        switch ( mode )
        {
            case ShapePolygroupMode::PerFace:
                return "Per Face";
            case ShapePolygroupMode::PerQuad:
                return "Per Quad";
            case ShapePolygroupMode::Single:
                return "Single";
        }
        return "Single";
    }

    constexpr const char* ToString( ShapePivot pivot )
    {
        switch ( pivot )
        {
            case ShapePivot::Base:
                return "Base";
            case ShapePivot::Centre:
                return "Centre";
            case ShapePivot::Top:
                return "Top";
        }
        return "Base";
    }

    struct ShapeMesh
    {
        std::vector<Vertex> Vertices;
        std::vector<Index>  Indices;
        std::vector<int>    Groups; // one per entry of Indices, numbered 0..GroupCount()-1

        [[nodiscard]] int GroupCount() const
        {
            int highest = -1;
            for ( const int g : Groups )
                highest = std::max( highest, g );
            return highest + 1;
        }

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
        // Smallest extent any shape is allowed to have, in world units (= 1 mm). A zero or negative size
        // is a slider being dragged to its end, not an intent to make an infinitely thin object: clamping
        // to something VISIBLE keeps the mesh well-formed and the shape recoverable by dragging back.
        inline constexpr float kMinExtent = 0.1f;

        // Hands out polygroup ids in the order faces are emitted, so ids are dense from 0 in every mode.
        class GroupAssigner
        {
        public:
            explicit GroupAssigner( ShapePolygroupMode mode ) : m_Mode( mode )
            {
            }

            // The group of one quad (or lone triangle) of logical face `face`.
            int Next( int face )
            {
                switch ( m_Mode )
                {
                    case ShapePolygroupMode::Single:
                        return 0;
                    case ShapePolygroupMode::PerQuad:
                        return m_Count++;
                    case ShapePolygroupMode::PerFace:
                        break;
                }
                if ( face >= static_cast<int>( m_FaceGroup.size() ) )
                    m_FaceGroup.resize( static_cast<size_t>( face ) + 1, -1 );
                int& group = m_FaceGroup[static_cast<size_t>( face )];
                if ( group < 0 )
                    group = m_Count++;
                return group;
            }

        private:
            ShapePolygroupMode m_Mode;
            int                m_Count = 0;
            std::vector<int>   m_FaceGroup;
        };

        inline uint32_t PushVertex( ShapeMesh& m, const glm::vec3& p, const glm::vec3& n, const glm::vec3& t,
                                    const glm::vec2& uv )
        {
            Vertex v{};
            v.Position  = p;
            v.Normal    = n;
            v.Tangent   = t;
            v.Bitangent = glm::normalize( glm::cross( n, t ) );
            v.TexCoord  = uv;
            m.Vertices.push_back( v );
            return static_cast<uint32_t>( m.Vertices.size() - 1 );
        }

        // One flat quad, p0..p3 counter-clockwise as seen from outside, with the UV of each corner.
        inline void AddQuad( ShapeMesh& m, int group, const std::array<glm::vec3, 4>& p,
                             const std::array<glm::vec2, 4>& uv )
        {
            const glm::vec3 n    = glm::normalize( glm::cross( p[1] - p[0], p[3] - p[0] ) );
            const glm::vec3 t    = glm::normalize( p[1] - p[0] );
            const auto      base = static_cast<uint32_t>( m.Vertices.size() );
            for ( int i = 0; i < 4; ++i )
                PushVertex( m, p[i], n, t, uv[i] );
            m.Indices.push_back( { base + 0, base + 1, base + 2 } );
            m.Indices.push_back( { base + 2, base + 3, base + 0 } );
            m.Groups.push_back( group );
            m.Groups.push_back( group );
        }

        // One flat triangle, counter-clockwise as seen from outside.
        inline void AddTriangle( ShapeMesh& m, int group, const std::array<glm::vec3, 3>& p,
                                 const std::array<glm::vec2, 3>& uv )
        {
            const glm::vec3 n    = glm::normalize( glm::cross( p[1] - p[0], p[2] - p[0] ) );
            const glm::vec3 t    = glm::normalize( p[1] - p[0] );
            const auto      base = static_cast<uint32_t>( m.Vertices.size() );
            for ( int i = 0; i < 3; ++i )
                PushVertex( m, p[i], n, t, uv[i] );
            m.Indices.push_back( { base + 0, base + 1, base + 2 } );
            m.Groups.push_back( group );
        }

        // A flat rectangular face split into nu x nv quads: corner `origin`, full edges `u` and `v`, facing
        // cross(u, v). UV is 0..1 over the whole face, U along `u`.
        inline void AddFaceGrid( ShapeMesh& m, GroupAssigner& groups, int face, const glm::vec3& origin,
                                 const glm::vec3& u, const glm::vec3& v, int nu, int nv )
        {
            for ( int i = 0; i < nu; ++i )
            {
                for ( int j = 0; j < nv; ++j )
                {
                    const float     u0 = static_cast<float>( i ) / static_cast<float>( nu );
                    const float     u1 = static_cast<float>( i + 1 ) / static_cast<float>( nu );
                    const float     v0 = static_cast<float>( j ) / static_cast<float>( nv );
                    const float     v1 = static_cast<float>( j + 1 ) / static_cast<float>( nv );
                    const glm::vec3 p0 = origin + u * u0 + v * v0;
                    const glm::vec3 p1 = origin + u * u1 + v * v0;
                    const glm::vec3 p2 = origin + u * u1 + v * v1;
                    const glm::vec3 p3 = origin + u * u0 + v * v1;
                    AddQuad(
                         m, groups.Next( face ), { p0, p1, p2, p3 },
                         { glm::vec2( u0, v0 ), glm::vec2( u1, v0 ), glm::vec2( u1, v1 ), glm::vec2( u0, v1 ) } );
                }
            }
        }

        // ── LATHE: every round shape is a profile revolved about +Y ──────────────────────────────────
        //
        // A profile point is (radius, height). The profile runs from the bottom of the shape to its top on
        // the OUTSIDE, so revolving a segment counter-clockwise (seen from above) winds it outward. A
        // segment whose end lies on the axis (radius 0) closes into one triangle per slice - a pole or an
        // apex - so the cap centre and the sphere poles are ONE vertex after welding, not a hole.
        enum class LatheUV
        {
            Wrap,   // U = slice / slices, V = the segment's own V0 -> V1
            Planar, // a disc seen from above: U, V = x, z mapped from [-R, R] to [0, 1]
        };

        struct LatheSegment
        {
            glm::vec2 P0{}, P1{}; // (radius, height)
            glm::vec2 N0{}, N1{}; // outward normal in (radial, up) at each end
            float     V0   = 0.0f;
            float     V1   = 0.0f;
            LatheUV   UV   = LatheUV::Wrap;
            int       Face = 0;
        };

        inline void Lathe( ShapeMesh& m, GroupAssigner& groups, const std::vector<LatheSegment>& profile,
                           int slices, float planarRadius )
        {
            constexpr float kOnAxis = 1e-6f;
            for ( const LatheSegment& s : profile )
            {
                // Two rows of slices + 1 vertices (the seam column is duplicated so U can reach 1).
                const auto column = [&]( int i, const glm::vec2& p, const glm::vec2& n, float v )
                {
                    const float a = glm::two_pi<float>() * static_cast<float>( i ) / static_cast<float>( slices );
                    const glm::vec3 dir( std::cos( a ), 0.0f, std::sin( a ) );
                    const glm::vec3 pos = dir * p.x + glm::vec3( 0.0f, p.y, 0.0f );
                    const glm::vec3 nrm = glm::normalize( dir * n.x + glm::vec3( 0.0f, n.y, 0.0f ) );
                    if ( s.UV == LatheUV::Planar )
                    {
                        const glm::vec2 uv( 0.5f + 0.5f * pos.x / planarRadius,
                                            0.5f + 0.5f * pos.z / planarRadius );
                        return PushVertex( m, pos, nrm, glm::vec3( 1.0f, 0.0f, 0.0f ), uv );
                    }
                    const glm::vec3 tangent( -std::sin( a ), 0.0f, std::cos( a ) );
                    return PushVertex( m, pos, nrm, tangent,
                                       glm::vec2( static_cast<float>( i ) / static_cast<float>( slices ), v ) );
                };
                std::vector<uint32_t> row0( static_cast<size_t>( slices ) + 1 );
                std::vector<uint32_t> row1( static_cast<size_t>( slices ) + 1 );
                for ( int i = 0; i <= slices; ++i )
                {
                    row0[static_cast<size_t>( i )] = column( i, s.P0, s.N0, s.V0 );
                    row1[static_cast<size_t>( i )] = column( i, s.P1, s.N1, s.V1 );
                }
                const bool startOnAxis = s.P0.x < kOnAxis;
                const bool endOnAxis   = s.P1.x < kOnAxis;
                for ( int i = 0; i < slices; ++i )
                {
                    const uint32_t a     = row0[static_cast<size_t>( i )];
                    const uint32_t b     = row0[static_cast<size_t>( i ) + 1];
                    const uint32_t c     = row1[static_cast<size_t>( i ) + 1];
                    const uint32_t d     = row1[static_cast<size_t>( i )];
                    const int      group = groups.Next( s.Face );
                    if ( !endOnAxis )
                    {
                        m.Indices.push_back( { a, d, c } );
                        m.Groups.push_back( group );
                    }
                    if ( !startOnAxis )
                    {
                        m.Indices.push_back( { c, b, a } );
                        m.Groups.push_back( group );
                    }
                }
            }
        }

        // Adds `rings` segments of a quarter circle of radius r centred at (0, centreY), from angle
        // `fromPhi` to `toPhi` (0 = straight down, pi/2 = the equator, pi = straight up), with V running
        // from v0 to v1 across them.
        inline void AddArc( std::vector<LatheSegment>& profile, float r, float centreY, float fromPhi, float toPhi,
                            int rings, float v0, float v1, int face )
        {
            for ( int k = 0; k < rings; ++k )
            {
                const float     f0 = static_cast<float>( k ) / static_cast<float>( rings );
                const float     f1 = static_cast<float>( k + 1 ) / static_cast<float>( rings );
                const float     a0 = fromPhi + ( toPhi - fromPhi ) * f0;
                const float     a1 = fromPhi + ( toPhi - fromPhi ) * f1;
                const glm::vec2 n0( std::sin( a0 ), -std::cos( a0 ) );
                const glm::vec2 n1( std::sin( a1 ), -std::cos( a1 ) );
                LatheSegment    s;
                s.P0 = glm::vec2( 0.0f, centreY ) + n0 * r;
                s.P1 = glm::vec2( 0.0f, centreY ) + n1 * r;
                // sin(pi) is not 0 in float: snap a pole onto the axis so the Lathe closes it.
                if ( s.P0.x < 1e-4f * r )
                    s.P0.x = 0.0f;
                if ( s.P1.x < 1e-4f * r )
                    s.P1.x = 0.0f;
                s.N0   = n0;
                s.N1   = n1;
                s.V0   = v0 + ( v1 - v0 ) * f0;
                s.V1   = v0 + ( v1 - v0 ) * f1;
                s.Face = face;
                profile.push_back( s );
            }
        }

        // Moves the shape so ShapeOptions::Pivot is at Y = 0. Every generator builds with its base on 0.
        inline void ApplyPivot( ShapeMesh& m, ShapePivot pivot )
        {
            const Common::Math::AABB box = m.Bounds();
            float                    dy  = 0.0f;
            switch ( pivot )
            {
                case ShapePivot::Base:
                    dy = -box.Min.y;
                    break;
                case ShapePivot::Centre:
                    dy = -0.5f * ( box.Min.y + box.Max.y );
                    break;
                case ShapePivot::Top:
                    dy = -box.Max.y;
                    break;
            }
            for ( Vertex& v : m.Vertices )
                v.Position.y += dy;
        }

        inline float Extent( float value )
        {
            return std::max( value, kMinExtent );
        }
    } // namespace Detail

    // Box with per-axis subdivisions (UE's Width / Height / Depth + Subdivisions). Logical faces: 6.
    inline ShapeMesh MakeBox( const glm::vec3& size, const glm::ivec3& subdivisions = glm::ivec3( 1 ),
                              const ShapeOptions& options = {} )
    {
        ShapeMesh             m;
        Detail::GroupAssigner groups( options.Groups );
        const glm::vec3       s( Detail::Extent( size.x ), Detail::Extent( size.y ), Detail::Extent( size.z ) );
        const glm::ivec3      n = glm::max( subdivisions, glm::ivec3( 1 ) );
        const glm::vec3       lo( -s.x * 0.5f, 0.0f, -s.z * 0.5f );
        const glm::vec3       hi( s.x * 0.5f, s.y, s.z * 0.5f );
        for ( int axis = 0; axis < 3; ++axis )
        {
            const int u = ( axis + 1 ) % 3;
            const int v = ( axis + 2 ) % 3;
            glm::vec3 eu( 0.0f );
            glm::vec3 ev( 0.0f );
            eu[u] = s[u];
            ev[v] = s[v];
            // cross(eu, ev) is +axis, so the positive side takes (eu, ev) and the negative side (ev, eu).
            glm::vec3 positive = lo;
            positive[axis]     = hi[axis];
            Detail::AddFaceGrid( m, groups, axis * 2, positive, eu, ev, n[u], n[v] );
            Detail::AddFaceGrid( m, groups, axis * 2 + 1, lo, ev, eu, n[v], n[u] );
        }
        Detail::ApplyPivot( m, options.Pivot );
        return m;
    }

    // A flat card in the XY plane facing +Z (the scene's `Plane` primitive), subdivided nx x ny. Open: the
    // one shape here that is not a closed shell. Logical faces: 1.
    inline ShapeMesh MakePlane( const glm::vec2& size, const glm::ivec2& subdivisions = glm::ivec2( 1 ),
                                const ShapeOptions& options = {} )
    {
        ShapeMesh             m;
        Detail::GroupAssigner groups( options.Groups );
        const glm::vec2       s( Detail::Extent( size.x ), Detail::Extent( size.y ) );
        const glm::ivec2      n = glm::max( subdivisions, glm::ivec2( 1 ) );
        Detail::AddFaceGrid( m, groups, 0, glm::vec3( -s.x * 0.5f, 0.0f, 0.0f ), glm::vec3( s.x, 0.0f, 0.0f ),
                             glm::vec3( 0.0f, s.y, 0.0f ), n.x, n.y );
        Detail::ApplyPivot( m, options.Pivot );
        return m;
    }

    // UV sphere: `slices` around the axis, `stacks` from pole to pole. Logical faces: 1.
    inline ShapeMesh MakeSphere( float diameter, int slices = 24, int stacks = 16,
                                 const ShapeOptions& options = {} )
    {
        ShapeMesh             m;
        Detail::GroupAssigner groups( options.Groups );
        const float           r = Detail::Extent( diameter ) * 0.5f;
        slices                  = std::max( slices, 3 );
        stacks                  = std::max( stacks, 2 );
        std::vector<Detail::LatheSegment> profile;
        Detail::AddArc( profile, r, r, 0.0f, glm::pi<float>(), stacks, 1.0f, 0.0f, 0 );
        Detail::Lathe( m, groups, profile, slices, r );
        Detail::ApplyPivot( m, options.Pivot );
        return m;
    }

    // Cylinder: a smooth side and two flat caps. Logical faces: 3 (bottom, side, top).
    inline ShapeMesh MakeCylinder( float diameter, float height, int slices = 24,
                                   const ShapeOptions& options = {} )
    {
        ShapeMesh             m;
        Detail::GroupAssigner groups( options.Groups );
        const float           r                         = Detail::Extent( diameter ) * 0.5f;
        const float           h                         = Detail::Extent( height );
        slices                                          = std::max( slices, 3 );
        const std::vector<Detail::LatheSegment> profile = {
             { { 0.0f, 0.0f },
               { r, 0.0f },
               { 0.0f, -1.0f },
               { 0.0f, -1.0f },
               0.0f,
               0.0f,
               Detail::LatheUV::Planar,
               0 },
             { { r, 0.0f }, { r, h }, { 1.0f, 0.0f }, { 1.0f, 0.0f }, 1.0f, 0.0f, Detail::LatheUV::Wrap, 1 },
             { { r, h }, { 0.0f, h }, { 0.0f, 1.0f }, { 0.0f, 1.0f }, 0.0f, 0.0f, Detail::LatheUV::Planar, 2 },
        };
        Detail::Lathe( m, groups, profile, slices, r );
        Detail::ApplyPivot( m, options.Pivot );
        return m;
    }

    // Cone: a flat base and a smooth side closing on the apex. Logical faces: 2 (base, side).
    inline ShapeMesh MakeCone( float diameter, float height, int slices = 24, const ShapeOptions& options = {} )
    {
        ShapeMesh             m;
        Detail::GroupAssigner groups( options.Groups );
        const float           r = Detail::Extent( diameter ) * 0.5f;
        const float           h = Detail::Extent( height );
        slices                  = std::max( slices, 3 );
        const glm::vec2 side    = glm::normalize( glm::vec2( h, r ) ); // perpendicular to the slant
        const std::vector<Detail::LatheSegment> profile = {
             { { 0.0f, 0.0f },
               { r, 0.0f },
               { 0.0f, -1.0f },
               { 0.0f, -1.0f },
               0.0f,
               0.0f,
               Detail::LatheUV::Planar,
               0 },
             { { r, 0.0f }, { 0.0f, h }, side, side, 1.0f, 0.0f, Detail::LatheUV::Wrap, 1 },
        };
        Detail::Lathe( m, groups, profile, slices, r );
        Detail::ApplyPivot( m, options.Pivot );
        return m;
    }

    // Capsule: a cylinder closed by two hemispheres, `height` end to end (never less than the diameter),
    // `rings` segments per hemisphere. Logical faces: 3 (bottom cap, side, top cap).
    inline ShapeMesh MakeCapsule( float diameter, float height, int slices = 24, int rings = 8,
                                  const ShapeOptions& options = {} )
    {
        ShapeMesh             m;
        Detail::GroupAssigner groups( options.Groups );
        const float           d                  = Detail::Extent( diameter );
        const float           r                  = d * 0.5f;
        const float           body               = std::max( Detail::Extent( height ) - d, Detail::kMinExtent );
        slices                                   = std::max( slices, 3 );
        rings                                    = std::max( rings, 1 );
        const float                       total  = d + body;
        const auto                        halfPi = glm::half_pi<float>();
        std::vector<Detail::LatheSegment> profile;
        Detail::AddArc( profile, r, r, 0.0f, halfPi, rings, 1.0f, 1.0f - r / total, 0 );
        profile.push_back( { { r, r },
                             { r, r + body },
                             { 1.0f, 0.0f },
                             { 1.0f, 0.0f },
                             1.0f - r / total,
                             r / total,
                             Detail::LatheUV::Wrap,
                             1 } );
        Detail::AddArc( profile, r, r + body, halfPi, glm::pi<float>(), rings, r / total, 0.0f, 2 );
        Detail::Lathe( m, groups, profile, slices, r );
        Detail::ApplyPivot( m, options.Pivot );
        return m;
    }

    // Square pyramid: a base and four triangles meeting at the apex. Logical faces: 5.
    inline ShapeMesh MakePyramid( const glm::vec3& size, const ShapeOptions& options = {} )
    {
        ShapeMesh             m;
        Detail::GroupAssigner groups( options.Groups );
        const glm::vec3       s( Detail::Extent( size.x ), Detail::Extent( size.y ), Detail::Extent( size.z ) );
        const float           hx = s.x * 0.5f;
        const float           hz = s.z * 0.5f;
        Detail::AddFaceGrid( m, groups, 0, glm::vec3( -hx, 0.0f, -hz ), glm::vec3( s.x, 0.0f, 0.0f ),
                             glm::vec3( 0.0f, 0.0f, s.z ), 1, 1 );
        // Base corners in the order that winds each side outward (checked by the suite's signed volume).
        const std::array<glm::vec3, 4> base = { glm::vec3( -hx, 0.0f, hz ), glm::vec3( hx, 0.0f, hz ),
                                                glm::vec3( hx, 0.0f, -hz ), glm::vec3( -hx, 0.0f, -hz ) };
        const glm::vec3                apex( 0.0f, s.y, 0.0f );
        for ( int i = 0; i < 4; ++i )
        {
            Detail::AddTriangle( m, groups.Next( 1 + i ), { base[i], base[( i + 1 ) % 4], apex },
                                 { glm::vec2( 0.0f, 0.0f ), glm::vec2( 1.0f, 0.0f ), glm::vec2( 0.5f, 1.0f ) } );
        }
        Detail::ApplyPivot( m, options.Pivot );
        return m;
    }

    // A straight flight climbing along +Z, `steps` treads, footprint width x (steps * stepDepth) centred
    // on X and Z. ONE closed shell: each side is a staircase of cells whose edges meet the risers, treads,
    // back and bottom exactly, so there is no interior face and no T-junction.
    // Logical faces: 4 + 2 * steps (two sides, back, bottom, every riser, every tread).
    inline ShapeMesh MakeStairs( float width, float stepDepth, float stepHeight, int steps = 8,
                                 const ShapeOptions& options = {} )
    {
        ShapeMesh             m;
        Detail::GroupAssigner groups( options.Groups );
        const float           w  = Detail::Extent( width );
        const float           sd = Detail::Extent( stepDepth );
        const float           sh = Detail::Extent( stepHeight );
        steps                    = std::max( steps, 1 );
        const auto  n            = static_cast<float>( steps );
        const float z0           = -0.5f * n * sd;
        const float hx           = 0.5f * w;
        int         face         = 0;

        // Sides: cell (column i along Z, row j up) exists for j <= i. UV is the side's own 0..1 square.
        for ( const float side : { hx, -hx } )
        {
            const int sideFace = face++;
            for ( int i = 0; i < steps; ++i )
            {
                for ( int j = 0; j <= i; ++j )
                {
                    const float     za = z0 + sd * static_cast<float>( i );
                    const float     zb = za + sd;
                    const float     ya = sh * static_cast<float>( j );
                    const float     yb = ya + sh;
                    const glm::vec2 ua( static_cast<float>( i ) / n, static_cast<float>( j ) / n );
                    const glm::vec2 ub( static_cast<float>( i + 1 ) / n, static_cast<float>( j + 1 ) / n );
                    // +X: first edge up then along +Z, cross(+Y, +Z) = +X; -X: the other way round.
                    if ( side > 0.0f )
                        Detail::AddQuad( m, groups.Next( sideFace ),
                                         { glm::vec3( side, ya, za ), glm::vec3( side, yb, za ),
                                           glm::vec3( side, yb, zb ), glm::vec3( side, ya, zb ) },
                                         { glm::vec2( ua.x, ua.y ), glm::vec2( ua.x, ub.y ),
                                           glm::vec2( ub.x, ub.y ), glm::vec2( ub.x, ua.y ) } );
                    else
                        Detail::AddQuad( m, groups.Next( sideFace ),
                                         { glm::vec3( side, ya, za ), glm::vec3( side, ya, zb ),
                                           glm::vec3( side, yb, zb ), glm::vec3( side, yb, za ) },
                                         { glm::vec2( ua.x, ua.y ), glm::vec2( ub.x, ua.y ),
                                           glm::vec2( ub.x, ub.y ), glm::vec2( ua.x, ub.y ) } );
                }
            }
        }
        // Back (+Z) and bottom (-Y), each split into one strip per step so its edges meet the side cells.
        Detail::AddFaceGrid( m, groups, face++, glm::vec3( -hx, 0.0f, -z0 ), glm::vec3( w, 0.0f, 0.0f ),
                             glm::vec3( 0.0f, n * sh, 0.0f ), 1, steps );
        Detail::AddFaceGrid( m, groups, face++, glm::vec3( -hx, 0.0f, z0 ), glm::vec3( w, 0.0f, 0.0f ),
                             glm::vec3( 0.0f, 0.0f, n * sd ), 1, steps );
        for ( int i = 0; i < steps; ++i )
        {
            const float za = z0 + sd * static_cast<float>( i );
            const float ya = sh * static_cast<float>( i );
            // Riser facing -Z: cross(-X, +Y) = -Z. Tread facing +Y: cross(+Z, +X) = +Y.
            Detail::AddFaceGrid( m, groups, face++, glm::vec3( hx, ya, za ), glm::vec3( -w, 0.0f, 0.0f ),
                                 glm::vec3( 0.0f, sh, 0.0f ), 1, 1 );
            Detail::AddFaceGrid( m, groups, face++, glm::vec3( -hx, ya + sh, za ), glm::vec3( 0.0f, 0.0f, sd ),
                                 glm::vec3( w, 0.0f, 0.0f ), 1, 1 );
        }
        Detail::ApplyPivot( m, options.Pivot );
        return m;
    }

    // ── THE SCENE'S PRIMITIVES ───────────────────────────────────────────────────────────────────
    //
    // The `Primitive` of a StaticMesh, built by the same generators as the Create tool. Each is authored
    // on the unit box [-0.5, 0.5] m with its pivot at the centre, which is what PrimitiveBounds states and
    // the ShapeGenerators suite holds against these vertices. Tessellation is fixed here: it is part of
    // what a saved scene looks like. The Capsule is half as wide as it is tall so it reads as a capsule.
    // Terrain and LightCube are built elsewhere (the landscape renderer, the light gizmo): nullopt.
    [[nodiscard]] inline std::optional<ShapeMesh> MakePrimitive( PrimitiveType type )
    {
        constexpr float    size = Common::Units::UnitsPerMetre;
        const ShapeOptions centred{ ShapePolygroupMode::PerFace, ShapePivot::Centre };
        switch ( type )
        {
            case PrimitiveType::Cube:
                return MakeBox( glm::vec3( size ), glm::ivec3( 1 ), centred );
            case PrimitiveType::Sphere:
                return MakeSphere( size, 32, 16, centred );
            case PrimitiveType::Plane:
                return MakePlane( glm::vec2( size ), glm::ivec2( 1 ), centred );
            case PrimitiveType::Pyramid:
                return MakePyramid( glm::vec3( size ), centred );
            case PrimitiveType::Cylinder:
                return MakeCylinder( size, size, 32, centred );
            case PrimitiveType::Capsule:
                return MakeCapsule( 0.5f * size, size, 32, 8, centred );
            case PrimitiveType::Terrain:
            case PrimitiveType::LightCube:
            case PrimitiveType::Count:
                return std::nullopt;
        }
        return std::nullopt;
    }

    // The shape as an EditMesh: coincident corners welded into one shell, hard edges and UV seams kept as
    // overlay seams, and ShapeMesh::Groups as the polygroups. REFUSED when the weld had to drop or detach
    // a triangle - a generator emitting a shape that does not weld into one clean shell is a defect in the
    // generator, and the Create tool must not hand the scene a mesh that disagrees with what it drew.
    [[nodiscard]] inline Common::ResultStr<EditMesh> ShapeToEditMesh( const ShapeMesh& shape )
    {
        if ( shape.Groups.size() != shape.Indices.size() )
            return Common::MakeFormattedError<EditMesh>( "ShapeToEditMesh: {} triangles but {} polygroup entries",
                                                         shape.Indices.size(), shape.Groups.size() );
        RenderMeshData render;
        render.Vertices = shape.Vertices;
        render.Indices  = shape.Indices;
        auto imported   = FromRenderMesh( render );
        if ( !imported.IsSuccess() )
            return Common::MakeFormattedError<EditMesh>( "ShapeToEditMesh: {}", imported.GetError() );
        ImportedEditMesh result = imported.ExtractValue();
        if ( result.DroppedDegenerate != 0 || result.DroppedDuplicate != 0 || result.DetachedTriangles != 0 )
            return Common::MakeFormattedError<EditMesh>(
                 "ShapeToEditMesh: the shape did not weld into one shell ({} degenerate, {} duplicate, {} "
                 "detached of {} triangles)",
                 result.DroppedDegenerate, result.DroppedDuplicate, result.DetachedTriangles,
                 shape.Indices.size() );
        // Nothing was dropped, so triangle k of the shape is triangle k of the fresh mesh.
        for ( size_t k = 0; k < shape.Groups.size(); ++k )
            result.Mesh.Attributes().SetPolyGroup( static_cast<int>( k ), shape.Groups[k] );
        return Common::MakeSuccess( std::move( result.Mesh ) );
    }
} // namespace Desert::Geometry
