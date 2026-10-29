// Ported (the pattern, not the letter) from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing ... ModelingOperators
// MeshMapBaker.cpp (FMeshMapBaker::Bake: texel loop over the target's UV occupancy, gutter fill),
// MeshBakerCommon.cpp (GetDetailMeshTrianglePoint_Raycast: the ray from the target point pushed out along its
// normal by the projection distance, cast back along -normal across twice that distance, nearest hit) and
// MeshNormalMapEvaluator.cpp (the detail normal interpolated at the hit, written in the target's tangent frame).
// See MeshNormalMapBake.hpp for what differs.

#include "MeshNormalMapBake.hpp"

#include "TriangleBvh.hpp"

#include <Common/Core/JobSystem.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

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

        // Texels whose centre lies strictly inside two or more target triangles in UV space (overlapping UVs -
        // e.g. six box faces each mapped to [0,1]^2). Texels on a shared edge are not counted: the test is
        // interior-only.
        struct UvOverlap
        {
            std::size_t Covered    = 0;
            std::size_t Overlapped = 0;
        };

        UvOverlap MeasureUvOverlap( const BakeSurface& target, const int W, const int H )
        {
            std::vector<uint8_t> inside( static_cast<size_t>( W ) * H, 0 );
            UvOverlap            r;
            for ( const glm::uvec3& tri : target.Triangles )
            {
                std::array<glm::dvec2, 3> px;
                for ( int c = 0; c < 3; ++c )
                    px[c] = { target.UVs[tri[c]].x * W, ( 1.0 - target.UVs[tri[c]].y ) * H };
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
                        constexpr double kInterior = 1e-6;
                        if ( bc.x <= kInterior || bc.y <= kInterior || bc.z <= kInterior )
                            continue;
                        uint8_t& n = inside[static_cast<size_t>( y ) * W + x];
                        if ( n == 0 )
                            ++r.Covered;
                        else if ( n == 1 )
                            ++r.Overlapped;
                        n = static_cast<uint8_t>( std::min( 2, n + 1 ) );
                    }
            }
            return r;
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
                 static_cast<size_t>( submesh.IndexOffset ) + submesh.IndexCount > render.Indices.size() * 3 )
                return Common::MakeFormattedError<BakeSurface>(
                     "submesh '{}' reaches past the arrays ({} vertices, {} indices)", submesh.Name,
                     render.Vertices.size(), render.Indices.size() * 3 );
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
            // The index buffer is triangles (Index), the submesh ranges count uint32 indices.
            static_assert( sizeof( Index ) == 3 * sizeof( uint32_t ) );
            const auto* flat = reinterpret_cast<const uint32_t*>( render.Indices.data() );
            for ( uint32_t i = 0; i + 2 < submesh.IndexCount; i += 3 )
            {
                const size_t at = submesh.IndexOffset + i;
                out.Triangles.push_back( { base + flat[at], base + flat[at + 1], base + flat[at + 2] } );
            }
        }
        return Common::MakeSuccess( std::move( out ) );
    }

    namespace
    {
        void WriteGrey( uint8_t* texel, const double v )
        {
            const uint8_t g = static_cast<uint8_t>( std::lround( std::clamp( v, 0.0, 1.0 ) * 255.0 ) );
            texel[0] = texel[1] = texel[2] = g;
            texel[3]                       = 255;
        }

        // UE FMeshOcclusionMapEvaluator's ray set, in the frame of the normal (z up): spherical-Fibonacci points
        // over the part of the hemisphere the spread admits and the bias does not drop (z >= sin(bias)), uniform
        // in area; each ray then weighs its cosine.
        std::vector<glm::dvec3> OcclusionDirections( const MeshMapBakeSettings& s )
        {
            const double            minZ    = std::max( std::cos( glm::radians( s.OcclusionSpreadAngle * 0.5 ) ),
                                                        std::sin( glm::radians( s.OcclusionBiasAngle ) ) );
            constexpr double        kGolden = 2.39996322972865332; // pi * (3 - sqrt 5)
            std::vector<glm::dvec3> dirs;
            for ( int i = 0; i < s.OcclusionRays; ++i )
            {
                const double z   = 1.0 - ( i + 0.5 ) / s.OcclusionRays * ( 1.0 - minZ );
                const double r   = std::sqrt( std::max( 0.0, 1.0 - z * z ) );
                const double phi = i * kGolden;
                dirs.push_back( { r * std::cos( phi ), r * std::sin( phi ), z } );
            }
            return dirs;
        }

        // Per-vertex mean curvature H (convex > 0) of @p mesh welded by position, as UE's
        // FMeshVertexCurvatureCache reads the topology: the cotangent Laplacian of the position,
        // L p_i = (1 / 2A_i) sum_j (cot a_ij + cot b_ij)(p_j - p_i) = -2 H n_i, with A_i a third of the incident
        // triangle areas and n_i the vertex normal the mesh carries (averaged over the render copies of the
        // vertex). Returned per RENDER vertex (every copy of a welded vertex gets its value).
        std::vector<double> VertexMeanCurvature( const BakeSurface& mesh )
        {
            std::map<std::array<double, 3>, uint32_t> ids;
            std::vector<uint32_t>                     weld( mesh.Positions.size() );
            std::vector<glm::dvec3>                   pos;
            for ( size_t i = 0; i < mesh.Positions.size(); ++i )
            {
                const glm::dvec3& p    = mesh.Positions[i];
                const auto [it, added] = ids.try_emplace( { p.x, p.y, p.z }, static_cast<uint32_t>( pos.size() ) );
                if ( added )
                    pos.push_back( p );
                weld[i] = it->second;
            }
            const size_t            n = pos.size();
            std::vector<glm::dvec3> lap( n, glm::dvec3( 0.0 ) ), nrm( n, glm::dvec3( 0.0 ) );
            std::vector<double>     area( n, 0.0 );
            for ( size_t i = 0; i < mesh.Positions.size(); ++i )
                nrm[weld[i]] += mesh.Normals[i];
            for ( const glm::uvec3& tri : mesh.Triangles )
            {
                const std::array<uint32_t, 3> v = { weld[tri[0]], weld[tri[1]], weld[tri[2]] };
                if ( v[0] == v[1] || v[1] == v[2] || v[0] == v[2] )
                    continue;
                const double a = 0.5 * glm::length( glm::cross( pos[v[1]] - pos[v[0]], pos[v[2]] - pos[v[0]] ) );
                if ( a < 1e-12 )
                    continue;
                for ( int c = 0; c < 3; ++c )
                {
                    // The cotangent at corner c weighs the opposite edge (e0, e1).
                    const uint32_t   o = v[c], e0 = v[( c + 1 ) % 3], e1 = v[( c + 2 ) % 3];
                    const glm::dvec3 u = pos[e0] - pos[o], w = pos[e1] - pos[o];
                    const double     cot =
                         std::clamp( glm::dot( u, w ) / glm::length( glm::cross( u, w ) ), -1e3, 1e3 );
                    lap[e0] += cot * ( pos[e1] - pos[e0] );
                    lap[e1] += cot * ( pos[e0] - pos[e1] );
                    area[o] += a / 3.0;
                }
            }
            std::vector<double> welded( n, 0.0 );
            for ( size_t i = 0; i < n; ++i )
                if ( area[i] > 0.0 && glm::length( nrm[i] ) > 1e-12 )
                    welded[i] = -0.5 * glm::dot( lap[i] / ( 2.0 * area[i] ), glm::normalize( nrm[i] ) );
            std::vector<double> out( mesh.Positions.size() );
            for ( size_t i = 0; i < out.size(); ++i )
                out[i] = welded[weld[i]];
            return out;
        }

        // Which target triangle a texel's centre lies in, and where (UE FMeshMapBaker's rasterised occupancy).
        struct TexelSample
        {
            int        Triangle = -1;
            glm::dvec3 Bary{ 0.0 };
        };
    } // namespace

    Common::ResultStr<MeshMapBakeResult> BakeMeshMaps( const BakeSurface& target, const BakeSurface& detail,
                                                       const MeshMapBakeSettings& settings )
    {
        using Out = MeshMapBakeResult;
        if ( settings.Resolution < kMinResolution || settings.Resolution > kMaxResolution )
            return Common::MakeFormattedError<Out>( "resolution {} is outside [{}, {}]", settings.Resolution,
                                                    kMinResolution, kMaxResolution );
        if ( !( settings.ProjectionDistance > 0.0 ) )
            return Common::MakeFormattedError<Out>( "projection distance {} cm is not positive",
                                                    settings.ProjectionDistance );
        if ( settings.Occlusion &&
             ( settings.OcclusionRays < 1 || settings.OcclusionRays > 1024 ||
               !( settings.OcclusionSpreadAngle > 0.0 && settings.OcclusionSpreadAngle <= 180.0 ) ||
               !( settings.OcclusionBiasAngle >= 0.0 && settings.OcclusionBiasAngle < 90.0 ) ||
               settings.OcclusionMaxDistance < 0.0 ) )
            return Common::MakeFormattedError<Out>(
                 "occlusion: {} rays (1..1024), spread {} (0..180], bias {} [0..90), max distance {} cm (>= 0)",
                 settings.OcclusionRays, settings.OcclusionSpreadAngle, settings.OcclusionBiasAngle,
                 settings.OcclusionMaxDistance );
        if ( target.Triangles.empty() || target.UVs.size() != target.Positions.size() )
            return Common::MakeFormattedError<Out>( "the target has {} triangles and {} UVs for {} vertices",
                                                    target.Triangles.size(), target.UVs.size(),
                                                    target.Positions.size() );
        if ( detail.Triangles.empty() )
            return Common::MakeError<Out>( "the detail mesh has no triangles" );

        // UE's FMeshMapBaker writes whichever triangle rasterises a texel last; overlapping target UVs then
        // bake several surface points into one texel and the map is garbage. Refuse with the reason instead.
        if ( const UvOverlap uvo = MeasureUvOverlap( target, settings.Resolution, settings.Resolution );
             uvo.Overlapped * 100 > uvo.Covered )
            return Common::MakeFormattedError<MeshMapBakeResult>(
                 "the target's UVs overlap: {} of {} covered texels lie inside more than one triangle - unwrap "
                 "the "
                 "target (non-overlapping UVs, e.g. a Lat Long sphere or AutoUV) before baking",
                 uvo.Overlapped, uvo.Covered );

        const TriangleBvh bvh = TriangleBvh::Build( detail.Positions, detail.Triangles );

        Out out;
        out.Width   = settings.Resolution;
        out.Height  = settings.Resolution;
        const int    W = out.Width, H = out.Height;
        const size_t texels = static_cast<size_t>( W ) * H;
        out.Rgba.assign( texels * 4, 0 );
        if ( settings.Occlusion )
            out.OcclusionRgba.assign( texels * 4, 0 );
        if ( settings.Curvature )
            out.CurvatureRgba.assign( texels * 4, 0 );

        // Pass 1 (UE FMeshMapBaker's occupancy): the target triangle each texel centre lies in. A texel on an
        // edge two triangles share is taken by the last, as in UE; it is counted once.
        std::vector<TexelSample> samples( texels );
        for ( size_t t = 0; t < target.Triangles.size(); ++t )
        {
            const glm::uvec3& tri = target.Triangles[t];
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
                    TexelSample& s = samples[static_cast<size_t>( y ) * W + x];
                    if ( s.Triangle < 0 )
                        ++out.CoveredTexels;
                    s = { static_cast<int>( t ), bc };
                }
        }

        const std::vector<glm::dvec3> occlusionDirs =
             settings.Occlusion ? OcclusionDirections( settings ) : std::vector<glm::dvec3>{};
        const std::vector<double> detailH =
             settings.Curvature ? VertexMeanCurvature( detail ) : std::vector<double>{};
        const std::vector<double> targetH =
             settings.Curvature ? VertexMeanCurvature( target ) : std::vector<double>{};
        std::vector<double> curvature( settings.Curvature ? texels : 0, 0.0 );
        // 0 = not covered, 1 = covered (sampled), 4 = covered and hit; the gutter uses 2 and 3
        std::vector<uint8_t> state( texels, 0 );

        // Pass 2 (UE's tile-parallel evaluation): every covered texel finds its detail point and evaluates each
        // map there. Rows are independent - each texel is written by exactly one job.
        const double D              = settings.ProjectionDistance;
        const double occlusionReach = settings.OcclusionMaxDistance > 0.0 ? settings.OcclusionMaxDistance : 1e12;
        const auto   evaluateRow    = [&]( const size_t row )
        {
            for ( int x = 0; x < W; ++x )
            {
                const size_t       texel  = row * W + x;
                const TexelSample& sample = samples[texel];
                if ( sample.Triangle < 0 )
                    continue;
                state[texel]           = 1;
                const glm::uvec3& tri  = target.Triangles[static_cast<size_t>( sample.Triangle )];
                const glm::dvec3& bc   = sample.Bary;
                uint8_t*          rgba = &out.Rgba[texel * 4];
                WriteFlat( rgba );
                if ( settings.Occlusion )
                    WriteGrey( &out.OcclusionRgba[texel * 4], 1.0 ); // UE's default: unoccluded

                const auto lerp = [&]( const std::vector<glm::dvec3>& a )
                { return a[tri[0]] * bc.x + a[tri[1]] * bc.y + a[tri[2]] * bc.z; };
                const glm::dvec3 P  = lerp( target.Positions );
                const glm::dvec3 N  = lerp( target.Normals );
                const double     nl = glm::length( N );
                if ( nl < 1e-12 )
                    continue;
                const glm::dvec3 dir    = -N / nl;
                const glm::dvec3 origin = P - dir * D;
                const double     o[3]   = { origin.x, origin.y, origin.z };
                const double     d[3]   = { dir.x, dir.y, dir.z };
                const auto       hit    = bvh.ClosestHit( o, d, 0.0, 2.0 * D );
                if ( hit.Triangle < 0 )
                    continue;
                const size_t      ht  = static_cast<size_t>( hit.Triangle );
                const glm::uvec3& dt  = detail.Triangles[ht];
                const glm::dvec3  hp  = origin + dir * hit.T;
                const glm::dvec3  dbc = Barycentric3D( hp, bvh.A[ht], bvh.E1[ht], bvh.E2[ht] );
                glm::dvec3        dn =
                     detail.Normals[dt[0]] * dbc.x + detail.Normals[dt[1]] * dbc.y + detail.Normals[dt[2]] * dbc.z;
                if ( glm::length( dn ) < 1e-12 )
                    continue;
                dn           = glm::normalize( dn );
                state[texel] = 4;

                // Normal: the shader shades with TBN * decoded, so store solve(TBN, n).
                const glm::dmat3 tbn( lerp( target.Tangents ), lerp( target.Bitangents ), N );
                if ( std::abs( glm::determinant( tbn ) ) >= 1e-12 )
                {
                    glm::dvec3 ts = glm::inverse( tbn ) * dn;
                    ts.z          = std::max( ts.z, 0.0 );
                    ts            = glm::length( ts ) < 1e-12 ? glm::dvec3( 0, 0, 1 ) : glm::normalize( ts );
                    rgba[0]       = Encode( ts.x );
                    rgba[1]       = Encode( ts.y );
                    rgba[2]       = Encode( ts.z );
                    rgba[3]       = 255;
                }

                if ( settings.Occlusion )
                {
                    // The hemisphere stands on the detail TRIANGLE's plane, not on the interpolated normal: at a
                    // sharp crease whose vertices share one averaged normal (an Extrude's foot) the interpolated
                    // normal leans up to 45 degrees away from the wall, the hemisphere tilts out of the corner and
                    // the occlusion there halves (p8: 0.6 at the foot of a vertical 15 cm wall instead of <= 0.5).
                    // Visibility is a property of the surface, so its plane decides; the shading normal only
                    // picks the side.
                    glm::dvec3   fn = glm::cross( bvh.E1[ht], bvh.E2[ht] );
                    const double fl = glm::length( fn );
                    fn              = fl < 1e-12 ? dn : fn / fl * ( glm::dot( fn, dn ) < 0.0 ? -1.0 : 1.0 );
                    // Frisvad's branchless frame around that normal.
                    const double     sgn = fn.z >= 0.0 ? 1.0 : -1.0;
                    const double     a   = -1.0 / ( sgn + fn.z );
                    const double     b   = fn.x * fn.y * a;
                    const glm::dvec3 tx( 1.0 + sgn * fn.x * fn.x * a, sgn * b, -sgn * fn.x );
                    const glm::dvec3 ty( b, sgn + fn.y * fn.y * a, -fn.y );
                    const glm::dvec3 start = hp + fn * 1e-3; // off the surface, UE's normal offset
                    const double     so[3] = { start.x, start.y, start.z };
                    double           total = 0.0, blocked = 0.0;
                    for ( const glm::dvec3& l : occlusionDirs )
                    {
                        const glm::dvec3 w     = tx * l.x + ty * l.y + fn * l.z;
                        const double     wd[3] = { w.x, w.y, w.z };
                        total += l.z;
                        if ( bvh.ClosestHit( so, wd, 0.0, occlusionReach, hit.Triangle ).Triangle >= 0 )
                            blocked += l.z;
                    }
                    WriteGrey( &out.OcclusionRgba[texel * 4], total > 0.0 ? 1.0 - blocked / total : 1.0 );
                }
                if ( settings.Curvature )
                    curvature[texel] =
                         detailH[dt[0]] * dbc.x + detailH[dt[1]] * dbc.y + detailH[dt[2]] * dbc.z -
                         ( targetH[tri[0]] * bc.x + targetH[tri[1]] * bc.y + targetH[tri[2]] * bc.z );
            }
        };
        Common::JobSystem::Get().ParallelFor( static_cast<size_t>( H ), evaluateRow );

        for ( size_t texel = 0; texel < texels; ++texel )
            if ( state[texel] == 4 )
                ++out.HitTexels;
        if ( out.HitTexels == 0 )
            return Common::MakeFormattedError<Out>(
                 "none of the {} covered texels found the detail surface within {} cm - the meshes do not overlap "
                 "within the projection distance",
                 out.CoveredTexels, D );

        if ( settings.Curvature )
        {
            // Full scale at RangeMultiplier * (mean + standard deviation) of |H| over the hit texels.
            double sum = 0.0, sumSq = 0.0;
            for ( size_t texel = 0; texel < texels; ++texel )
                if ( state[texel] == 4 )
                {
                    const double c = std::abs( curvature[texel] );
                    sum += c;
                    sumSq += c * c;
                }
            const double n     = static_cast<double>( out.HitTexels );
            const double mean  = sum / n;
            const double sd    = std::sqrt( std::max( 0.0, sumSq / n - mean * mean ) );
            const double range = settings.CurvatureRangeMultiplier * ( mean + sd );
            for ( size_t texel = 0; texel < texels; ++texel )
                if ( state[texel] != 0 )
                    WriteGrey( &out.CurvatureRgba[texel * 4],
                               range > 0.0 ? 0.5 + 0.5 * std::clamp( curvature[texel] / range, -1.0, 1.0 ) : 0.5 );
        }

        // Gutter: each ring takes the average of its filled 8-neighbours, in every map.
        std::vector<std::vector<uint8_t>*> maps = { &out.Rgba };
        if ( settings.Occlusion )
            maps.push_back( &out.OcclusionRgba );
        if ( settings.Curvature )
            maps.push_back( &out.CurvatureRgba );
        for ( int ring = 0; ring < settings.GutterTexels; ++ring )
        {
            std::vector<size_t> filled;
            for ( int y = 0; y < H; ++y )
                for ( int x = 0; x < W; ++x )
                {
                    const size_t texel = static_cast<size_t>( y ) * W + x;
                    if ( state[texel] != 0 )
                        continue;
                    std::array<size_t, 8> from{};
                    int                   n = 0;
                    for ( int dy = -1; dy <= 1; ++dy )
                        for ( int dx = -1; dx <= 1; ++dx )
                        {
                            const int nx = x + dx, ny = y + dy;
                            if ( nx < 0 || ny < 0 || nx >= W || ny >= H )
                                continue;
                            const size_t nt = static_cast<size_t>( ny ) * W + nx;
                            if ( state[nt] != 0 && state[nt] != 3 )
                                from[static_cast<size_t>( n++ )] = nt;
                        }
                    if ( n == 0 )
                        continue;
                    for ( std::vector<uint8_t>* map : maps )
                    {
                        for ( int c = 0; c < 3; ++c )
                        {
                            int sum = 0;
                            for ( int k = 0; k < n; ++k )
                                sum += ( *map )[from[static_cast<size_t>( k )] * 4 + c];
                            ( *map )[texel * 4 + c] = static_cast<uint8_t>( ( sum + n / 2 ) / n );
                        }
                        ( *map )[texel * 4 + 3] = 255;
                    }
                    state[texel] = 3; // this ring: not a source for its own ring
                    filled.push_back( texel );
                }
            for ( const size_t texel : filled )
                state[texel] = 2;
            out.GutterTexels += filled.size();
        }
        for ( size_t texel = 0; texel < texels; ++texel )
            if ( state[texel] == 0 )
            {
                WriteFlat( &out.Rgba[texel * 4] );
                if ( settings.Occlusion )
                    WriteGrey( &out.OcclusionRgba[texel * 4], 1.0 );
                if ( settings.Curvature )
                    WriteGrey( &out.CurvatureRgba[texel * 4], 0.5 );
            }
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry
