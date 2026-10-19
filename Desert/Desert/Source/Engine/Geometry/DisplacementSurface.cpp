#include "DisplacementSurface.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Desert::Geometry
{
    namespace
    {
        constexpr float kBarycentricScale = static_cast<float>( kBarycentricMax );

        glm::vec4 Fetch( const DisplacementHeightImage::Mip& mip, int x, int y )
        {
            // Repeat addressing: the material sampler a height texture is drawn with tiles (UV transforms scale
            // past 1).
            const int w = static_cast<int>( mip.Width );
            const int h = static_cast<int>( mip.Height );
            x           = ( ( x % w ) + w ) % w;
            y           = ( ( y % h ) + h ) % h;
            return mip.Texels[static_cast<size_t>( y ) * mip.Width + static_cast<size_t>( x )];
        }

        glm::vec4 Bilinear( const DisplacementHeightImage::Mip& mip, const glm::vec2 uv )
        {
            const glm::vec2 p  = uv * glm::vec2( mip.Width, mip.Height ) - 0.5f;
            const glm::vec2 f  = glm::floor( p );
            const glm::vec2 t  = p - f;
            const int       x0 = static_cast<int>( f.x );
            const int       y0 = static_cast<int>( f.y );
            const glm::vec4 a  = glm::mix( Fetch( mip, x0, y0 ), Fetch( mip, x0 + 1, y0 ), t.x );
            const glm::vec4 b  = glm::mix( Fetch( mip, x0, y0 + 1 ), Fetch( mip, x0 + 1, y0 + 1 ), t.x );
            return glm::mix( a, b, t.y );
        }

        // Trilinear over the box chain, as SampleBindless(texture, sampler, uv, lod) with a linear-mip sampler.
        glm::vec4 Trilinear( const DisplacementHeightImage& image, const glm::vec2 uv, const float lod )
        {
            const float     top = static_cast<float>( image.Mips.size() - 1 );
            const float     l   = std::clamp( lod, 0.0f, top );
            const auto      l0  = static_cast<size_t>( std::floor( l ) );
            const size_t    l1  = std::min( l0 + 1, image.Mips.size() - 1 );
            const float     t   = l - static_cast<float>( l0 );
            const glm::vec4 a   = Bilinear( image.Mips[l0], uv );
            if ( l1 == l0 || t == 0.0f )
                return a;
            return glm::mix( a, Bilinear( image.Mips[l1], uv ), t );
        }

        glm::vec2 TransformUV( const glm::vec2 uv, const glm::vec2 offset, const glm::vec2 scale,
                               const float rotation )
        {
            // SurfaceInputs.glslh SurfaceTransformUV.
            const glm::vec2 s  = uv * scale;
            const float     c  = std::cos( rotation );
            const float     sn = std::sin( rotation );
            return glm::vec2( c * s.x + sn * s.y, c * s.y - sn * s.x ) + offset;
        }

        glm::vec3 Weights( const uint32_t barycentrics )
        {
            const float w0 = static_cast<float>( barycentrics & 0xffffu ) / kBarycentricScale;
            const float w1 = static_cast<float>( barycentrics >> 16u ) / kBarycentricScale;
            return { w0, w1, 1.0f - w0 - w1 };
        }

        class SurfaceEvaluator
        {
        public:
            SurfaceEvaluator( const DisplacementTopology& topology, const DisplacementSurfaceSource& source,
                              std::span<const DisplacementSurfaceMaterial> materials )
                 : m_Topology( topology ), m_Source( source ), m_Materials( materials )
            {
            }

            glm::vec2 SourceUV( const uint32_t vertex, const uint32_t uvSet ) const
            {
                if ( uvSet == 0 || m_Source.Streams.empty() )
                    return m_Source.Vertices[vertex].TexCoord;
                return m_Source.Streams[vertex].UV1;
            }

            // DisplacementHeights.shader SampleDisplacement.
            float Sample( const uint32_t triangle, const uint32_t barycentrics, const uint32_t level ) const
            {
                const uint32_t                     submesh  = m_Topology.TriangleSubmesh[triangle];
                const DisplacementSurfaceMaterial& material = m_Materials[submesh];
                if ( !material.Image )
                    return 0.0f;
                const glm::vec3 w   = Weights( barycentrics );
                const uint32_t  v0  = m_Topology.CornerVertex[triangle * 3 + 0];
                const uint32_t  v1  = m_Topology.CornerVertex[triangle * 3 + 1];
                const uint32_t  v2  = m_Topology.CornerVertex[triangle * 3 + 2];
                const glm::vec2 uv0 = SourceUV( v0, material.UVSet );
                const glm::vec2 uv1 = SourceUV( v1, material.UVSet );
                const glm::vec2 uv2 = SourceUV( v2, material.UVSet );
                glm::vec2       uv  = uv0 * w.x + uv1 * w.y + uv2 * w.z;
                if ( material.HasTransform )
                {
                    glm::vec2 tiling = material.Tiling;
                    if ( tiling.x <= 0.0f )
                        tiling.x = 1.0f;
                    if ( tiling.y <= 0.0f )
                        tiling.y = 1.0f;
                    uv = TransformUV( uv, material.Offset, tiling, material.Rotation );
                }

                const glm::vec3& p0      = m_Source.Vertices[v0].Position;
                const glm::vec3& p1      = m_Source.Vertices[v1].Position;
                const glm::vec3& p2      = m_Source.Vertices[v2].Position;
                const float      area    = glm::length( glm::cross( p1 - p0, p2 - p0 ) );
                const glm::vec2  e1      = uv1 - uv0;
                const glm::vec2  e2      = uv2 - uv0;
                const float      uvArea  = std::abs( e1.x * e2.y - e1.y * e2.x );
                const float      uvPerCm = area > 0.0f ? std::sqrt( uvArea / area ) : 0.0f;
                const auto&      mip0    = material.Image->Mips.front();
                const float      edgeCm  = std::ldexp( m_Topology.BaseEdgeCm, -static_cast<int>( level ) );
                const float texels = static_cast<float>( std::max( mip0.Width, mip0.Height ) ) * uvPerCm * edgeCm;
                const float lod    = texels > 1.0f ? std::log2( texels ) : 0.0f;

                const float height =
                     Trilinear( *material.Image, uv, lod )[static_cast<int>( std::min( material.Channel, 3u ) )];
                return ( height - std::clamp( material.Center, 0.0f, 1.0f ) ) * material.ScaleCm;
            }

            glm::vec3 Position( const TessellatedCluster& cluster, const DisplacementPoint& point ) const
            {
                const float b0 = static_cast<float>( point.Weights & 0xffffu ) / kBarycentricScale;
                const float b1 = static_cast<float>( point.Weights >> 16u ) / kBarycentricScale;
                const float b2 = static_cast<float>( kBarycentricMax - ( point.Weights & 0xffffu ) -
                                                     ( point.Weights >> 16u ) ) /
                                 kBarycentricScale;
                const auto&     wp = m_Topology.WeldPosition;
                const auto&     wd = m_Topology.WeldDirection;
                const glm::vec3 position =
                     wp[point.Welds[0]] * b0 + wp[point.Welds[1]] * b1 + wp[point.Welds[2]] * b2;
                const glm::vec3 sum = wd[point.Welds[0]] * b0 + wd[point.Welds[1]] * b1 + wd[point.Welds[2]] * b2;
                const float     length    = glm::length( sum );
                const glm::vec3 direction = length > 0.0f ? sum / length : sum;

                float displacement = 0.0f;
                for ( uint32_t s = 0; s < point.SampleCount; ++s )
                {
                    const DisplacementSample& sample = cluster.Samples[point.SampleOffset + s];
                    displacement += Sample( sample.Triangle, sample.Barycentrics, point.Level );
                }
                displacement /= static_cast<float>( std::max( point.SampleCount, 1u ) );
                return position + direction * displacement;
            }

            // DisplacementEmit.shader main: attributes from the sample triangle at the vertex barycentrics.
            void Emit( const DisplacementVertex& record, const glm::vec3& position, Vertex& out,
                       MeshVertexStreams* streams ) const
            {
                const glm::vec3 w     = Weights( record.Barycentrics );
                const uint32_t  v0    = m_Topology.CornerVertex[record.Triangle * 3 + 0];
                const uint32_t  v1    = m_Topology.CornerVertex[record.Triangle * 3 + 1];
                const uint32_t  v2    = m_Topology.CornerVertex[record.Triangle * 3 + 2];
                const Vertex&   a     = m_Source.Vertices[v0];
                const Vertex&   b     = m_Source.Vertices[v1];
                const Vertex&   c     = m_Source.Vertices[v2];
                const auto      frame = []( const glm::vec3 value )
                {
                    const float length = glm::length( value );
                    return length > 0.0f ? value / length : value;
                };
                out.Position  = position;
                out.Normal    = frame( a.Normal * w.x + b.Normal * w.y + c.Normal * w.z );
                out.Tangent   = frame( a.Tangent * w.x + b.Tangent * w.y + c.Tangent * w.z );
                out.Bitangent = frame( a.Bitangent * w.x + b.Bitangent * w.y + c.Bitangent * w.z );
                out.TexCoord  = a.TexCoord * w.x + b.TexCoord * w.y + c.TexCoord * w.z;
                if ( !streams )
                    return;
                const MeshVertexStreams& sa = m_Source.Streams[v0];
                const MeshVertexStreams& sb = m_Source.Streams[v1];
                const MeshVertexStreams& sc = m_Source.Streams[v2];
                for ( size_t i = 0; i < 4; ++i )
                {
                    // unpackUnorm4x8 -> weighted sum -> packUnorm4x8 (round(clamp(x, 0, 1) * 255)).
                    const float value =
                         ( static_cast<float>( sa.Color[i] ) * w.x + static_cast<float>( sb.Color[i] ) * w.y +
                           static_cast<float>( sc.Color[i] ) * w.z ) /
                         255.0f;
                    streams->Color[i] =
                         static_cast<uint8_t>( std::round( std::clamp( value, 0.0f, 1.0f ) * 255.0f ) );
                }
                streams->UV1 = sa.UV1 * w.x + sb.UV1 * w.y + sc.UV1 * w.z;
            }

        private:
            const DisplacementTopology&                  m_Topology;
            const DisplacementSurfaceSource&             m_Source;
            std::span<const DisplacementSurfaceMaterial> m_Materials;
        };
    } // namespace

    Common::ResultStr<std::shared_ptr<const DisplacementHeightImage>>
    MakeDisplacementHeightImage( const uint32_t width, const uint32_t height, std::vector<glm::vec4> texels )
    {
        using Result = std::shared_ptr<const DisplacementHeightImage>;
        if ( width == 0 || height == 0 || texels.size() != static_cast<size_t>( width ) * height )
            return Common::MakeFormattedError<Result>( "displacement height image: {}x{} with {} texels", width,
                                                       height, texels.size() );
        auto image = std::make_shared<DisplacementHeightImage>();
        image->Mips.push_back( { width, height, std::move( texels ) } );
        while ( image->Mips.back().Width > 1 || image->Mips.back().Height > 1 )
        {
            const DisplacementHeightImage::Mip& src = image->Mips.back();
            DisplacementHeightImage::Mip        dst;
            dst.Width  = std::max( src.Width / 2, 1u );
            dst.Height = std::max( src.Height / 2, 1u );
            dst.Texels.resize( static_cast<size_t>( dst.Width ) * dst.Height );
            for ( uint32_t y = 0; y < dst.Height; ++y )
                for ( uint32_t x = 0; x < dst.Width; ++x )
                {
                    // A 2x2 box; an odd edge (or a 1-texel axis) reuses its last row/column.
                    const uint32_t x0 = std::min( x * 2, src.Width - 1 );
                    const uint32_t x1 = std::min( x * 2 + 1, src.Width - 1 );
                    const uint32_t y0 = std::min( y * 2, src.Height - 1 );
                    const uint32_t y1 = std::min( y * 2 + 1, src.Height - 1 );
                    const auto     at = [&src]( const uint32_t u, const uint32_t v )
                    { return src.Texels[static_cast<size_t>( v ) * src.Width + u]; };
                    dst.Texels[static_cast<size_t>( y ) * dst.Width + x] =
                         ( at( x0, y0 ) + at( x1, y0 ) + at( x0, y1 ) + at( x1, y1 ) ) * 0.25f;
                }
            image->Mips.push_back( std::move( dst ) );
        }
        return Common::MakeSuccess<Result>( std::move( image ) );
    }

    Common::ResultStr<std::vector<uint8_t>>
    DisplacementLevelsForEdge( const DisplacementTopology& topology, const float edgeLengthCm, uint32_t* clamped )
    {
        if ( !( edgeLengthCm > 0.0f ) || !std::isfinite( edgeLengthCm ) )
            return Common::MakeFormattedError<std::vector<uint8_t>>(
                 "displacement levels: the diced edge length must be a positive number of centimetres, got {}",
                 edgeLengthCm );
        std::vector<uint8_t> levels( topology.Clusters.size() );
        const float wanted = std::ceil( std::log2( std::max( topology.BaseEdgeCm / edgeLengthCm, 1.0f ) ) );
        uint32_t    count  = 0;
        for ( size_t c = 0; c < levels.size(); ++c )
        {
            const float top = static_cast<float>( topology.Clusters[c].MaxLevel );
            count += wanted > top ? 1u : 0u;
            levels[c] = static_cast<uint8_t>( std::min( wanted, top ) );
        }
        if ( clamped )
            *clamped = count;
        return Common::MakeSuccess( std::move( levels ) );
    }

    std::vector<uint8_t> DisplacementMaxLevels( const DisplacementTopology& topology )
    {
        std::vector<uint8_t> levels( topology.Clusters.size() );
        for ( size_t c = 0; c < levels.size(); ++c )
            levels[c] = topology.Clusters[c].MaxLevel;
        return levels;
    }

    Common::ResultStr<DisplacedSurface>
    BuildDisplacedSurface( const DisplacementTopology& topology, const DisplacementSurfaceSource& source,
                           std::span<const DisplacementSurfaceMaterial> materials,
                           std::span<const uint8_t>                     levels )
    {
        if ( materials.size() != source.Submeshes.size() )
            return Common::MakeFormattedError<DisplacedSurface>(
                 "displaced surface: {} height materials for {} submeshes", materials.size(),
                 source.Submeshes.size() );
        if ( levels.size() != topology.Clusters.size() )
            return Common::MakeFormattedError<DisplacedSurface>( "displaced surface: {} levels for {} clusters",
                                                                 levels.size(), topology.Clusters.size() );
        if ( !source.Streams.empty() && source.Streams.size() != source.Vertices.size() )
            return Common::MakeFormattedError<DisplacedSurface>(
                 "displaced surface: {} stream vertices for {} vertices", source.Streams.size(),
                 source.Vertices.size() );

        // Gathered per submesh first: the clusters are cooked per submesh, but the surface must not depend on it.
        struct Part
        {
            std::vector<Vertex>            Vertices;
            std::vector<MeshVertexStreams> Streams;
            std::vector<Index>             Triangles;
        };
        std::vector<Part>      parts( source.Submeshes.size() );
        const SurfaceEvaluator evaluator( topology, source, materials );
        const bool             streams = !source.Streams.empty();
        std::vector<glm::vec3> positions;
        for ( uint32_t c = 0; c < topology.Clusters.size(); ++c )
        {
            auto tessellated = TessellateCluster( topology, c, levels );
            if ( !tessellated.IsSuccess() )
                return Common::MakeError<DisplacedSurface>( tessellated.GetError() );
            const TessellatedCluster& cluster = tessellated.GetValue();
            const uint32_t            submesh = topology.Clusters[c].Submesh;
            if ( submesh >= parts.size() )
                return Common::MakeFormattedError<DisplacedSurface>(
                     "displaced surface: cluster {} names submesh {} of {}", c, submesh, parts.size() );
            Part& part = parts[submesh];

            positions.resize( cluster.Points.size() );
            for ( size_t p = 0; p < cluster.Points.size(); ++p )
                positions[p] = evaluator.Position( cluster, cluster.Points[p] );

            const auto base = static_cast<uint32_t>( part.Vertices.size() );
            for ( const DisplacementVertex& record : cluster.Vertices )
            {
                Vertex            vertex{};
                MeshVertexStreams stream{};
                evaluator.Emit( record, positions[record.Point], vertex, streams ? &stream : nullptr );
                part.Vertices.push_back( vertex );
                if ( streams )
                    part.Streams.push_back( stream );
            }
            for ( size_t i = 0; i + 2 < cluster.Indices.size(); i += 3 )
                part.Triangles.push_back(
                     { base + cluster.Indices[i], base + cluster.Indices[i + 1], base + cluster.Indices[i + 2] } );
        }

        DisplacedSurface out;
        for ( size_t s = 0; s < parts.size(); ++s )
        {
            Part&              part = parts[s];
            const Submesh&     from = source.Submeshes[s];
            Submesh            sm{ from.Name,
                        static_cast<uint32_t>( out.Vertices.size() ),
                        static_cast<uint32_t>( part.Vertices.size() ),
                        static_cast<uint32_t>( out.Indices.size() * 3 ),
                        static_cast<uint32_t>( part.Triangles.size() * 3 ),
                        from.Transform,
                                   {} };
            Common::Math::AABB box{ glm::vec3( std::numeric_limits<float>::max() ),
                                    glm::vec3( std::numeric_limits<float>::lowest() ) };
            for ( const Vertex& v : part.Vertices )
            {
                box.Min = glm::min( box.Min, v.Position );
                box.Max = glm::max( box.Max, v.Position );
            }
            sm.BoundingBox = part.Vertices.empty() ? from.BoundingBox : box;
            out.Submeshes.push_back( std::move( sm ) );
            out.Vertices.insert( out.Vertices.end(), part.Vertices.begin(), part.Vertices.end() );
            out.Streams.insert( out.Streams.end(), part.Streams.begin(), part.Streams.end() );
            out.Indices.insert( out.Indices.end(), part.Triangles.begin(), part.Triangles.end() );
        }
        return Common::MakeSuccess( std::move( out ) );
    }
} // namespace Desert::Geometry
