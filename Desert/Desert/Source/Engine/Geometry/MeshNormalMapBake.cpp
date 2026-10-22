// Ported (the pattern, not the letter) from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing ... ModelingOperators
// MeshMapBaker.cpp (FMeshMapBaker::Bake: texel loop over the target's UV occupancy, gutter fill),
// MeshBakerCommon.cpp (GetDetailMeshTrianglePoint_Raycast: the ray from the target point pushed out along its
// normal by the projection distance, cast back along -normal across twice that distance, nearest hit) and
// MeshNormalMapEvaluator.cpp (the detail normal interpolated at the hit, written in the target's tangent frame).
// See MeshNormalMapBake.hpp for what differs.

#include "MeshNormalMapBake.hpp"

#include "TriangleBvh.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace Desert::Geometry
{
    namespace
    {
        constexpr int kMinResolution = 16;
        constexpr int kMaxResolution = 8192;

        uint8_t Encode( const double v )
        {
            const double c = std::clamp( v * 0.5 + 0.5, 0.0, 1.0 );
            return static_cast<uint8_t>( std::lround( c * 255.0 ) );
        }

        void WriteFlat( uint8_t* texel )
        {
            texel[0] = 128;
            texel[1] = 128;
            texel[2] = 255;
            texel[3] = 255;
        }

        // Barycentrics of @p p in the 2D triangle (a, b, c); false for a degenerate triangle.
        bool Barycentric2D( const glm::dvec2& p, const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c,
                            glm::dvec3& out )
        {
            const glm::dvec2 v0 = b - a, v1 = c - a, v2 = p - a;
            const double     den = v0.x * v1.y - v1.x * v0.y;
            if ( std::abs( den ) < 1e-20 )
                return false;
            const double v = ( v2.x * v1.y - v1.x * v2.y ) / den;
            const double w = ( v0.x * v2.y - v2.x * v0.y ) / den;
            out            = { 1.0 - v - w, v, w };
            return true;
        }

        // Barycentrics of a point ON the 3D triangle a + s*e1 + t*e2.
        glm::dvec3 Barycentric3D( const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& e1,
                                  const glm::dvec3& e2 )
        {
            const glm::dvec3 d   = p - a;
            const double     d00 = glm::dot( e1, e1 ), d01 = glm::dot( e1, e2 ), d11 = glm::dot( e2, e2 );
            const double     d20 = glm::dot( d, e1 ), d21 = glm::dot( d, e2 );
            const double     den = d00 * d11 - d01 * d01;
            if ( std::abs( den ) < 1e-30 )
                return { 1.0, 0.0, 0.0 };
            const double v = ( d11 * d20 - d01 * d21 ) / den;
            const double w = ( d00 * d21 - d01 * d20 ) / den;
            return { 1.0 - v - w, v, w };
        }
    } // namespace

    Common::ResultStr<BakeSurface> BakeSurfaceFromRender( const RenderMeshData& render, const glm::dmat4& world,
                                                          const bool needTangentFrame )
    {
        BakeSurface out;
        for ( const Submesh& submesh : render.Submeshes )
        {
            const glm::dmat4 m      = world * glm::dmat4( submesh.Transform );
            const glm::dmat3 normal = glm::transpose( glm::inverse( glm::dmat3( m ) ) );
            const uint32_t   base   = static_cast<uint32_t>( out.Positions.size() );
            if ( static_cast<size_t>( submesh.VertexOffset ) + submesh.VertexCount > render.Vertices.size() ||
                 static_cast<size_t>( submesh.IndexOffset ) + submesh.IndexCount > render.Indices.size() )
                return Common::MakeFormattedError<BakeSurface>(
                     "submesh '{}' reaches past the arrays ({} vertices, {} indices)", submesh.Name,
                     render.Vertices.size(), render.Indices.size() );
            for ( uint32_t i = 0; i < submesh.VertexCount; ++i )
            {
                const Vertex& v = render.Vertices[submesh.VertexOffset + i];
                const auto    t = normal * glm::dvec3( v.Tangent );
                const auto    b = normal * glm::dvec3( v.Bitangent );
                const auto    n = normal * glm::dvec3( v.Normal );
                if ( glm::length( n ) < 1e-12 )
                    return Common::MakeFormattedError<BakeSurface>( "vertex {} has a zero normal", base + i );
                if ( needTangentFrame && ( glm::length( t ) < 1e-12 || glm::length( b ) < 1e-12 ) )
                    return Common::MakeFormattedError<BakeSurface>(
                         "vertex {} has no tangent frame (tangent {:.3g}, bitangent {:.3g}) - a mesh without a "
                         "tangent space cannot carry a tangent-space normal map",
                         base + i, glm::length( t ), glm::length( b ) );
                out.Positions.push_back( glm::dvec3( m * glm::dvec4( glm::dvec3( v.Position ), 1.0 ) ) );
                out.Normals.push_back( glm::normalize( n ) );
                out.Tangents.push_back( glm::length( t ) < 1e-12 ? glm::dvec3( 0.0 ) : glm::normalize( t ) );
                out.Bitangents.push_back( glm::length( b ) < 1e-12 ? glm::dvec3( 0.0 ) : glm::normalize( b ) );
                out.UVs.push_back( glm::dvec2( v.TexCoord ) );
            }
            for ( uint32_t i = 0; i + 2 < submesh.IndexCount; i += 3 )
            {
                const size_t at = submesh.IndexOffset + i;
                out.Triangles.push_back( { base + static_cast<uint32_t>( render.Indices[at] ),
                                           base + static_cast<uint32_t>( render.Indices[at + 1] ),
                                           base + static_cast<uint32_t>( render.Indices[at + 2] ) } );
            }
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    Common::ResultStr<NormalMapBakeResult> BakeTangentNormalMap( const BakeSurface&           target,
                                                                 const BakeSurface&           detail,
                                                                 const NormalMapBakeSettings& settings )
    {
        using Out = NormalMapBakeResult;
        if ( settings.Resolution < kMinResolution || settings.Resolution > kMaxResolution )
            return Common::MakeFormattedError<Out>( "resolution {} is outside [{}, {}]", settings.Resolution,
                                                    kMinResolution, kMaxResolution );
        if ( !( settings.ProjectionDistance > 0.0 ) )
            return Common::MakeFormattedError<Out>( "projection distance {} cm is not positive",
                                                    settings.ProjectionDistance );
        if ( target.Triangles.empty() || target.UVs.size() != target.Positions.size() )
            return Common::MakeFormattedError<Out>( "the target has {} triangles and {} UVs for {} vertices",
                                                    target.Triangles.size(), target.UVs.size(),
                                                    target.Positions.size() );
        if ( detail.Triangles.empty() )
            return Common::MakeError<Out>( "the detail mesh has no triangles" );

        const TriangleBvh bvh = TriangleBvh::Build( detail.Positions, detail.Triangles );

        Out out;
        out.Width   = settings.Resolution;
        out.Height  = settings.Resolution;
        const int W = out.Width, H = out.Height;
        out.Rgba.assign( static_cast<size_t>( W ) * H * 4, 0 );
        // 0 = not covered, 1 = covered (sampled), 2 = gutter-filled
        std::vector<uint8_t> state( static_cast<size_t>( W ) * H, 0 );

        const double D = settings.ProjectionDistance;
        for ( const glm::uvec3& tri : target.Triangles )
        {
            // The triangle in texel space: x = u * W, y = (1 - v) * H (the shader samples (u, 1 - v)).
            std::array<glm::dvec2, 3> px;
            for ( int c = 0; c < 3; ++c )
            {
                const glm::dvec2 uv = target.UVs[tri[c]];
                px[c]               = { uv.x * W, ( 1.0 - uv.y ) * H };
            }
            const int x0 =
                 std::max( 0, static_cast<int>( std::floor( std::min( { px[0].x, px[1].x, px[2].x } ) ) ) );
            const int x1 =
                 std::min( W - 1, static_cast<int>( std::ceil( std::max( { px[0].x, px[1].x, px[2].x } ) ) ) );
            const int y0 =
                 std::max( 0, static_cast<int>( std::floor( std::min( { px[0].y, px[1].y, px[2].y } ) ) ) );
            const int y1 =
                 std::min( H - 1, static_cast<int>( std::ceil( std::max( { px[0].y, px[1].y, px[2].y } ) ) ) );
            for ( int y = y0; y <= y1; ++y )
                for ( int x = x0; x <= x1; ++x )
                {
                    glm::dvec3 bc;
                    if ( !Barycentric2D( { x + 0.5, y + 0.5 }, px[0], px[1], px[2], bc ) )
                        continue;
                    constexpr double kEdge = -1e-9;
                    if ( bc.x < kEdge || bc.y < kEdge || bc.z < kEdge )
                        continue;
                    const size_t texel = static_cast<size_t>( y ) * W + x;
                    if ( state[texel] == 0 )
                        ++out.CoveredTexels;
                    state[texel] = 1;

                    const auto lerp = [&]( const std::vector<glm::dvec3>& a )
                    { return a[tri[0]] * bc.x + a[tri[1]] * bc.y + a[tri[2]] * bc.z; };
                    const glm::dvec3 P    = lerp( target.Positions );
                    const glm::dvec3 T    = lerp( target.Tangents );
                    const glm::dvec3 B    = lerp( target.Bitangents );
                    const glm::dvec3 N    = lerp( target.Normals );
                    uint8_t*         rgba = &out.Rgba[texel * 4];
                    const double     nl   = glm::length( N );
                    if ( nl < 1e-12 )
                    {
                        WriteFlat( rgba );
                        continue;
                    }
                    const glm::dvec3 dir    = -N / nl;
                    const glm::dvec3 origin = P - dir * D;
                    const double     o[3]   = { origin.x, origin.y, origin.z };
                    const double     d[3]   = { dir.x, dir.y, dir.z };
                    const auto       hit    = bvh.ClosestHit( o, d, 0.0, 2.0 * D );
                    if ( hit.Triangle < 0 )
                    {
                        WriteFlat( rgba );
                        continue;
                    }
                    const glm::uvec3& dt  = detail.Triangles[static_cast<size_t>( hit.Triangle )];
                    const glm::dvec3  hp  = origin + dir * hit.T;
                    const glm::dvec3  dbc = Barycentric3D( hp, bvh.A[static_cast<size_t>( hit.Triangle )],
                                                           bvh.E1[static_cast<size_t>( hit.Triangle )],
                                                           bvh.E2[static_cast<size_t>( hit.Triangle )] );
                    glm::dvec3        dn  = detail.Normals[dt[0]] * dbc.x + detail.Normals[dt[1]] * dbc.y +
                                    detail.Normals[dt[2]] * dbc.z;
                    if ( glm::length( dn ) < 1e-12 )
                    {
                        WriteFlat( rgba );
                        continue;
                    }
                    dn = glm::normalize( dn );
                    // The shader shades with TBN * decoded: store solve(TBN, n).
                    const glm::dmat3 tbn( T, B, N );
                    if ( std::abs( glm::determinant( tbn ) ) < 1e-12 )
                    {
                        WriteFlat( rgba );
                        continue;
                    }
                    glm::dvec3 ts = glm::inverse( tbn ) * dn;
                    ts.z          = std::max( ts.z, 0.0 );
                    ts            = glm::length( ts ) < 1e-12 ? glm::dvec3( 0, 0, 1 ) : glm::normalize( ts );
                    rgba[0]       = Encode( ts.x );
                    rgba[1]       = Encode( ts.y );
                    rgba[2]       = Encode( ts.z );
                    rgba[3]       = 255;
                    ++out.HitTexels;
                }
        }
        if ( out.HitTexels == 0 )
            return Common::MakeFormattedError<Out>(
                 "none of the {} covered texels found the detail surface within {} cm - the meshes do not overlap "
                 "within the projection distance",
                 out.CoveredTexels, D );

        // Gutter: each ring takes the average of its filled 8-neighbours.
        for ( int ring = 0; ring < settings.GutterTexels; ++ring )
        {
            std::vector<size_t> filled;
            for ( int y = 0; y < H; ++y )
                for ( int x = 0; x < W; ++x )
                {
                    const size_t texel = static_cast<size_t>( y ) * W + x;
                    if ( state[texel] != 0 )
                        continue;
                    int sum[3] = { 0, 0, 0 }, n = 0;
                    for ( int dy = -1; dy <= 1; ++dy )
                        for ( int dx = -1; dx <= 1; ++dx )
                        {
                            const int nx = x + dx, ny = y + dy;
                            if ( nx < 0 || ny < 0 || nx >= W || ny >= H )
                                continue;
                            const size_t nt = static_cast<size_t>( ny ) * W + nx;
                            if ( state[nt] == 0 || state[nt] == 3 )
                                continue;
                            for ( int c = 0; c < 3; ++c )
                                sum[c] += out.Rgba[nt * 4 + c];
                            ++n;
                        }
                    if ( n == 0 )
                        continue;
                    for ( int c = 0; c < 3; ++c )
                        out.Rgba[texel * 4 + c] = static_cast<uint8_t>( ( sum[c] + n / 2 ) / n );
                    out.Rgba[texel * 4 + 3] = 255;
                    state[texel]            = 3; // this ring: not a source for its own ring
                    filled.push_back( texel );
                }
            for ( const size_t texel : filled )
                state[texel] = 2;
            out.GutterTexels += filled.size();
        }
        for ( size_t texel = 0; texel < state.size(); ++texel )
            if ( state[texel] == 0 )
                WriteFlat( &out.Rgba[texel * 4] );
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry
