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
#include <map>
#include <unordered_map>
#include <optional>
#include <vector>

namespace Desert::Geometry
{
    // THE ONE SOURCE OF PRIMITIVE GEOMETRY. Every shape the engine draws without an asset comes from here:
    // the scene's `Primitive` meshes (PrimitiveMeshFactory is a GPU cache over MakePrimitive), and the
    // Modeling Create tool's shapes (ShapeToEditMesh). The pattern is UE's UAddPrimitiveTool: Box, Sphere,
    // Cylinder, Cone, Capsule, Pyramid, Stairs (Linear / Floating / Curved / Spiral), Torus, Arrow, Disc and
    // Rectangle, each with a polygroup mode and a pivot.
    //
    // Pure CPU: vertices + triangles + one polygroup per triangle, nothing GPU, nothing ECS.
    //
    // Every shape with a UE generator behind it is that generator, ported (see "UE'S SHAPE GENERATORS" below):
    // its parameters, UVs, normals and polygroups are UE's. The Pyramid has no UE counterpart and stays this
    // file's own; the Stairs are UE's StairGenerator ported onto one lattice (see there).
    //
    // Conventions, shared by every generator here:
    //   * world units are CENTIMETRES; a shape's parameters are named and sized as UE names them;
    //   * +Y is up, the shape stands on its own axis as UE builds it, and ShapeOptions::Pivot says where
    //     Y = 0 falls (UE's pivot Base / Centre / Top, on the bounds);
    //   * every closed shape is ONE closed, two-manifold, outward-wound shell once its coincident corners
    //     are welded (ShapeToEditMesh) - the ShapeGenerators suite checks that for every shape and mode.

    // How triangles are gathered into polygroups: UE's EMakeMeshPolygroupMode, in UE's order.
    enum class ShapePolygroupMode
    {
        PerShape, // the whole shape is one group
        PerFace,  // one group per logical face, as each UE generator defines its faces
        PerQuad,  // one group per quad, and per triangle where a row closes on a pole or an apex
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
            case ShapePolygroupMode::PerShape:
                return "Per Shape";
        }
        return "Per Shape";
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
                    case ShapePolygroupMode::PerShape:
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
        // cross(u, v). UV is 0..1 over the whole face, U along `u`. The (nu+1) x (nv+1) grid points are
        // emitted once and shared by the quads around them, as UE's FGridBoxMeshGenerator does: a flat face
        // has one normal, one tangent and a continuous UV, so a private copy per quad carried nothing but four
        // times the vertices for the weld to fold back together.
        inline void AddFaceGrid( ShapeMesh& m, GroupAssigner& groups, int face, const glm::vec3& origin,
                                 const glm::vec3& u, const glm::vec3& v, int nu, int nv )
        {
            const glm::vec3 n    = glm::normalize( glm::cross( u, v ) );
            const glm::vec3 t    = glm::normalize( u );
            const auto      base = static_cast<uint32_t>( m.Vertices.size() );
            const auto      at   = [&]( int i, int j ) {
                return base + static_cast<uint32_t>( i ) * static_cast<uint32_t>( nv + 1 ) +
                       static_cast<uint32_t>( j );
            };
            for ( int i = 0; i <= nu; ++i )
                for ( int j = 0; j <= nv; ++j )
                {
                    const float s0 = static_cast<float>( i ) / static_cast<float>( nu );
                    const float t0 = static_cast<float>( j ) / static_cast<float>( nv );
                    PushVertex( m, origin + u * s0 + v * t0, n, t, glm::vec2( s0, t0 ) );
                }
            for ( int i = 0; i < nu; ++i )
                for ( int j = 0; j < nv; ++j )
                {
                    // p0..p3 counter-clockwise from outside: (i, j), (i+1, j), (i+1, j+1), (i, j+1).
                    const int group = groups.Next( face );
                    m.Indices.push_back( { at( i, j ), at( i + 1, j ), at( i + 1, j + 1 ) } );
                    m.Indices.push_back( { at( i + 1, j + 1 ), at( i, j + 1 ), at( i, j ) } );
                    m.Groups.push_back( group );
                    m.Groups.push_back( group );
                }
        }

        // Moves the shape so ShapeOptions::Pivot is at Y = 0 (UAddPrimitiveTool::UpdatePreviewMesh, on the bounds).
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

    // ── UE'S SHAPE GENERATORS, PORTED ─────────────────────────────────────────────────────────────
    //
    // Every shape of UE's Add Primitive palette that UE builds with a GeometryCore generator is that
    // generator, ported: the same vertices, the same triangles in the same order, UE's UV layout, UE's
    // normals (hard where UE splits them, smooth where UE shares them) and UE's polygroups. Each generator
    // writes UE's own buffers (Detail::UEShapeBuffers, FMeshShapeGenerator's arrays) in UE's coordinates,
    // and Detail::ToShapeMesh turns them into this file's split-vertex ShapeMesh once, at the end.
    //
    // The shape structs below are UE's UProcedural*ToolProperties (MeshModelingTools/Public/
    // AddPrimitiveTool.h:118-404): the same field names and defaults, in centimetres. The ranges the panel
    // and the `modeling` subject accept are UE's ClampMin/ClampMax, except that a size never goes below
    // Detail::kMinExtent (1 mm) - see there why.

    // UE's EProceduralRectType.
    enum class RectangleType
    {
        Rectangle,
        RoundedRectangle,
    };

    // UE's EProceduralDiscType.
    enum class DiscType
    {
        Disc,
        PuncturedDisc,
    };

    // UE's EProceduralSphereType.
    enum class SphereType
    {
        LatLong,
        Box,
    };

    constexpr const char* ToString( RectangleType type )
    {
        return type == RectangleType::RoundedRectangle ? "Rounded Rectangle" : "Rectangle";
    }

    constexpr const char* ToString( DiscType type )
    {
        return type == DiscType::PuncturedDisc ? "Punctured Disc" : "Disc";
    }

    constexpr const char* ToString( SphereType type )
    {
        return type == SphereType::Box ? "Box" : "Lat Long";
    }

    // UE's ClampMin/ClampMax of the counts (AddPrimitiveTool.h): every count stops at 500.
    inline constexpr int kMaxShapeCount = 500;

    struct BoxShape // UProceduralBoxToolProperties; built by MakeBox( { Width, Height, Depth }, ... )
    {
        float Width              = 100.0f; // X
        float Depth              = 100.0f; // Z
        float Height             = 100.0f; // Y
        int   WidthSubdivisions  = 1;
        int   DepthSubdivisions  = 1;
        int   HeightSubdivisions = 1;

        bool operator==( const BoxShape& ) const = default;
    };

    struct RectangleShape // UProceduralRectangleToolProperties
    {
        RectangleType Type               = RectangleType::Rectangle;
        float         Width              = 100.0f; // X
        float         Depth              = 100.0f; // Z
        int           WidthSubdivisions  = 1;
        int           DepthSubdivisions  = 1;
        bool          MaintainDimension  = true; // rounded: Width / Depth stay the outer size
        float         CornerRadius       = 25.0f;
        int           CornerSlices       = 16; // 3..500

        bool operator==( const RectangleShape& ) const = default;
    };

    struct DiscShape // UProceduralDiscToolProperties
    {
        DiscType Type               = DiscType::Disc;
        float    Radius             = 50.0f;
        int      RadialSlices       = 16; // 3..500
        int      RadialSubdivisions = 1;
        float    HoleRadius         = 25.0f; // punctured only

        bool operator==( const DiscShape& ) const = default;
    };

    struct TorusShape // UProceduralTorusToolProperties
    {
        float MajorRadius = 50.0f;
        float MinorRadius = 25.0f;
        int   MajorSlices = 16; // 3..500
        int   MinorSlices = 16; // 3..500

        bool operator==( const TorusShape& ) const = default;
    };

    struct CylinderShape // UProceduralCylinderToolProperties
    {
        float Radius             = 50.0f;
        float Height             = 200.0f;
        int   RadialSlices       = 16; // 3..500
        int   HeightSubdivisions = 1;

        bool operator==( const CylinderShape& ) const = default;
    };

    struct ConeShape // UProceduralConeToolProperties
    {
        float Radius             = 50.0f;
        float Height             = 200.0f;
        int   RadialSlices       = 16; // 3..500
        int   HeightSubdivisions = 1;

        bool operator==( const ConeShape& ) const = default;
    };

    struct ArrowShape // UProceduralArrowToolProperties
    {
        float ShaftRadius        = 20.0f;
        float ShaftHeight        = 200.0f;
        float HeadRadius         = 60.0f;
        float HeadHeight         = 120.0f;
        int   RadialSlices       = 16; // 3..500
        int   HeightSubdivisions = 1;

        bool operator==( const ArrowShape& ) const = default;
    };

    struct SphereShape // UProceduralSphereToolProperties
    {
        float      Radius           = 50.0f;
        SphereType SubdivisionType  = SphereType::Box;
        int        Subdivisions     = 16; // Box
        int        HorizontalSlices = 16; // Lat Long, 4..500
        int        VerticalSlices   = 16; // Lat Long, 4..500

        bool operator==( const SphereShape& ) const = default;
    };

    struct CapsuleShape // UProceduralCapsuleToolProperties
    {
        float Radius               = 25.0f;
        float CylinderLength       = 50.0f;
        int   HemisphereSlices     = 8;  // 2..500
        int   CylinderSlices       = 16; // 3..500
        int   CylinderSubdivisions = 1;  // 0..500

        bool operator==( const CapsuleShape& ) const = default;
    };

    namespace Detail
    {
        inline int Count( int value, int lowest )
        {
            return std::clamp( value, lowest, kMaxShapeCount );
        }

        // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Generators/MeshShapeGenerator.h:30-260
        // (FMeshShapeGenerator's buffers and setters), adapted: std::vector / glm; bReverseOrientation is
        // dropped because UAddPrimitiveTool never sets it, so only the per-call clockwise override remains.
        struct UEShapeBuffers
        {
            std::vector<glm::dvec3> Vertices;
            std::vector<glm::vec2>  UVs;
            std::vector<int>        UVParentVertex;
            std::vector<glm::vec3>  Normals;
            std::vector<int>        NormalParentVertex;
            std::vector<glm::ivec3> Triangles;
            std::vector<glm::ivec3> TriangleUVs;
            std::vector<glm::ivec3> TriangleNormals;
            std::vector<int>        TrianglePolygonIDs;

            void SetBufferSizes( int numVertices, int numTriangles, int numUVs, int numNormals )
            {
                Vertices.resize( static_cast<size_t>( numVertices ) );
                Triangles.resize( static_cast<size_t>( numTriangles ) );
                TriangleUVs.resize( static_cast<size_t>( numTriangles ) );
                TriangleNormals.resize( static_cast<size_t>( numTriangles ) );
                TrianglePolygonIDs.resize( static_cast<size_t>( numTriangles ) );
                UVs.resize( static_cast<size_t>( numUVs ) );
                UVParentVertex.resize( static_cast<size_t>( numUVs ) );
                Normals.resize( static_cast<size_t>( numNormals ) );
                NormalParentVertex.resize( static_cast<size_t>( numNormals ) );
            }
            void SetUV( int i, const glm::vec2& uv, int parent )
            {
                UVs[static_cast<size_t>( i )]           = uv;
                UVParentVertex[static_cast<size_t>( i )] = parent;
            }
            void SetNormal( int i, const glm::vec3& n, int parent )
            {
                Normals[static_cast<size_t>( i )]            = n;
                NormalParentVertex[static_cast<size_t>( i )] = parent;
            }
            static glm::ivec3 Order( int a, int b, int c, bool clockwise )
            {
                return clockwise ? glm::ivec3( c, b, a ) : glm::ivec3( a, b, c );
            }
            void SetTriangle( int i, int a, int b, int c, bool clockwise = false )
            {
                Triangles[static_cast<size_t>( i )] = Order( a, b, c, clockwise );
            }
            void SetTriangleUVs( int i, int a, int b, int c, bool clockwise = false )
            {
                TriangleUVs[static_cast<size_t>( i )] = Order( a, b, c, clockwise );
            }
            void SetTriangleNormals( int i, int a, int b, int c, bool clockwise = false )
            {
                TriangleNormals[static_cast<size_t>( i )] = Order( a, b, c, clockwise );
            }
            void SetTriangleWithMatchedUVNormal( int i, int a, int b, int c )
            {
                SetTriangle( i, a, b, c );
                SetTriangleUVs( i, a, b, c );
                SetTriangleNormals( i, a, b, c );
            }
            void SetTrianglePolygon( int i, int id )
            {
                TrianglePolygonIDs[static_cast<size_t>( i )] = id;
            }
        };

        // UE is left-handed with +Z up, +X forward and +Y right; this engine is right-handed with +Y up and
        // -Z forward. (x, y, z)_UE -> (y, z, -x) is that change of basis. It is a reflection, so it turns
        // UE's winding convention (normal = (C - A) x (B - A)) into this engine's ((B - A) x (C - A)) by
        // itself: no triangle is re-wound, and a texture reads the same way round as in UE.
        inline glm::vec3 FromUE( const glm::dvec3& p )
        {
            return glm::vec3( static_cast<float>( p.y ), static_cast<float>( p.z ), static_cast<float>( -p.x ) );
        }

        // UE's buffers as a ShapeMesh: one output vertex per distinct (vertex, UV element, normal element)
        // corner, so UE's UV and normal seams stay seams after the weld in ShapeToEditMesh. Polygroup ids are
        // renumbered densely in order of first use (the partition is UE's; PerShape collapses it to one, as
        // UAddPrimitiveTool::UpdatePreviewMesh does). Tangents follow +U, as the renderer's normal maps expect.
        inline ShapeMesh ToShapeMesh( const UEShapeBuffers& g, ShapePolygroupMode mode )
        {
            ShapeMesh                               m;
            // (vertex, UV, normal) packed 21 bits each: the largest buffer (a 500^3 box) holds 1.5M < 2^21.
            std::unordered_map<uint64_t, uint32_t> corners;
            corners.reserve( g.Triangles.size() * 3 );
            std::map<int, int>                      groups;
            std::vector<glm::vec3>                  tangents;
            for ( size_t t = 0; t < g.Triangles.size(); ++t )
            {
                std::array<uint32_t, 3> out{};
                for ( int k = 0; k < 3; ++k )
                {
                    const std::array<int, 3> key = { g.Triangles[t][k], g.TriangleUVs[t][k], g.TriangleNormals[t][k] };
                    const uint64_t packed        = ( static_cast<uint64_t>( key[0] ) << 42 ) |
                                            ( static_cast<uint64_t>( key[1] ) << 21 ) | static_cast<uint64_t>( key[2] );
                    const auto [it, fresh] = corners.emplace( packed, static_cast<uint32_t>( m.Vertices.size() ) );
                    if ( fresh )
                    {
                        Vertex v{};
                        v.Position = FromUE( g.Vertices[static_cast<size_t>( key[0] )] );
                        v.TexCoord = g.UVs[static_cast<size_t>( key[1] )];
                        v.Normal   = FromUE( glm::dvec3( g.Normals[static_cast<size_t>( key[2] )] ) );
                        m.Vertices.push_back( v );
                        tangents.emplace_back( 0.0f );
                    }
                    out[static_cast<size_t>( k )] = it->second;
                }
                m.Indices.push_back( { out[0], out[1], out[2] } );
                const int id = g.TrianglePolygonIDs[t];
                m.Groups.push_back( mode == ShapePolygroupMode::PerShape
                                         ? 0
                                         : groups.emplace( id, static_cast<int>( groups.size() ) ).first->second );

                const Vertex&   a  = m.Vertices[out[0]];
                const Vertex&   b  = m.Vertices[out[1]];
                const Vertex&   c  = m.Vertices[out[2]];
                const glm::vec3 e1 = b.Position - a.Position;
                const glm::vec3 e2 = c.Position - a.Position;
                const glm::vec2 d1 = b.TexCoord - a.TexCoord;
                const glm::vec2 d2 = c.TexCoord - a.TexCoord;
                const float     r  = d1.x * d2.y - d2.x * d1.y;
                if ( std::abs( r ) > 1e-12f )
                {
                    const glm::vec3 dPdU = ( e1 * d2.y - e2 * d1.y ) / r;
                    for ( const uint32_t i : out )
                        tangents[i] += dPdU;
                }
            }
            for ( size_t i = 0; i < m.Vertices.size(); ++i )
            {
                Vertex&   v = m.Vertices[i];
                glm::vec3 n = v.Normal;
                n           = glm::length( n ) > 1e-12f ? glm::normalize( n ) : glm::vec3( 0.0f, 1.0f, 0.0f );
                glm::vec3 t = tangents[i] - n * glm::dot( n, tangents[i] );
                if ( glm::length( t ) < 1e-8f )
                    t = glm::cross( n, std::abs( n.y ) < 0.9f ? glm::vec3( 0.0f, 1.0f, 0.0f )
                                                              : glm::vec3( 1.0f, 0.0f, 0.0f ) );
                v.Normal    = n;
                v.Tangent   = glm::normalize( t );
                v.Bitangent = glm::normalize( glm::cross( n, v.Tangent ) );
            }
            return m;
        }

        // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Generators/GridBoxMeshGenerator.h:35-280
        // (FGridBoxMeshGenerator::Generate), adapted: UE's shared corner / edge / face vertex bookkeeping
        // (lines 55-165) only decides WHICH index a face grid point gets; here every face grid point is its
        // own vertex at the same position, and the weld in ShapeToEditMesh joins the faces. UVs (with UE's
        // per-axis flips and aspect-ratio scale), normals, triangles and polygroups are UE's line for line.
        inline void GenerateGridBox( UEShapeBuffers& g, const glm::dvec3& extents, glm::ivec3 edgeVertices,
                                     bool polygroupPerQuad )
        {
            const glm::ivec3 N = glm::max( edgeVertices, glm::ivec3( 2 ) );
            const glm::dvec3 nScale( 1.0 / ( N.x - 1 ), 1.0 / ( N.y - 1 ), 1.0 / ( N.z - 1 ) );
            const glm::ivec3 nTri             = N - 1;
            const int        numUVsAndNormals = 2 * ( N.x * N.y + N.y * N.z + N.z * N.x );
            const int        numTriangles     = 4 * ( nTri.x * nTri.y + nTri.y * nTri.z + nTri.z * nTri.x );
            g.SetBufferSizes( numUVsAndNormals, numTriangles, numUVsAndNormals, numUVsAndNormals );

            constexpr int faceDimOrder[3] = { 1, 2, 0 };
            constexpr int dims[2][3]      = { { 1, 2, 0 }, { 2, 0, 1 } };
            const double  maxDimension    = 2.0 * std::max( { extents.x, extents.y, extents.z } );
            const float   uvScale         = static_cast<float>( 1.0 / maxDimension ); // bScaleUVByAspectRatio
            int           triIdx          = 0;
            int           uvIdx           = 0;
            int           quadIdx         = 0;
            for ( int dim = 0; dim < 3; ++dim )
            {
                const int    faceIdxBase    = faceDimOrder[dim] * 2;
                const int    d0             = dims[0][dim];
                const int    d1             = dims[1][dim];
                // UV-specific flips, set by UE to match its default cube texture arrangement.
                const int    minor1Flip[3]  = { -1, 1, 1 };
                const int    minor2Flip[3]  = { -1, -1, 1 };
                const double widthUVScale   = std::abs( extents[d0] ) * 2.0 * uvScale;
                const double heightUVScale  = std::abs( extents[d1] ) * 2.0 * uvScale;
                for ( int side = 0; side < 2; ++side )
                {
                    const int sideOpp  = 1 - side;
                    const int sideSign = side * 2 - 1;
                    glm::vec3 normal( 0.0f );
                    normal[dim]                = static_cast<float>( 2 * side - 1 );
                    const int majorFaceInd     = faceIdxBase + side;
                    const int faceUVStartInd   = uvIdx;
                    const int uvXDim           = dim == 1 ? 1 : 0;
                    const int uvYDim           = 1 - uvXDim;
                    for ( int i0 = 0; i0 < N[d0]; ++i0 )
                        for ( int i1 = 0; i1 < N[d1]; ++i1 )
                        {
                            glm::vec2 uv;
                            uv[uvXDim] = static_cast<float>( i0 * nScale[d0] - 0.5 );
                            uv[uvYDim] = static_cast<float>( i1 * nScale[d1] - 0.5 );
                            uv.x *= static_cast<float>( sideSign * minor1Flip[dim] );
                            uv.y *= static_cast<float>( minor2Flip[dim] );
                            uv[uvXDim] = static_cast<float>( ( uv[uvXDim] + 0.5f ) * widthUVScale );
                            uv[uvYDim] = static_cast<float>( ( uv[uvYDim] + 0.5f ) * heightUVScale );
                            // UE's FaceVertIndices[majorFaceInd][ToFaceV(dim, i0, i1)]: the box point on this
                            // side of `dim`, at grid step i0 along d0 and i1 along d1.
                            glm::dvec3 p;
                            p[dim] = side != 0 ? extents[dim] : -extents[dim];
                            p[d0]  = -extents[d0] + 2.0 * extents[d0] * i0 * nScale[d0];
                            p[d1]  = -extents[d1] + 2.0 * extents[d1] * i1 * nScale[d1];
                            g.Vertices[static_cast<size_t>( uvIdx )] = p;
                            g.SetUV( uvIdx, uv, uvIdx );
                            g.SetNormal( uvIdx, normal, uvIdx );
                            ++uvIdx;
                        }
                    const auto at = [&]( int i0, int i1 ) { return faceUVStartInd + i1 + i0 * N[d1]; };
                    for ( int i0 = 0; i0 + 1 < N[d0]; ++i0 )
                        for ( int i1 = 0; i1 + 1 < N[d1]; ++i1 )
                        {
                            const int group = polygroupPerQuad ? quadIdx : majorFaceInd;
                            g.SetTriangleWithMatchedUVNormal( triIdx, at( i0, i1 ), at( i0 + sideOpp, i1 + side ),
                                                              at( i0 + 1, i1 + 1 ) );
                            g.SetTrianglePolygon( triIdx++, group );
                            g.SetTriangleWithMatchedUVNormal( triIdx, at( i0, i1 ), at( i0 + 1, i1 + 1 ),
                                                              at( i0 + side, i1 + sideOpp ) );
                            g.SetTrianglePolygon( triIdx++, group );
                            ++quadIdx;
                        }
                }
            }
        }

        // UE's VectorUtil::BilinearInterp as RectangleMeshGenerator.cpp calls it: v01 is the +X corner.
        template <typename T, typename S>
        T Bilinear( const T& v00, const T& v01, const T& v11, const T& v10, S tx, S ty )
        {
            const T a = v00 * ( S( 1 ) - tx ) + v01 * tx;
            const T b = v10 * ( S( 1 ) - tx ) + v11 * tx;
            return a * ( S( 1 ) - ty ) + b * ty;
        }

        // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Generators/RectangleMeshGenerator.cpp:
        // 18-109 (FRectangleMeshGenerator::Generate), adapted: Origin = 0, Normal = +Z and IndicesMap = (0, 1),
        // the values UAddRectanglePrimitiveTool leaves them at.
        inline void GenerateRectangle( UEShapeBuffers& g, double width, double height, int widthVertexCount,
                                       int heightVertexCount, bool singlePolyGroup )
        {
            const int widthNV  = widthVertexCount > 1 ? widthVertexCount : 2;
            const int heightNV = heightVertexCount > 1 ? heightVertexCount : 2;
            const int total    = widthNV * heightNV;
            g.SetBufferSizes( total, 2 * ( widthNV - 1 ) * ( heightNV - 1 ), total, total );
            const glm::dvec3 v00( -width / 2.0, -height / 2.0, 0.0 );
            const glm::dvec3 v01( width / 2.0, -height / 2.0, 0.0 );
            const glm::dvec3 v11( width / 2.0, height / 2.0, 0.0 );
            const glm::dvec3 v10( -width / 2.0, height / 2.0, 0.0 );
            float uvRight = 1.0f, uvTop = 1.0f; // bScaleUVByAspectRatio
            if ( width != height )
            {
                if ( width > height )
                    uvTop = static_cast<float>( height / width );
                else
                    uvRight = static_cast<float>( width / height );
            }
            const glm::vec2 uv00( 0.0f, 0.0f ), uv01( uvRight, 0.0f ), uv11( uvRight, uvTop ), uv10( 0.0f, uvTop );
            int vi = 0;
            for ( int yi = 0; yi < heightNV; ++yi )
            {
                const double ty = static_cast<double>( yi ) / static_cast<double>( heightNV - 1 );
                for ( int xi = 0; xi < widthNV; ++xi )
                {
                    const double tx = static_cast<double>( xi ) / static_cast<double>( widthNV - 1 );
                    g.SetNormal( vi, glm::vec3( 0.0f, 0.0f, 1.0f ), vi );
                    g.SetUV( vi, Bilinear( uv00, uv01, uv11, uv10, static_cast<float>( tx ), static_cast<float>( ty ) ),
                             vi );
                    g.Vertices[static_cast<size_t>( vi++ )] = Bilinear( v00, v01, v11, v10, tx, ty );
                }
            }
            int ti = 0, polyIndex = 0;
            for ( int y0 = 0; y0 < heightNV - 1; ++y0 )
                for ( int x0 = 0; x0 < widthNV - 1; ++x0 )
                {
                    const int i00 = y0 * widthNV + x0;
                    const int i10 = ( y0 + 1 ) * widthNV + x0;
                    const int i01 = i00 + 1, i11 = i10 + 1;
                    g.SetTriangleWithMatchedUVNormal( ti, i00, i11, i01 );
                    g.SetTrianglePolygon( ti++, polyIndex );
                    g.SetTriangleWithMatchedUVNormal( ti, i00, i10, i11 );
                    g.SetTrianglePolygon( ti++, polyIndex );
                    if ( !singlePolyGroup )
                        ++polyIndex;
                }
        }

        // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Generators/RectangleMeshGenerator.cpp:
        // 112-309 (FRoundedRectangleMeshGenerator::Generate), adapted: SharpCorners = None (every corner
        // rounded), as UAddRectanglePrimitiveTool leaves it; Origin / Normal / IndicesMap as the rectangle's.
        inline void GenerateRoundedRectangle( UEShapeBuffers& g, double width, double height, double radius,
                                              int widthVertexCount, int heightVertexCount, int angleSamples,
                                              bool singlePolyGroup )
        {
            int       widthNV  = ( widthVertexCount > 1 ? widthVertexCount : 2 ) + 2;
            int       heightNV = ( heightVertexCount > 1 ? heightVertexCount : 2 ) + 2;
            const int roundNV  = angleSamples > 0 ? angleSamples : 1;
            const int numRound = 4;
            const int totalV   = widthNV * heightNV + numRound * ( roundNV - 1 );
            const int totalT   = 2 * ( widthNV - 1 ) * ( heightNV - 1 ) + numRound * ( roundNV - 1 );
            g.SetBufferSizes( totalV, totalT, totalV, totalV );
            for ( int i = 0; i < totalV; ++i )
            {
                g.SetNormal( i, glm::vec3( 0.0f, 0.0f, 1.0f ), i );
                g.UVParentVertex[static_cast<size_t>( i )] = i;
            }
            constexpr float kZeroTolerance = 1e-06f; // FMathf::ZeroTolerance
            const float     totWidth  = std::max( kZeroTolerance, static_cast<float>( radius * 2 + width ) );
            const float     totHeight = std::max( kZeroTolerance, static_cast<float>( radius * 2 + height ) );
            const glm::dvec3 v00( -totWidth / 2.0f, -totHeight / 2.0f, 0.0 );
            const glm::dvec3 v01( totWidth / 2.0f, -totHeight / 2.0f, 0.0 );
            const glm::dvec3 v11( totWidth / 2.0f, totHeight / 2.0f, 0.0 );
            const glm::dvec3 v10( -totWidth / 2.0f, totHeight / 2.0f, 0.0 );
            float uvRight = 1.0f, uvTop = 1.0f;
            if ( totWidth != totHeight )
            {
                if ( totWidth > totHeight )
                    uvTop = totHeight / totWidth;
                else
                    uvRight = totWidth / totHeight;
            }
            const glm::vec2 uv00( 0.0f, 0.0f ), uv01( uvRight, 0.0f ), uv11( uvRight, uvTop ), uv10( 0.0f, uvTop );
            std::vector<double> xFrac( static_cast<size_t>( widthNV ) ), yFrac( static_cast<size_t>( heightNV ) );
            xFrac.front() = 0.0;
            xFrac.back()  = 1.0;
            yFrac.front() = 0.0;
            yFrac.back()  = 1.0;
            for ( int i = 1; i + 1 < widthNV; ++i )
                xFrac[static_cast<size_t>( i )] = ( radius + width * ( i - 1 ) / ( widthNV - 3 ) ) / totWidth;
            for ( int i = 1; i + 1 < heightNV; ++i )
                yFrac[static_cast<size_t>( i )] = ( radius + height * ( i - 1 ) / ( heightNV - 3 ) ) / totHeight;
            int vi = 0;
            for ( int yi = 0; yi < heightNV; ++yi )
                for ( int xi = 0; xi < widthNV; ++xi )
                {
                    const double tx = xFrac[static_cast<size_t>( xi )];
                    const double ty = yFrac[static_cast<size_t>( yi )];
                    g.UVs[static_cast<size_t>( vi )] =
                         Bilinear( uv00, uv01, uv11, uv10, static_cast<float>( tx ), static_cast<float>( ty ) );
                    g.Vertices[static_cast<size_t>( vi++ )] = Bilinear( v00, v01, v11, v10, tx, ty );
                }
            int ti = 0, polyIndex = 0;
            for ( int y0 = 0; y0 < heightNV - 1; ++y0 )
            {
                const bool yEdge = y0 == 0 || y0 == heightNV - 2;
                for ( int x0 = 0; x0 < widthNV - 1; ++x0 )
                {
                    // A rounded corner's cell is left out here and fanned below.
                    if ( yEdge && ( x0 == 0 || x0 == widthNV - 2 ) )
                        continue;
                    const int i00 = y0 * widthNV + x0;
                    const int i10 = ( y0 + 1 ) * widthNV + x0;
                    const int i01 = i00 + 1, i11 = i10 + 1;
                    g.SetTriangleWithMatchedUVNormal( ti, i00, i11, i01 );
                    g.SetTrianglePolygon( ti++, polyIndex );
                    g.SetTriangleWithMatchedUVNormal( ti, i00, i10, i11 );
                    g.SetTrianglePolygon( ti++, polyIndex );
                    if ( !singlePolyGroup )
                        ++polyIndex;
                }
            }
            for ( int sideX = 0; sideX < 2; ++sideX )
                for ( int sideY = 0; sideY < 2; ++sideY )
                {
                    const int  cornerY      = sideY * ( heightNV - 1 );
                    const int  cornerX      = sideX * ( widthNV - 1 );
                    const int  inCornerY    = sideY ? heightNV - 2 : 1;
                    const int  inCornerX    = sideX ? widthNV - 2 : 1;
                    int        useVIdx      = cornerY * widthNV + cornerX; // the corner vertex is re-purposed
                    const int  vCenterIdx   = inCornerY * widthNV + inCornerX;
                    const int  offXIdx      = inCornerY * widthNV + cornerX;
                    const int  offYIdx      = cornerY * widthNV + inCornerX;
                    const int  actingCosIdx = sideY == sideX ? offYIdx : offXIdx;
                    const int  actingSinIdx = sideY == sideX ? offXIdx : offYIdx;
                    const auto V            = [&]( int i ) { return g.Vertices[static_cast<size_t>( i )]; };
                    const auto UV           = [&]( int i ) { return g.UVs[static_cast<size_t>( i )]; };
                    const glm::dvec3 centerV  = V( vCenterIdx );
                    const glm::dvec3 cosV     = V( actingCosIdx ) - centerV;
                    const glm::dvec3 sinV     = V( actingSinIdx ) - centerV;
                    const glm::vec2  centerUV = UV( vCenterIdx );
                    const glm::vec2  cosUV    = UV( actingCosIdx ) - centerUV;
                    const glm::vec2  sinUV    = UV( actingSinIdx ) - centerUV;
                    int              lastUsed = actingCosIdx;
                    for ( int k = 1; k < roundNV + 1; ++k )
                    {
                        const double angle = glm::half_pi<double>() * k / static_cast<float>( roundNV + 1 );
                        const double c     = std::cos( angle );
                        const double s     = std::sin( angle );
                        g.Vertices[static_cast<size_t>( useVIdx )] = centerV + c * cosV + s * sinV;
                        g.UVs[static_cast<size_t>( useVIdx )] =
                             centerUV + static_cast<float>( c ) * cosUV + static_cast<float>( s ) * sinUV;
                        g.SetTriangleWithMatchedUVNormal( ti, vCenterIdx, lastUsed, useVIdx );
                        g.SetTrianglePolygon( ti++, polyIndex );
                        lastUsed = useVIdx;
                        useVIdx  = vi++;
                    }
                    --vi;
                    g.SetTriangleWithMatchedUVNormal( ti, vCenterIdx, lastUsed, actingSinIdx );
                    g.SetTrianglePolygon( ti++, polyIndex );
                    if ( !singlePolyGroup )
                        ++polyIndex;
                }
        }

        // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Generators/DiscMeshGenerator.cpp:30-131
        // (FDiscMeshGenerator::Generate), adapted: StartAngle 0 / EndAngle 360 and Normal +Z, as
        // UAddDiscPrimitiveTool leaves them, so every disc is a full one.
        inline void GenerateDisc( UEShapeBuffers& g, double radius, int angleSamples, int radialSamples,
                                  bool singlePolygroup )
        {
            const int angleNV  = angleSamples > 3 ? angleSamples : 3;
            const int radialNV = radialSamples > 1 ? radialSamples : 1;
            const int numV     = angleNV * radialNV + 1;
            g.SetBufferSizes( numV, angleNV * ( 2 * radialNV - 1 ), numV, numV );
            g.Vertices[0] = glm::dvec3( 0.0 );
            g.UVs[0]      = glm::vec2( 0.5f, 0.5f );
            const double toRadians = glm::two_pi<double>() / static_cast<double>( angleNV );
            for ( int a = 0; a < angleNV; ++a )
            {
                const double angle = toRadians * a;
                const double cosA  = std::cos( angle );
                const double sinA  = std::sin( angle );
                for ( int r = 1; r <= radialNV; ++r )
                {
                    const int    vi = a + 1 + ( r - 1 ) * angleNV;
                    const double R  = r * radius / static_cast<double>( radialNV );
                    g.Vertices[static_cast<size_t>( vi )] = glm::dvec3( R * cosA, R * sinA, 0.0 );
                    g.UVs[static_cast<size_t>( vi )] =
                         glm::vec2( static_cast<float>( 0.5 + cosA * r * 0.5 / radialNV ),
                                    static_cast<float>( 0.5 + sinA * r * 0.5 / radialNV ) );
                }
            }
            for ( int i = 0; i < numV; ++i )
            {
                g.UVParentVertex[static_cast<size_t>( i )] = i;
                g.SetNormal( i, glm::vec3( 0.0f, 0.0f, 1.0f ), i );
            }
            int tri = 0, poly = 0;
            const auto next = [&]() {
                if ( !singlePolygroup )
                    ++poly;
            };
            for ( int a = 0; a + 1 < angleNV; ++a )
            {
                g.SetTriangleWithMatchedUVNormal( tri, 0, a + 2, a + 1 );
                g.SetTrianglePolygon( tri++, poly );
                next();
            }
            g.SetTriangleWithMatchedUVNormal( tri, 0, 1, angleNV );
            g.SetTrianglePolygon( tri++, poly );
            next();
            for ( int r = 0; r + 1 < radialNV; ++r )
            {
                const int inner = 1 + r * angleNV;
                const int outer = inner + angleNV;
                for ( int a = 0; a + 1 < angleNV; ++a )
                {
                    g.SetTriangleWithMatchedUVNormal( tri, inner + a, outer + a + 1, outer + a );
                    g.SetTrianglePolygon( tri++, poly );
                    g.SetTriangleWithMatchedUVNormal( tri, inner + a, inner + a + 1, outer + a + 1 );
                    g.SetTrianglePolygon( tri++, poly );
                    next();
                }
                g.SetTriangleWithMatchedUVNormal( tri, inner + angleNV - 1, outer, outer + angleNV - 1 );
                g.SetTrianglePolygon( tri++, poly );
                g.SetTriangleWithMatchedUVNormal( tri, inner + angleNV - 1, inner, outer );
                g.SetTrianglePolygon( tri++, poly );
                next();
            }
        }

        // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Generators/DiscMeshGenerator.cpp:
        // 135-211 (FPuncturedDiscMeshGenerator::Generate), adapted as GenerateDisc.
        inline void GeneratePuncturedDisc( UEShapeBuffers& g, double radius, double holeRadius, int angleSamples,
                                           int radialSamples, bool singlePolygroup )
        {
            const int angleNV  = angleSamples > 3 ? angleSamples : 3;
            const int radialNV = radialSamples > 2 ? radialSamples : 2;
            const int numV     = angleNV * radialNV;
            g.SetBufferSizes( numV, angleNV * 2 * ( radialNV - 1 ), numV, numV );
            const double toRadians   = glm::two_pi<double>() / static_cast<double>( angleNV );
            const double radiusScale = ( radius - holeRadius ) / static_cast<double>( radialNV - 1 );
            const double uvScale     = 0.5 / radius;
            for ( int a = 0; a < angleNV; ++a )
            {
                const double cosA = std::cos( toRadians * a );
                const double sinA = std::sin( toRadians * a );
                for ( int r = 0; r < radialNV; ++r )
                {
                    const int    vi = a + r * angleNV;
                    const double R  = r * radiusScale + holeRadius;
                    g.Vertices[static_cast<size_t>( vi )] = glm::dvec3( R * cosA, R * sinA, 0.0 );
                    g.UVs[static_cast<size_t>( vi )]      = glm::vec2( static_cast<float>( 0.5 + cosA * R * uvScale ),
                                                                  static_cast<float>( 0.5 + sinA * R * uvScale ) );
                }
            }
            for ( int i = 0; i < numV; ++i )
            {
                g.UVParentVertex[static_cast<size_t>( i )] = i;
                g.SetNormal( i, glm::vec3( 0.0f, 0.0f, 1.0f ), i );
            }
            int tri = 0, poly = 0;
            for ( int r = 0; r + 1 < radialNV; ++r )
            {
                const int inner = r * angleNV;
                const int outer = inner + angleNV;
                for ( int a = 0; a + 1 < angleNV; ++a )
                {
                    g.SetTriangleWithMatchedUVNormal( tri, inner + a, outer + a + 1, outer + a );
                    g.SetTrianglePolygon( tri++, poly );
                    g.SetTriangleWithMatchedUVNormal( tri, inner + a, inner + a + 1, outer + a + 1 );
                    g.SetTrianglePolygon( tri++, poly );
                    if ( !singlePolygroup )
                        ++poly;
                }
                g.SetTriangleWithMatchedUVNormal( tri, inner + angleNV - 1, outer, outer + angleNV - 1 );
                g.SetTrianglePolygon( tri++, poly );
                g.SetTriangleWithMatchedUVNormal( tri, inner + angleNV - 1, inner, outer );
                g.SetTrianglePolygon( tri++, poly );
                if ( !singlePolygroup )
                    ++poly;
            }
        }

        inline glm::dvec3 SphericalToCartesian( double r, double theta, double phi )
        {
            return glm::dvec3( r * std::cos( theta ) * std::sin( phi ), r * std::sin( theta ) * std::sin( phi ),
                               r * std::cos( phi ) );
        }

        // The ring-and-poles triangulation FSphereGenerator and FCapsuleGenerator share, line for line:
        // `rings` rings of `numTheta` vertices from the north pole down, then the two poles; UVs as UE lays
        // them out (numTheta + 1 per ring with a wrap column, then numTheta per pole). Ported from UE 5.8
        // Engine/Source/Runtime/GeometryCore/Public/Generators/SphereGenerator.h:110-197
        // (OutputEquatorialTriangles / OutputPolarTriangles; CapsuleGenerator.h:185-262 is the same code).
        inline void OutputRingTriangles( UEShapeBuffers& g, int rings, int numTheta, bool perQuad )
        {
            const auto out = [&]( int tri, int poly, glm::ivec3 c, glm::ivec3 uv )
            {
                g.SetTriangle( tri, c.x, c.y, c.z );
                g.SetTrianglePolygon( tri, poly );
                g.SetTriangleUVs( tri, uv.x, uv.y, uv.z );
                g.SetTriangleNormals( tri, c.x, c.y, c.z );
            };
            const int numPhi = rings + 2;
            int       tri = 0, poly = 0;
            int       corners[4]   = { 0, 1, numTheta + 1, numTheta };
            int       uvCorners[4] = { 0, 1, numTheta + 2, numTheta + 1 };
            for ( int p = 1; p < numPhi - 2; ++p )
            {
                for ( int t = 0; t < numTheta - 1; ++t )
                {
                    out( tri++, poly, { corners[0], corners[1], corners[2] }, { uvCorners[0], uvCorners[1], uvCorners[2] } );
                    out( tri++, poly, { corners[2], corners[3], corners[0] }, { uvCorners[2], uvCorners[3], uvCorners[0] } );
                    for ( int& i : corners )
                        ++i;
                    for ( int& i : uvCorners )
                        ++i;
                    if ( perQuad )
                        ++poly;
                }
                out( tri++, poly, { corners[0], corners[1] - numTheta, corners[2] - numTheta },
                     { uvCorners[0], uvCorners[1], uvCorners[2] } );
                out( tri++, poly, { corners[2] - numTheta, corners[3], corners[0] },
                     { uvCorners[2], uvCorners[3], uvCorners[0] } );
                for ( int& i : corners )
                    ++i;
                for ( int& i : uvCorners )
                    i += 2;
                if ( perQuad )
                    ++poly;
            }
            const int numEquatorialVtx   = rings * numTheta;
            const int numEquatorialUVVtx = rings * ( numTheta + 1 );
            const int north              = numEquatorialVtx;
            const int south              = numEquatorialVtx + 1;
            poly                         = perQuad ? numTheta * ( numPhi - 3 ) : 0;
            tri                          = numTheta * ( numPhi - 3 ) * 2;
            for ( int t = 0; t < numTheta; ++t )
            {
                out( tri++, poly, { t, north, ( t + 1 ) % numTheta }, { t, numEquatorialUVVtx + t, t + 1 } );
                if ( perQuad )
                    ++poly;
            }
            const int offset   = numEquatorialVtx - numTheta;
            const int offsetUV = numEquatorialUVVtx - ( numTheta + 1 );
            for ( int t = 0; t < numTheta; ++t )
            {
                out( tri++, poly, { t + offset, ( ( t + 1 ) % numTheta ) + offset, south },
                     { t + offsetUV, t + 1 + offsetUV, numEquatorialUVVtx + numTheta + t } );
                if ( perQuad )
                    ++poly;
            }
        }

        // UE's pole UVs: numTheta per pole, V = 0 at the north pole and 1 at the south one.
        inline void AddPoleUVs( UEShapeBuffers& g, int& uvIdx, int numTheta, int north )
        {
            const float dTheta = -1.0f / static_cast<float>( numTheta );
            for ( int pole = 0; pole < 2; ++pole )
            {
                float uvTheta = 1.0f + dTheta;
                for ( int t = 0; t < numTheta; ++t, ++uvIdx, uvTheta += dTheta )
                    g.SetUV( uvIdx, glm::vec2( uvTheta, static_cast<float>( pole ) ), north + pole );
            }
        }

        // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Generators/SphereGenerator.h:20-218
        // (FSphereGenerator), adapted: the triangulation is OutputRingTriangles.
        inline void GenerateLatLongSphere( UEShapeBuffers& g, double radius, int numPhi, int numTheta, bool perQuad )
        {
            numPhi   = std::max( numPhi, 3 );
            numTheta = std::max( numTheta, 3 );
            const int numV = ( numPhi - 2 ) * numTheta + 2;
            g.SetBufferSizes( numV, ( numPhi - 2 ) * numTheta * 2, ( numPhi - 2 ) * ( numTheta + 1 ) + 2 * numTheta,
                              numV );
            const double dPhi   = glm::pi<double>() / static_cast<double>( numPhi - 1 );
            const double dTheta = glm::two_pi<double>() / static_cast<double>( numTheta );
            int          vi     = 0;
            for ( int p = 1; p < numPhi - 1; ++p )
                for ( int t = 0; t < numTheta; ++t, ++vi )
                {
                    const glm::dvec3 n = SphericalToCartesian( 1.0, t * dTheta, p * dPhi );
                    g.Vertices[static_cast<size_t>( vi )] = n * radius;
                    g.SetNormal( vi, glm::vec3( n ), vi );
                }
            g.Vertices[static_cast<size_t>( vi )] = glm::dvec3( 0.0, 0.0, radius );
            g.SetNormal( vi, glm::vec3( 0.0f, 0.0f, 1.0f ), vi );
            ++vi;
            g.Vertices[static_cast<size_t>( vi )] = glm::dvec3( 0.0, 0.0, -radius );
            g.SetNormal( vi, glm::vec3( 0.0f, 0.0f, -1.0f ), vi );

            const float duvPhi   = 1.0f / static_cast<float>( numPhi - 1 );
            const float duvTheta = -1.0f / static_cast<float>( numTheta );
            int         uvIdx    = 0;
            float       uvPhi    = duvPhi;
            for ( int p = 1; p < numPhi - 1; ++p, uvPhi += duvPhi )
            {
                float uvTheta = 1.0f;
                for ( int t = 0; t < numTheta; ++t, ++uvIdx, uvTheta += duvTheta )
                    g.SetUV( uvIdx, glm::vec2( uvTheta, uvPhi ), ( p - 1 ) * numTheta + t );
                g.SetUV( uvIdx++, glm::vec2( uvTheta, uvPhi ), ( p - 1 ) * numTheta ); // wrap around
            }
            AddPoleUVs( g, uvIdx, numTheta, ( numPhi - 2 ) * numTheta );
            OutputRingTriangles( g, numPhi - 2, numTheta, perQuad );
        }

        // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Generators/BoxSphereGenerator.h:15-79
        // (FBoxSphereGenerator, CubeMapping projection), adapted: the normal is the projected unit vector of
        // the vertex itself (UE reads it through NormalParentVertex, the same point).
        inline void GenerateBoxSphere( UEShapeBuffers& g, double radius, int subdivisions, bool perQuad )
        {
            const double e = 0.5 * ( subdivisions + 1 );
            GenerateGridBox( g, glm::dvec3( e ), glm::ivec3( subdivisions + 1 ), perQuad );
            for ( size_t i = 0; i < g.Vertices.size(); ++i )
            {
                const glm::dvec3 q = g.Vertices[i] / e;
                const glm::dvec3 q2 = q * q;
                glm::dvec3       s( q.x * std::sqrt( 1.0 - q2.y * 0.5 - q2.z * 0.5 + q2.y * q2.z / 3.0 ),
                                    q.y * std::sqrt( 1.0 - q2.x * 0.5 - q2.z * 0.5 + q2.x * q2.z / 3.0 ),
                                    q.z * std::sqrt( 1.0 - q2.x * 0.5 - q2.y * 0.5 + q2.x * q2.y / 3.0 ) );
                s                = glm::normalize( s );
                g.Normals[i]     = glm::vec3( s );
                g.Vertices[i]    = radius * s;
            }
        }

        // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/Generators/CapsuleGenerator.h:18-283
        // (FCapsuleGenerator), adapted: the triangulation is OutputRingTriangles (UE's own copy of it).
        inline void GenerateCapsule( UEShapeBuffers& g, double radius, double segmentLength, int arcSteps,
                                     int circleSteps, int segmentSteps, bool perQuad )
        {
            arcSteps          = std::max( arcSteps, 2 );
            circleSteps       = std::max( circleSteps, 3 );
            const int rings   = 2 * arcSteps - 2 + segmentSteps;
            const int numV    = rings * circleSteps + 2;
            g.SetBufferSizes( numV, rings * circleSteps * 2, rings * ( circleSteps + 1 ) + 2 * circleSteps, numV );
            const double     dPhi   = glm::half_pi<double>() / static_cast<double>( arcSteps - 1 );
            const double     dTheta = glm::two_pi<double>() / static_cast<double>( circleSteps );
            const glm::dvec3 offset( 0.0, 0.0, segmentLength );
            int              vi = 0;
            const auto       set = [&]( const glm::dvec3& p, const glm::dvec3& n )
            {
                g.Vertices[static_cast<size_t>( vi )] = p;
                g.SetNormal( vi, glm::vec3( n ), vi );
                ++vi;
            };
            for ( int p = 1; p < arcSteps; ++p )
                for ( int t = 0; t < circleSteps; ++t )
                {
                    const glm::dvec3 n = SphericalToCartesian( 1.0, t * dTheta, p * dPhi );
                    set( n * radius + offset, n );
                }
            const double segStep = 1.0 / ( segmentSteps + 1.0 );
            double       along   = segStep;
            for ( int k = 0; k < segmentSteps; ++k, along += segStep )
                for ( int t = 0; t < circleSteps; ++t )
                {
                    const glm::dvec3 n( std::cos( t * dTheta ), std::sin( t * dTheta ), 0.0 );
                    set( n * radius + offset * ( 1.0 - along ), n );
                }
            for ( int p = 1; p < arcSteps; ++p )
                for ( int t = 0; t < circleSteps; ++t )
                {
                    const glm::dvec3 n = SphericalToCartesian( 1.0, t * dTheta, glm::half_pi<double>() + ( p - 1 ) * dPhi );
                    set( n * radius, n );
                }
            set( glm::dvec3( 0.0, 0.0, radius ) + offset, glm::dvec3( 0.0, 0.0, 1.0 ) );
            set( glm::dvec3( 0.0, 0.0, -radius ), glm::dvec3( 0.0, 0.0, -1.0 ) );

            const float duvTheta = -1.0f / static_cast<float>( circleSteps );
            int         uvIdx    = 0;
            const auto  span     = [&]( int stepStart, int numSteps, float phiStart, float phiStep )
            {
                float uvPhi = phiStart;
                int   p     = stepStart;
                for ( ; p < stepStart + numSteps; ++p, uvPhi += phiStep )
                {
                    float uvTheta = 1.0f;
                    for ( int t = 0; t < circleSteps; ++t, ++uvIdx, uvTheta += duvTheta )
                        g.SetUV( uvIdx, glm::vec2( uvTheta, uvPhi ), ( p - 1 ) * circleSteps + t );
                    g.SetUV( uvIdx++, glm::vec2( uvTheta, uvPhi ), ( p - 1 ) * circleSteps ); // wrap around
                }
                return p;
            };
            // UE's AddUVSpan parents to PIdx * NumCircleSteps with PIdx starting at 1; the vertex rows start
            // at 0, so the parent is the row (PIdx - 1) - the vertex the triangles actually pair it with.
            const float phiSpan      = static_cast<float>( 2 * radius + segmentLength );
            const float hemiStep     = static_cast<float>( radius ) / ( phiSpan * static_cast<float>( arcSteps - 1 ) );
            int         p            = span( 1, arcSteps - 1, hemiStep, hemiStep );
            const float segmentStep  = static_cast<float>( segmentLength ) / ( phiSpan * static_cast<float>( segmentSteps + 1 ) );
            p = span( p, segmentSteps, static_cast<float>( radius ) / phiSpan + segmentStep, segmentStep );
            span( p, arcSteps - 1, static_cast<float>( radius + segmentLength ) / phiSpan, hemiStep );
            AddPoleUVs( g, uvIdx, circleSteps, rings * circleSteps );
            OutputRingTriangles( g, rings, circleSteps, perQuad );
        }

        // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Generators/SweepGenerator.cpp:13-379
        // (FSweepGeneratorBase::ConstructMeshTopology), adapted: only what UAddPrimitiveTool's sweeps reach -
        // a closed cross section, no UV or normal sections, no custom texture coordinates, and caps None or
        // FlatMidpointFan (FlatTriangulation is a generalized cylinder's default that no primitive uses).
        struct SweepTopology
        {
            std::array<int, 2> CapVertStart{}, CapNormalStart{}, CapUVStart{};
        };

        inline SweepTopology ConstructMeshTopology( UEShapeBuffers& g, const std::vector<glm::dvec2>& crossSection,
                                                    const std::vector<int>& sharpNormalsAlongLength,
                                                    bool evenlySpaceUVs, const std::vector<glm::dvec3>& path,
                                                    int numCrossSections, bool loop, bool capped,
                                                    const glm::vec2& sectionsUVScale, const glm::vec2& capUVScale,
                                                    const glm::vec2& capUVOffset, bool polygroupPerQuad )
        {
            SweepTopology top;
            const int     xVerts    = static_cast<int>( crossSection.size() );
            const int     xSegments = xVerts;
            const int     xNormals  = xVerts;
            const int     xUVs      = xSegments + 1;
            std::vector<float> crossTex, pathTex;
            if ( evenlySpaceUVs )
            {
                float perimeter = 0.0f;
                crossTex.push_back( 0.0f );
                for ( int i = 0; i < xSegments; ++i )
                {
                    perimeter += static_cast<float>(
                         glm::distance( crossSection[static_cast<size_t>( i )],
                                        crossSection[static_cast<size_t>( ( i + 1 ) % xSegments )] ) );
                    crossTex.push_back( perimeter );
                }
                perimeter = std::max( perimeter, 1e-06f );
                for ( float& u : crossTex )
                    u = 1.0f - u / perimeter;
                float length = 0.0f;
                pathTex.push_back( 0.0f );
                const int pathSegs = loop ? static_cast<int>( path.size() ) : static_cast<int>( path.size() ) - 1;
                for ( int i = 0; i < pathSegs; ++i )
                {
                    length += static_cast<float>( glm::distance( path[static_cast<size_t>( i )],
                                                                 path[( static_cast<size_t>( i ) + 1 ) % path.size()] ) );
                    pathTex.push_back( length );
                }
                length = std::max( length, 1e-06f );
                for ( float& v : pathTex )
                    v = 1.0f - v / length;
            }
            else
            {
                for ( int i = 0; i < xSegments; ++i )
                    crossTex.push_back( 1.0f - static_cast<float>( i ) / static_cast<float>( xSegments ) );
                crossTex.push_back( 0.0f );
                for ( int i = 0; i < numCrossSections; ++i )
                    pathTex.push_back( 1.0f - static_cast<float>( i ) / static_cast<float>( numCrossSections - 1 ) );
            }

            const int sharpCount  = static_cast<int>( sharpNormalsAlongLength.size() );
            int       numVerts    = xVerts * numCrossSections - ( loop ? xVerts : 0 );
            int       numNormals  = numCrossSections > 1 ? xNormals * numCrossSections - ( loop ? xNormals : 0 ) : 0;
            numNormals           += xNormals * sharpCount;
            int       numUVs      = numCrossSections > 1 ? xUVs * numCrossSections : 0;
            int       numPolygons = ( numCrossSections - 1 ) * xSegments;
            int       numTris     = numPolygons * 2;
            std::array<int, 2> capTriStart{}, capPolyStart{};
            if ( capped )
                for ( int cap = 0; cap < 2; ++cap ) // FlatMidpointFan
                {
                    top.CapVertStart[cap]   = numVerts;
                    top.CapNormalStart[cap] = numNormals;
                    top.CapUVStart[cap]     = numUVs;
                    capTriStart[cap]        = numTris;
                    capPolyStart[cap]       = numPolygons;
                    numTris += xSegments;
                    numPolygons++;
                    numUVs += xVerts + 1;
                    numNormals += xVerts + 1;
                    numVerts += 1;
                }
            g.SetBufferSizes( numVerts, numTris, numUVs, numNormals );
            if ( capped )
                for ( int cap = 0; cap < 2; ++cap )
                {
                    const int  vertOffset = cap * ( xVerts * ( numCrossSections - 1 ) );
                    const bool flipped    = cap == 0;
                    int        tri        = capTriStart[cap];
                    for ( int i = 0; i < xSegments; ++i, ++tri )
                    {
                        g.SetTriangle( tri, vertOffset + i, top.CapVertStart[cap], vertOffset + ( i + 1 ) % xVerts,
                                       flipped );
                        g.SetTriangleUVs( tri, top.CapUVStart[cap] + i, top.CapUVStart[cap] + xVerts,
                                          top.CapUVStart[cap] + ( i + 1 ) % xVerts, flipped );
                        g.SetTriangleNormals( tri, top.CapNormalStart[cap] + i, top.CapNormalStart[cap] + xVerts,
                                              top.CapNormalStart[cap] + ( i + 1 ) % xVerts, flipped );
                        g.SetTrianglePolygon( tri, capPolyStart[cap] );
                    }
                    g.SetUV( top.CapUVStart[cap] + xVerts, capUVOffset, top.CapVertStart[cap] );
                    g.SetNormal( top.CapNormalStart[cap] + xVerts, glm::vec3( 0.0f ), top.CapVertStart[cap] );
                    for ( int i = 0; i < xVerts; ++i )
                    {
                        g.SetUV( top.CapUVStart[cap] + i,
                                 glm::vec2( crossSection[static_cast<size_t>( i )] ) * capUVScale + capUVOffset,
                                 vertOffset + i );
                        g.SetNormal( top.CapNormalStart[cap] + i, glm::vec3( 0.0f ), vertOffset + i );
                    }
                }

            const int curFaceGroupIndex = numPolygons;
            if ( numCrossSections < ( loop ? 3 : 2 ) )
                return top;
            const int sectionsMod       = numCrossSections - ( loop ? 1 : 0 ); // a loop's last section is its first
            const int normalSectionsMod = sectionsMod + sharpCount;
            for ( int s = 0; s < xSegments; ++s )
            {
                for ( int x = 0; x < numCrossSections; ++x )
                    g.SetUV( x * xUVs + s,
                             glm::vec2( crossTex[static_cast<size_t>( s )], pathTex[static_cast<size_t>( x )] ) *
                                  sectionsUVScale,
                             ( x % sectionsMod ) * xVerts + s );
                for ( int x = 0; x + 1 < numCrossSections; ++x )
                {
                    g.SetTriangleUVs( xSegments * 2 * x + 2 * s, x * xUVs + s, x * xUVs + s + 1, ( x + 1 ) * xUVs + s,
                                      true );
                    g.SetTriangleUVs( xSegments * 2 * x + 2 * s + 1, ( x + 1 ) * xUVs + s + 1, ( x + 1 ) * xUVs + s,
                                      x * xUVs + s + 1, true );
                }
            }
            for ( int x = 0; x < numCrossSections; ++x ) // the final UV column closes the profile
                g.SetUV( x * xUVs + xSegments,
                         glm::vec2( crossTex.back(), pathTex[static_cast<size_t>( x )] ) * sectionsUVScale,
                         ( x % sectionsMod ) * xVerts );
            for ( int s = 0; s < xVerts; ++s )
            {
                int sharp = 0;
                for ( int x = 0, nx = 0; x < numCrossSections; ++x, ++nx )
                {
                    g.SetNormal( ( nx % normalSectionsMod ) * xNormals + s, glm::vec3( 0.0f ), ( x % sectionsMod ) * xVerts + s );
                    // a sharp cross section carries two normals, one per side
                    if ( sharp < sharpCount && x == sharpNormalsAlongLength[static_cast<size_t>( sharp )] )
                    {
                        ++nx;
                        g.SetNormal( ( nx % normalSectionsMod ) * xNormals + s, glm::vec3( 0.0f ),
                                     ( x % sectionsMod ) * xVerts + s );
                        ++sharp;
                    }
                }
                const int nextN = ( s + 1 ) % xNormals;
                const int nextV = ( s + 1 ) % xVerts;
                sharp           = 0;
                for ( int x = 0, nx = 0; x + 1 < numCrossSections; ++x, ++nx )
                {
                    const int t0    = xSegments * 2 * x + 2 * s;
                    const int t1    = t0 + 1;
                    const int group = polygroupPerQuad ? xSegments * x + s : curFaceGroupIndex + x;
                    const int nextX = ( x + 1 ) % sectionsMod;
                    const int nextNX = ( nx + 1 ) % normalSectionsMod;
                    g.SetTrianglePolygon( t0, group );
                    g.SetTrianglePolygon( t1, group );
                    g.SetTriangle( t0, x * xVerts + s, x * xVerts + nextV, nextX * xVerts + s, true );
                    g.SetTriangle( t1, nextX * xVerts + nextV, nextX * xVerts + s, x * xVerts + nextV, true );
                    g.SetTriangleNormals( t0, nx * xNormals + s, nx * xNormals + nextN, nextNX * xNormals + s, true );
                    g.SetTriangleNormals( t1, nextNX * xNormals + nextN, nextNX * xNormals + s, nx * xNormals + nextN,
                                          true );
                    if ( sharp < sharpCount && x + 1 == sharpNormalsAlongLength[static_cast<size_t>( sharp )] )
                    {
                        ++nx;
                        ++sharp;
                    }
                }
            }
            return top;
        }

        // UE's FPolygon2d::MakeCircle: `steps` points counter-clockwise from +X.
        inline std::vector<glm::dvec2> MakeCircle( double radius, int steps )
        {
            std::vector<glm::dvec2> circle( static_cast<size_t>( steps ) );
            for ( int i = 0; i < steps; ++i )
            {
                const double a = glm::two_pi<double>() * i / static_cast<double>( steps );
                circle[static_cast<size_t>( i )] = glm::dvec2( std::cos( a ), std::sin( a ) ) * radius;
            }
            return circle;
        }

        // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Generators/SweepGenerator.cpp:381-534
        // (FVerticalCylinderGeneratorBase::ComputeSegLengths / GenerateVerticalCircleSweep), adapted: always
        // capped with FlatMidpointFan and bUVScaleMatchSidesAndCaps, as FCylinderGenerator and FArrowGenerator
        // are used by the Add Primitive tools.
        inline void GenerateVerticalCircleSweep( UEShapeBuffers& g, const std::vector<float>& radii,
                                                 const std::vector<float>& heights,
                                                 const std::vector<int>& sharpNormalsAlongLength, int angleSamples,
                                                 bool polygroupPerQuad )
        {
            const std::vector<glm::dvec2> x    = MakeCircle( 1.0, angleSamples );
            const int                     numX = static_cast<int>( radii.size() );
            float                         lenAlong = 0.0f;
            for ( int i = 0; i + 1 < numX; ++i )
                lenAlong += static_cast<float>( glm::distance( glm::dvec2( radii[static_cast<size_t>( i )], heights[static_cast<size_t>( i )] ),
                                                               glm::dvec2( radii[static_cast<size_t>( i ) + 1], heights[static_cast<size_t>( i ) + 1] ) ) );
            const SweepTopology top =
                 ConstructMeshTopology( g, x, sharpNormalsAlongLength, false, {}, numX, false, true, glm::vec2( 1.0f ),
                                        glm::vec2( 0.5f ), glm::vec2( 0.5f ), polygroupPerQuad );
            // PerpCW of each profile segment: the outward normal in (radial, up).
            std::vector<glm::dvec2> sides( static_cast<size_t>( numX - 1 ) ), smoothed( static_cast<size_t>( numX ) );
            for ( int i = 0; i + 1 < numX; ++i )
            {
                const glm::dvec2 v( radii[static_cast<size_t>( i ) + 1] - radii[static_cast<size_t>( i )],
                                    heights[static_cast<size_t>( i ) + 1] - heights[static_cast<size_t>( i )] );
                sides[static_cast<size_t>( i )] = glm::normalize( glm::dvec2( v.y, -v.x ) );
            }
            smoothed.front() = sides.front();
            smoothed.back()  = sides.back();
            for ( int i = 1; i + 1 < numX; ++i )
                smoothed[static_cast<size_t>( i )] =
                     glm::normalize( sides[static_cast<size_t>( i )] + sides[static_cast<size_t>( i ) - 1] );
            const auto radial = []( const glm::dvec2& dir, const glm::dvec2& n )
            { return glm::vec3( static_cast<float>( dir.x * n.x ), static_cast<float>( dir.y * n.x ), static_cast<float>( n.y ) ); };
            for ( int sub = 0; sub < angleSamples; ++sub )
            {
                const glm::dvec2 dir   = x[static_cast<size_t>( sub )];
                int              sharp = 0;
                for ( int i = 0, ni = 0; i < numX; ++i, ++ni )
                {
                    const double r = radii[static_cast<size_t>( i )];
                    g.Vertices[static_cast<size_t>( sub + i * angleSamples )] =
                         glm::dvec3( dir.x * r, dir.y * r, heights[static_cast<size_t>( i )] );
                    if ( sharp < static_cast<int>( sharpNormalsAlongLength.size() ) &&
                         i == sharpNormalsAlongLength[static_cast<size_t>( sharp )] )
                    {
                        g.Normals[static_cast<size_t>( sub + ni * angleSamples )] = radial( dir, sides[static_cast<size_t>( i ) - 1] );
                        ++ni;
                        g.Normals[static_cast<size_t>( sub + ni * angleSamples )] = radial( dir, sides[static_cast<size_t>( i )] );
                        ++sharp;
                    }
                    else
                        g.Normals[static_cast<size_t>( sub + ni * angleSamples )] = radial( dir, smoothed[static_cast<size_t>( i )] );
                }
            }
            for ( int cap = 0; cap < 2; ++cap )
            {
                const float up = static_cast<float>( 2 * cap - 1 );
                g.Vertices[static_cast<size_t>( top.CapVertStart[cap] )] =
                     glm::dvec3( 0.0, 0.0, heights[static_cast<size_t>( cap * ( numX - 1 ) )] );
                for ( int sub = 0; sub <= angleSamples; ++sub ) // the ring and the midpoint
                    g.Normals[static_cast<size_t>( top.CapNormalStart[cap] + sub )] = glm::vec3( 0.0f, 0.0f, up );
            }
            // bUVScaleMatchSidesAndCaps: sides and caps in one texel density.
            float maxAbsRad = 0.0f;
            for ( const float r : radii )
                maxAbsRad = std::max( maxAbsRad, std::abs( r ) );
            float thetaScale  = maxAbsRad * glm::two_pi<float>();
            float heightScale = lenAlong;
            float capScale    = maxAbsRad * 2.0f;
            const float maxScale = std::max( { thetaScale, heightScale, capScale } );
            thetaScale /= maxScale;
            heightScale /= maxScale;
            capScale /= maxScale;
            for ( int i = 0; i < top.CapUVStart[0]; ++i )
            {
                g.UVs[static_cast<size_t>( i )].x *= thetaScale;
                g.UVs[static_cast<size_t>( i )].y *= heightScale;
            }
            for ( size_t i = static_cast<size_t>( top.CapUVStart[0] ); i < g.UVs.size(); ++i )
                g.UVs[i] *= capScale;
        }

        // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Generators/SweepGenerator.cpp:603-734
        // (FGeneralizedCylinderGenerator::Generate) as UAddTorusPrimitiveTool::GenerateMesh drives it
        // (AddPrimitiveTool.cpp:668-690: a circle swept along a circle, looped, uncapped, InitialFrame at
        // Path[0], MiterLimit 1, no scaling). Adapted: the frames are written in closed form. On a planar
        // circle the frame UE carries along with FFrame3d::AlignAxis(2, tangent) starts as identity rotated
        // Z -> +Y (so X = +X, Y = -Z) and then only turns about the circle's axis: X stays radial and Y stays
        // -Z. The face-averaged normals of a regular polygon are its radial directions.
        inline void GenerateTorus( UEShapeBuffers& g, double majorRadius, double minorRadius, int majorSlices,
                                   int minorSlices, bool polygroupPerQuad )
        {
            const std::vector<glm::dvec2> cross = MakeCircle( minorRadius, minorSlices );
            const std::vector<glm::dvec2> ring  = MakeCircle( majorRadius, majorSlices );
            std::vector<glm::dvec3>       path;
            for ( const glm::dvec2& p : ring )
                path.emplace_back( p.x, p.y, 0.0 );
            ConstructMeshTopology( g, cross, {}, true, path, majorSlices + 1, true, false, glm::vec2( 1.0f ),
                                   glm::vec2( 1.0f ), glm::vec2( 0.0f ), polygroupPerQuad );
            for ( int p = 0; p < majorSlices; ++p )
            {
                const glm::dvec3 radial = glm::dvec3( ring[static_cast<size_t>( p )] / majorRadius, 0.0 );
                const glm::dvec3 y( 0.0, 0.0, -1.0 );
                for ( int s = 0; s < minorSlices; ++s )
                {
                    const glm::dvec2 xp = cross[static_cast<size_t>( s )];
                    const glm::dvec2 xn = xp / minorRadius;
                    const size_t     i  = static_cast<size_t>( s + p * minorSlices );
                    g.Vertices[i]       = path[static_cast<size_t>( p )] + radial * xp.x + y * xp.y;
                    g.Normals[i]        = glm::vec3( radial * xn.x + y * xn.y );
                }
            }
        }

        inline ShapeMesh Finish( const UEShapeBuffers& g, const ShapeOptions& options )
        {
            ShapeMesh m = ToShapeMesh( g, options.Groups );
            ApplyPivot( m, options.Pivot );
            return m;
        }
    } // namespace Detail

    // Box: UAddBoxPrimitiveTool::GenerateMesh (AddPrimitiveTool.cpp:521-533). `size` is (Width X, Height Y,
    // Depth Z) and `subdivisions` the quads along each. Polygroups: 6 per face, one per quad.
    inline ShapeMesh MakeBox( const glm::vec3& size, const glm::ivec3& subdivisions = glm::ivec3( 1 ),
                              const ShapeOptions& options = {} )
    {
        Detail::UEShapeBuffers g;
        // UE: Box extents 0.5 * (Depth, Width, Height) on (X, Y, Z)_UE.
        const glm::dvec3 extents( 0.5 * Detail::Extent( size.z ), 0.5 * Detail::Extent( size.x ),
                                  0.5 * Detail::Extent( size.y ) );
        const glm::ivec3 edges( Detail::Count( subdivisions.z, 1 ) + 1, Detail::Count( subdivisions.x, 1 ) + 1,
                                Detail::Count( subdivisions.y, 1 ) + 1 );
        Detail::GenerateGridBox( g, extents, edges, options.Groups == ShapePolygroupMode::PerQuad );
        return Detail::Finish( g, options );
    }

    // Rectangle, lying on Y = 0 and facing +Y: UAddRectanglePrimitiveTool::GenerateMesh (AddPrimitiveTool.cpp:
    // 543-596), both types. Open. One polygroup unless PerQuad (UE: bSinglePolyGroup = mode != PerQuad).
    inline ShapeMesh MakeRectangle( const RectangleShape& rect, const ShapeOptions& options = {} )
    {
        Detail::UEShapeBuffers g;
        const bool   single = options.Groups != ShapePolygroupMode::PerQuad;
        // UE: RectGen.Width = Depth (along X_UE), RectGen.Height = Width (along Y_UE).
        double       width  = Detail::Extent( rect.Depth );
        double       height = Detail::Extent( rect.Width );
        const int    wVerts = Detail::Count( rect.DepthSubdivisions, 1 ) + 1;
        const int    hVerts = Detail::Count( rect.WidthSubdivisions, 1 ) + 1;
        if ( rect.Type == RectangleType::Rectangle )
            Detail::GenerateRectangle( g, width, height, wVerts, hVerts, single );
        else
        {
            double radius = std::max( static_cast<double>( rect.CornerRadius ), 0.0 );
            if ( rect.MaintainDimension )
            {
                // UE keeps the outer size: the radius comes out of Width / Depth, and it is clamped so the
                // straight part never vanishes (MinSide = UE_DOUBLE_KINDA_SMALL_NUMBER = 1e-4).
                constexpr double kMinSide = 1e-4;
                radius = std::clamp( radius, kMinSide * 0.5, ( std::min( width, height ) - kMinSide ) * 0.5 );
                width  = std::max( kMinSide, width - radius * 2 );
                height = std::max( kMinSide, height - radius * 2 );
            }
            Detail::GenerateRoundedRectangle( g, width, height, radius, wVerts, hVerts,
                                              Detail::Count( rect.CornerSlices, 3 ) - 1, single );
        }
        return Detail::Finish( g, options );
    }

    // A flat card in the XY plane facing +Z (the scene's `Plane` primitive), subdivided nx x ny. UE has no
    // such primitive: it is the ported rectangle (size.x wide, size.y tall) stood up by +90 degrees about X.
    inline ShapeMesh MakePlane( const glm::vec2& size, const glm::ivec2& subdivisions = glm::ivec2( 1 ),
                                const ShapeOptions& options = {} )
    {
        RectangleShape rect;
        rect.Width             = size.x;
        rect.Depth             = size.y;
        rect.WidthSubdivisions = subdivisions.x;
        rect.DepthSubdivisions = subdivisions.y;
        ShapeMesh  m           = MakeRectangle( rect, { options.Groups, ShapePivot::Base } );
        const auto stand       = []( const glm::vec3& v ) { return glm::vec3( v.x, -v.z, v.y ); };
        for ( Vertex& v : m.Vertices )
        {
            v.Position  = stand( v.Position );
            v.Normal    = stand( v.Normal );
            v.Tangent   = stand( v.Tangent );
            v.Bitangent = stand( v.Bitangent );
        }
        Detail::ApplyPivot( m, options.Pivot );
        return m;
    }

    // Disc, lying on Y = 0 and facing +Y: UAddDiscPrimitiveTool::GenerateMesh (AddPrimitiveTool.cpp:628-660).
    // The hole stays inside the rim (UE: HoleRadius <= 0.999 Radius). Open. One polygroup unless PerQuad.
    inline ShapeMesh MakeDisc( const DiscShape& disc, const ShapeOptions& options = {} )
    {
        Detail::UEShapeBuffers g;
        const bool   single = options.Groups != ShapePolygroupMode::PerQuad;
        const double radius = Detail::Extent( disc.Radius );
        const int    slices = Detail::Count( disc.RadialSlices, 3 );
        const int    rings  = Detail::Count( disc.RadialSubdivisions, 1 );
        if ( disc.Type == DiscType::Disc )
            Detail::GenerateDisc( g, radius, slices, rings, single );
        else
            Detail::GeneratePuncturedDisc( g, radius,
                                           std::clamp( static_cast<double>( disc.HoleRadius ), 0.0, radius * 0.999 ),
                                           slices, rings, single );
        return Detail::Finish( g, options );
    }

    // Sphere: UAddSpherePrimitiveTool::GenerateMesh (AddPrimitiveTool.cpp:795-839). Lat Long takes
    // PerFace as PerQuad (UE's comment: its PerFace would be one group); Box has 6 faces.
    inline ShapeMesh MakeSphere( const SphereShape& sphere, const ShapeOptions& options = {} )
    {
        Detail::UEShapeBuffers g;
        const double           radius = Detail::Extent( sphere.Radius );
        if ( sphere.SubdivisionType == SphereType::LatLong )
            Detail::GenerateLatLongSphere( g, radius, Detail::Count( sphere.HorizontalSlices, 4 ) + 1,
                                           Detail::Count( sphere.VerticalSlices, 4 ),
                                           options.Groups != ShapePolygroupMode::PerShape );
        else
            Detail::GenerateBoxSphere( g, radius, Detail::Count( sphere.Subdivisions, 1 ),
                                       options.Groups == ShapePolygroupMode::PerQuad );
        return Detail::Finish( g, options );
    }

    // Cylinder: UAddCylinderPrimitiveTool::GenerateMesh (AddPrimitiveTool.cpp:695-711). Polygroups PerFace:
    // the two caps and one per height segment.
    inline ShapeMesh MakeCylinder( const CylinderShape& cylinder, const ShapeOptions& options = {} )
    {
        Detail::UEShapeBuffers g;
        const float            r = Detail::Extent( cylinder.Radius );
        const float            h = Detail::Extent( cylinder.Height );
        const int              n = Detail::Count( cylinder.HeightSubdivisions, 1 );
        std::vector<float>     radii, heights;
        for ( int i = 0; i <= n; ++i )
        {
            radii.push_back( r );
            heights.push_back( h * static_cast<float>( i ) / static_cast<float>( n ) );
        }
        Detail::GenerateVerticalCircleSweep( g, radii, heights, {}, Detail::Count( cylinder.RadialSlices, 3 ),
                                             options.Groups == ShapePolygroupMode::PerQuad );
        return Detail::Finish( g, options );
    }

    // Cone: UAddConePrimitiveTool::GenerateMesh (AddPrimitiveTool.cpp:739-758) - UE's cone is a cylinder
    // whose top radius is 0.01 cm, capped there as well.
    inline ShapeMesh MakeCone( const ConeShape& cone, const ShapeOptions& options = {} )
    {
        Detail::UEShapeBuffers g;
        const float            r = Detail::Extent( cone.Radius );
        const float            h = Detail::Extent( cone.Height );
        const int              n = Detail::Count( cone.HeightSubdivisions, 1 );
        constexpr float        kTip = 0.01f;
        std::vector<float>     radii, heights;
        for ( int i = 0; i <= n; ++i )
        {
            const float along = static_cast<float>( i ) / static_cast<float>( n );
            radii.push_back( r + ( kTip - r ) * along );
            heights.push_back( h * along );
        }
        Detail::GenerateVerticalCircleSweep( g, radii, heights, {}, Detail::Count( cone.RadialSlices, 3 ),
                                             options.Groups == ShapePolygroupMode::PerQuad );
        return Detail::Finish( g, options );
    }

    // Arrow along +Y: UAddArrowPrimitiveTool::GenerateMesh (AddPrimitiveTool.cpp:763-791) and
    // FArrowGenerator::Generate (SweepGenerator.cpp:561-601): shaft, the head's flat underside and the head
    // cone, split by sharp normals, the tip a 0.01 cm cap. Polygroups PerFace: 2 caps + 3 segments' worth.
    inline ShapeMesh MakeArrow( const ArrowShape& arrow, const ShapeOptions& options = {} )
    {
        Detail::UEShapeBuffers g;
        const float            sr         = Detail::Extent( arrow.ShaftRadius );
        const float            sl         = Detail::Extent( arrow.ShaftHeight );
        const float            hr         = Detail::Extent( arrow.HeadRadius );
        const float            hl         = Detail::Extent( arrow.HeadHeight );
        const int              additional = Detail::Count( arrow.HeightSubdivisions, 1 ) - 1;
        const float            srcRadii[] = { sr, sr, hr, 0.01f };
        const float            srcHeights[] = { 0.0f, sl, sl, sl + hl };
        std::vector<float>     radii, heights;
        for ( int seg = 0;; ++seg )
        {
            radii.push_back( srcRadii[seg] );
            heights.push_back( srcHeights[seg] );
            if ( seg == 3 )
                break;
            for ( int k = 1; k < additional + 1; ++k )
            {
                const float along = static_cast<float>( k ) / static_cast<float>( additional + 1 );
                radii.push_back( srcRadii[seg] + ( srcRadii[seg + 1] - srcRadii[seg] ) * along );
                heights.push_back( srcHeights[seg] + ( srcHeights[seg + 1] - srcHeights[seg] ) * along );
            }
        }
        Detail::GenerateVerticalCircleSweep( g, radii, heights, { 1 + additional, 2 + 2 * additional },
                                             Detail::Count( arrow.RadialSlices, 3 ),
                                             options.Groups == ShapePolygroupMode::PerQuad );
        return Detail::Finish( g, options );
    }

    // Capsule: UAddCapsulePrimitiveTool::GenerateMesh (AddPrimitiveTool.cpp:717-733); total height is
    // CylinderLength + 2 Radius. UE's capsule has no per-face split: PerFace is one group.
    inline ShapeMesh MakeCapsule( const CapsuleShape& capsule, const ShapeOptions& options = {} )
    {
        Detail::UEShapeBuffers g;
        Detail::GenerateCapsule( g, Detail::Extent( capsule.Radius ), Detail::Extent( capsule.CylinderLength ),
                                 Detail::Count( capsule.HemisphereSlices, 2 ), Detail::Count( capsule.CylinderSlices, 3 ),
                                 Detail::Count( capsule.CylinderSubdivisions, 0 ),
                                 options.Groups == ShapePolygroupMode::PerQuad );
        return Detail::Finish( g, options );
    }

    // Torus around +Y: UAddTorusPrimitiveTool::GenerateMesh (AddPrimitiveTool.cpp:668-690). Polygroups
    // PerFace: one per major slice (UE's sweep groups each path segment). A genus-1 shell: V - E + F = 0.
    inline ShapeMesh MakeTorus( const TorusShape& torus, const ShapeOptions& options = {} )
    {
        Detail::UEShapeBuffers g;
        Detail::GenerateTorus( g, Detail::Extent( torus.MajorRadius ), Detail::Extent( torus.MinorRadius ),
                               Detail::Count( torus.MajorSlices, 3 ), Detail::Count( torus.MinorSlices, 3 ),
                               options.Groups == ShapePolygroupMode::PerQuad );
        return Detail::Finish( g, options );
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

    // ── STAIRS: UE's four stair generators on one lattice ──────────────────────────────────────────
    //
    // Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Generators/StairGenerator.cpp:30-780,
    // 866-1262 (FStairGenerator::GenerateSolidStairs / GenerateFloatingStairs and the GenerateVertex of
    // FLinear-, FFloating-, FCurved- and FSpiralStairGenerator), adapted: UE's column/row vertex layout is kept
    // as a lattice of (column, row) points - column = step index along the flight, row = step-height units -
    // and each stair type is only a map from (side, column, row) to a position, as in UE. UE's FaceDesc /
    // NormalDesc / UVDesc tables are replaced by emitting each quad through this file's AddQuad, so a flat
    // face gets its hard normal from its own winding and the weld in ShapeToEditMesh joins the shell; +Y is up
    // and the flight climbs along +Z (UE: +Z up, +X forward); the side wall of a curved flight is smooth-shaded
    // with the radial normal, as UE's FCurvedStairGenerator::GenerateNormal gives it.

    // UE's EProceduralStairsType.
    enum class StairsType
    {
        Linear,   // solid flight, every step standing on the floor
        Floating, // each step rests only on the one below it (UE's EStairStyle::Floating)
        Curved,   // Linear bent around a vertical axis
        Spiral,   // Floating bent around a vertical axis
    };

    constexpr const char* ToString( StairsType type )
    {
        switch ( type )
        {
            case StairsType::Linear:
                return "Linear";
            case StairsType::Floating:
                return "Floating";
            case StairsType::Curved:
                return "Curved";
            case StairsType::Spiral:
                return "Spiral";
        }
        return "Linear";
    }

    // UE's UProceduralStairsToolProperties, in centimetres. StepDepth is read by Linear and Floating only,
    // InnerRadius and CurveAngle by Curved and Spiral only (UE's EditConditions): a curved tread's depth is
    // what the angle and the radii make it.
    struct StairsShape
    {
        StairsType Type        = StairsType::Linear;
        int        Steps       = 8;
        float      StepWidth   = 150.0f;
        float      StepHeight  = 20.0f;
        float      StepDepth   = 30.0f;
        float      InnerRadius = 150.0f;
        float      CurveAngle  = 90.0f; // degrees the whole flight turns; the sign picks the direction

        bool operator==( const StairsShape& ) const = default;
    };

    namespace Detail
    {
        // A curved flight is one solid block, so its first riser and its back must not meet: at 360 degrees
        // they coincide and the weld would fold the shell onto itself (UE's generator never welds, so its
        // clamp is 360). A spiral is floating, and two turns only touch when fewer than four steps make a
        // turn, so it may wind as far as 90 degrees per step.
        inline constexpr float kMinStairsDegrees        = 1.0f;
        inline constexpr float kMaxCurvedStairsDegrees  = 359.0f;
        inline constexpr float kMaxSpiralDegreesPerStep = 90.0f;

        // The side profile of a flight on the (column, row) lattice. Strip s is the column span [s, s+1];
        // it is solid from row Bottom(s) to row Top(s). UE's diagrams, StairGenerator.cpp:30-49 and 374-392.
        struct StairProfile
        {
            bool Floating = false;

            [[nodiscard]] int Bottom( int strip ) const
            {
                return Floating ? std::max( strip - 1, 0 ) : 0;
            }
            [[nodiscard]] static int Top( int strip )
            {
                return strip + 1;
            }
        };

        // One flat quad whose corners carry their own normals (a curved stair wall).
        inline void AddSmoothQuad( ShapeMesh& m, int group, const std::array<glm::vec3, 4>& p,
                                   const std::array<glm::vec3, 4>& n, const std::array<glm::vec2, 4>& uv )
        {
            const glm::vec3 t    = glm::normalize( p[1] - p[0] );
            const auto      base = static_cast<uint32_t>( m.Vertices.size() );
            for ( int i = 0; i < 4; ++i )
                PushVertex( m, p[i], n[i], t, uv[i] );
            m.Indices.push_back( { base + 0, base + 1, base + 2 } );
            m.Indices.push_back( { base + 2, base + 3, base + 0 } );
            m.Groups.push_back( group );
            m.Groups.push_back( group );
        }

        inline void CentreOnXZ( ShapeMesh& m )
        {
            const Common::Math::AABB box = m.Bounds();
            const glm::vec3          mid = 0.5f * ( box.Min + box.Max );
            for ( Vertex& v : m.Vertices )
            {
                v.Position.x -= mid.x;
                v.Position.z -= mid.z;
            }
        }
    } // namespace Detail

    // A flight of `Steps` steps. ONE closed shell: each side wall is split into one cell per lattice square,
    // and every lattice edge on the profile's outline carries one quad across the width (a tread, a riser, the
    // back or a piece of the bottom), so edges meet exactly and there is no T-junction. Centred on X and Z.
    // Logical faces: 4 + 2 * Steps (two sides, back, bottom, every riser, every tread). Quads: Linear/Curved
    // Steps*(Steps+1) side cells + 4*Steps across; Floating/Spiral 2*(2*Steps-1) + 4*Steps (UE's counts).
    inline ShapeMesh MakeStairs( const StairsShape& stairs, const ShapeOptions& options = {} )
    {
        ShapeMesh             m;
        Detail::GroupAssigner groups( options.Groups );
        const int             n        = std::max( stairs.Steps, 1 );
        const auto            nf       = static_cast<float>( n );
        const bool            floating = stairs.Type == StairsType::Floating || stairs.Type == StairsType::Spiral;
        const bool            curved   = stairs.Type == StairsType::Curved || stairs.Type == StairsType::Spiral;
        const float           w        = Detail::Extent( stairs.StepWidth );
        const float           sh       = Detail::Extent( stairs.StepHeight );
        const float           sd       = Detail::Extent( stairs.StepDepth );
        const float           inner    = Detail::Extent( stairs.InnerRadius );
        const float           turn     = stairs.Type == StairsType::Curved ? Detail::kMaxCurvedStairsDegrees
                                                                           : Detail::kMaxSpiralDegreesPerStep * nf;
        const float degrees = std::clamp( std::abs( stairs.CurveAngle ), Detail::kMinStairsDegrees, turn );
        const float perStep = glm::radians( stairs.CurveAngle < 0.0f ? -degrees : degrees ) / nf;
        const Detail::StairProfile profile{ floating };

        // Side 0 is -X on a straight flight and the inner radius on a curved one; side 1 is +X / the outer
        // radius. A negative angle turns the flight the other way, which mirrors it: every quad is then wound
        // the other way round so the shell still faces out.
        const bool mirrored = curved && perStep < 0.0f;
        const auto at       = [&]( int side, int column, int row ) -> glm::vec3
        {
            const float y = sh * static_cast<float>( row );
            if ( !curved )
                return { side == 1 ? 0.5f * w : -0.5f * w, y, ( static_cast<float>( column ) - 0.5f * nf ) * sd };
            const float r = side == 1 ? inner + w : inner;
            const float a = perStep * static_cast<float>( column );
            return { r * std::cos( a ), y, r * std::sin( a ) };
        };
        const auto wind = [mirrored]( auto corners )
        {
            if ( mirrored )
                std::swap( corners[1], corners[3] );
            return corners;
        };

        int face = 0;
        // Side walls: one cell per lattice square, UV the wall's own (column, row) / Steps.
        for ( const int side : { 0, 1 } )
        {
            const int wallFace = face++;
            for ( int s = 0; s < n; ++s )
                for ( int j = profile.Bottom( s ); j < Detail::StairProfile::Top( s ); ++j )
                {
                    // Side 1 goes up first then along the flight, cross(+Y, +Z) = +X; side 0 the other way.
                    std::array<glm::ivec2, 4> cell = { glm::ivec2( s, j ), glm::ivec2( s + 1, j ),
                                                       glm::ivec2( s + 1, j + 1 ), glm::ivec2( s, j + 1 ) };
                    if ( side == 1 )
                        std::swap( cell[1], cell[3] );
                    cell = wind( cell );
                    std::array<glm::vec3, 4> p{};
                    std::array<glm::vec3, 4> nrm{};
                    std::array<glm::vec2, 4> uv{};
                    for ( int k = 0; k < 4; ++k )
                    {
                        p[k]  = at( side, cell[k].x, cell[k].y );
                        uv[k] = glm::vec2( cell[k] ) / nf;
                        // UE's curved wall normal: straight out from the axis, into it on the inner wall.
                        nrm[k] =
                             glm::normalize( glm::vec3( p[k].x, 0.0f, p[k].z ) ) * ( side == 1 ? 1.0f : -1.0f );
                    }
                    if ( curved )
                        Detail::AddSmoothQuad( m, groups.Next( wallFace ), p, nrm, uv );
                    else
                        Detail::AddQuad( m, groups.Next( wallFace ), p, uv );
                }
        }

        // One quad across the width on the outline edge p0 -> p1. The edge runs so that the quad
        // side0(p0), side0(p1), side1(p1), side1(p0) faces out: cross(p1 - p0, +X) is the outward normal.
        const auto across = [&]( int group, glm::ivec2 p0, glm::ivec2 p1, float v0, float v1 )
        {
            const std::array<glm::vec3, 4> p  = wind( std::array<glm::vec3, 4>{
                 at( 0, p0.x, p0.y ), at( 0, p1.x, p1.y ), at( 1, p1.x, p1.y ), at( 1, p0.x, p0.y ) } );
            const std::array<glm::vec2, 4> uv = wind( std::array<glm::vec2, 4>{
                 glm::vec2( 0.0f, v0 ), glm::vec2( 0.0f, v1 ), glm::vec2( 1.0f, v1 ), glm::vec2( 1.0f, v0 ) } );
            Detail::AddQuad( m, group, p, uv );
        };

        // Back: the last column, top to bottom.
        const int backFace = face++;
        {
            const int lowest = profile.Bottom( n - 1 );
            const int rows   = n - lowest;
            for ( int j = n - 1; j >= lowest; --j )
                across( groups.Next( backFace ), { n, j + 1 }, { n, j },
                        static_cast<float>( n - 1 - j ) / static_cast<float>( rows ),
                        static_cast<float>( n - j ) / static_cast<float>( rows ) );
        }
        // Bottom, back to front: each strip's underside, and on a floating flight the short upright piece
        // between two undersides (it faces the back, UE's ESide::Bottom).
        const int bottomFace = face++;
        {
            std::vector<std::pair<glm::ivec2, glm::ivec2>> run;
            for ( int s = n - 1; s >= 0; --s )
            {
                const int b = profile.Bottom( s );
                run.emplace_back( glm::ivec2( s + 1, b ), glm::ivec2( s, b ) );
                if ( s > 0 && profile.Bottom( s - 1 ) < b )
                    run.emplace_back( glm::ivec2( s, b ), glm::ivec2( s, profile.Bottom( s - 1 ) ) );
            }
            const auto count = static_cast<float>( run.size() );
            for ( size_t k = 0; k < run.size(); ++k )
                across( groups.Next( bottomFace ), run[k].first, run[k].second, static_cast<float>( k ) / count,
                        static_cast<float>( k + 1 ) / count );
        }
        // Every step: its riser (facing down the flight) and its tread (facing up).
        for ( int s = 0; s < n; ++s )
        {
            across( groups.Next( face++ ), { s, s }, { s, s + 1 }, 0.0f, 1.0f );
            across( groups.Next( face++ ), { s, s + 1 }, { s + 1, s + 1 }, 0.0f, 1.0f );
        }
        Detail::CentreOnXZ( m );
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
                return MakeSphere( { 0.5f * size, SphereType::LatLong, 1, 16, 32 }, centred );
            case PrimitiveType::Plane:
                return MakePlane( glm::vec2( size ), glm::ivec2( 1 ), centred );
            case PrimitiveType::Pyramid:
                return MakePyramid( glm::vec3( size ), centred );
            case PrimitiveType::Cylinder:
                return MakeCylinder( { 0.5f * size, size, 32, 1 }, centred );
            case PrimitiveType::Capsule:
                return MakeCapsule( { 0.25f * size, 0.5f * size, 8, 32, 1 }, centred );
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
