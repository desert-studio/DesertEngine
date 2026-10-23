// Ported from UE 5.8 Runtime/Landscape: LandscapeSplineSegment.cpp (UpdateSplineMeshes: the placement along
// the segment) and Runtime/Engine SplineMeshComponent.cpp / SplineMeshShaderParams (the per-vertex bend),
// done on the CPU once per edit: a landscape's spline meshes change only when their splines do.
#include "LandscapeSplineMeshes.hpp"

#include <algorithm>
#include <cmath>

namespace Desert::World::Landscape
{
    namespace
    {
        /// The mesh's local axes for a forward axis: forward, up (engine +Y; +Z when forward is Y) and side
        /// (forward x up, the axis laid along the curve's right).
        struct MeshAxes
        {
            int       Forward = 0;
            int       Up      = 1;
            glm::vec3 Side    = glm::vec3( 0.0f, 0.0f, 1.0f );
        };

        MeshAxes AxesFor( LandscapeSplineMeshAxis axis )
        {
            MeshAxes  axes;
            glm::vec3 forward( 0.0f ), up( 0.0f );
            axes.Forward          = static_cast<int>( axis );
            axes.Up               = axis == LandscapeSplineMeshAxis::Y ? 2 : 1;
            forward[axes.Forward] = 1.0f;
            up[axes.Up]           = 1.0f;
            axes.Side             = glm::cross( forward, up );
            return axes;
        }

        /// The curve's arc length, tabulated: distance -> curve parameter.
        struct ArcLength
        {
            static constexpr size_t kSamples = 256u;
            std::vector<float>      Distance; // Distance[i] = arc length up to t = i / (kSamples - 1)

            explicit ArcLength( const LandscapeSplineCurve& curve ) : Distance( kSamples, 0.0f )
            {
                glm::vec3 previous = curve.Position( 0.0f );
                for ( size_t i = 1; i < kSamples; ++i )
                {
                    const glm::vec3 p = curve.Position( static_cast<float>( i ) / static_cast<float>( kSamples - 1u ) );
                    Distance[i]       = Distance[i - 1u] + glm::length( p - previous );
                    previous          = p;
                }
            }

            [[nodiscard]] float Total() const { return Distance.back(); }

            [[nodiscard]] float ParameterAt( float distance ) const
            {
                distance = std::clamp( distance, 0.0f, Total() );
                const auto   upper = std::lower_bound( Distance.begin(), Distance.end(), distance );
                const size_t i     = static_cast<size_t>( std::distance( Distance.begin(), upper ) );
                if ( i == 0u )
                    return 0.0f;
                if ( i >= kSamples )
                    return 1.0f;
                const float span = Distance[i] - Distance[i - 1u];
                const float f    = span > 0.0f ? ( distance - Distance[i - 1u] ) / span : 0.0f;
                return ( static_cast<float>( i - 1u ) + f ) / static_cast<float>( kSamples - 1u );
            }
        };

        struct SourceExtent
        {
            float ForwardMin = 0.0f, ForwardMax = 0.0f, SideCentre = 0.0f;
            [[nodiscard]] float Length() const { return ForwardMax - ForwardMin; }
        };

        SourceExtent ExtentOf( const LandscapeSplineMeshEntry& entry, const MeshAxes& axes,
                               const LandscapeSplineMeshSource& source )
        {
            SourceExtent extent;
            float        sideMin = 0.0f, sideMax = 0.0f;
            for ( size_t i = 0; i < source.Vertices.size(); ++i )
            {
                const glm::vec3 p    = source.Vertices[i].Position * entry.Scale;
                const float     f    = p[axes.Forward];
                const float     side = glm::dot( p, axes.Side );
                extent.ForwardMin    = i == 0u ? f : std::min( extent.ForwardMin, f );
                extent.ForwardMax    = i == 0u ? f : std::max( extent.ForwardMax, f );
                sideMin              = i == 0u ? side : std::min( sideMin, side );
                sideMax              = i == 0u ? side : std::max( sideMax, side );
            }
            extent.SideCentre = entry.CenterH ? 0.5f * ( sideMin + sideMax ) : 0.0f;
            return extent;
        }

        struct Piece
        {
            size_t Entry = 0u;
            float  From = 0.0f, To = 0.0f; // arc length along the segment
        };

        /// UE UpdateSplineMeshes: cycle through the entries while the next one's half fits, then stretch all.
        std::vector<Piece> PlacePieces( const std::vector<SourceExtent>& extents, float length )
        {
            std::vector<Piece> pieces;
            const bool         anyLength =
                 std::any_of( extents.begin(), extents.end(), []( const SourceExtent& e ) { return e.Length() > 0.0f; } );
            if ( !anyLength || length <= 0.0f )
                return pieces;
            float placed = 0.0f;
            for ( size_t k = 0;; k = ( k + 1u ) % extents.size() )
            {
                const float l = extents[k].Length();
                if ( l <= 0.0f )
                    continue;
                if ( !pieces.empty() && placed + 0.5f * l > length )
                    break;
                pieces.push_back( { k, placed, placed + l } );
                placed += l;
            }
            const float stretch = length / placed;
            for ( Piece& p : pieces )
            {
                p.From *= stretch;
                p.To *= stretch;
            }
            pieces.back().To = length; // exactly the end, whatever the rounding
            return pieces;
        }
    } // namespace

    std::vector<LandscapeSplineDeformedMesh> BuildLandscapeSplineSegmentMeshes(
         const LandscapeSplines& splines, size_t segment, std::span<const LandscapeSplineMeshSource> sources )
    {
        const LandscapeSplineSegment&            s = splines.Segments[segment];
        std::vector<LandscapeSplineDeformedMesh> out( s.SplineMeshes.size() );
        if ( sources.size() != s.SplineMeshes.size() || s.SplineMeshes.empty() )
            return out;

        const LandscapeSplineCurve curve = LandscapeSplineSegmentCurve( splines, segment );
        const ArcLength            arc( curve );
        std::vector<MeshAxes>      axes;
        std::vector<SourceExtent>  extents;
        for ( size_t e = 0; e < s.SplineMeshes.size(); ++e )
        {
            axes.push_back( AxesFor( s.SplineMeshes[e].ForwardAxis ) );
            extents.push_back( ExtentOf( s.SplineMeshes[e], axes.back(), sources[e] ) );
        }
        const std::vector<Piece> pieces = PlacePieces( extents, arc.Total() );

        for ( size_t e = 0; e < s.SplineMeshes.size(); ++e )
        {
            const LandscapeSplineMeshEntry&  entry  = s.SplineMeshes[e];
            const LandscapeSplineMeshSource& source = sources[e];
            const MeshAxes&                  ax     = axes[e];
            const SourceExtent&              extent = extents[e];
            LandscapeSplineDeformedMesh&     mesh   = out[e];
            if ( extent.Length() <= 0.0f )
                continue;

            for ( const Submesh& from : source.Submeshes )
            {
                Submesh to;
                to.Name         = from.Name;
                to.Transform    = glm::mat4( 1.0f );
                to.VertexOffset = static_cast<uint32_t>( mesh.Vertices.size() );
                to.IndexOffset  = static_cast<uint32_t>( mesh.Indices.size() * 3u );
                for ( const Piece& piece : pieces )
                {
                    if ( piece.Entry != e )
                        continue;
                    const uint32_t base = static_cast<uint32_t>( mesh.Vertices.size() ) - to.VertexOffset;
                    for ( uint32_t v = 0; v < from.VertexCount; ++v )
                    {
                        const Vertex&   src   = source.Vertices[from.VertexOffset + v];
                        const glm::vec3 p     = src.Position * entry.Scale;
                        const float     along = ( p[ax.Forward] - extent.ForwardMin ) / extent.Length();
                        const float     t     = arc.ParameterAt( glm::mix( piece.From, piece.To, along ) );
                        const glm::vec3 d     = curve.Derivative( t );
                        const float     dl    = glm::length( d );
                        const glm::vec3 forward = dl > 0.0f ? d / dl : glm::vec3( 1.0f, 0.0f, 0.0f );
                        const glm::vec3 right   = curve.Right( t );
                        const glm::vec3 up      = glm::normalize( glm::cross( right, forward ) );
                        // Local (forward, up, side) -> world; directions keep their scale, normals invert it.
                        const auto toWorld = [&]( const glm::vec3& local )
                        {
                            return forward * local[ax.Forward] + up * local[ax.Up] +
                                   right * glm::dot( local, ax.Side );
                        };
                        const auto direction = [&]( const glm::vec3& local )
                        {
                            const glm::vec3 w = toWorld( local );
                            const float     l = glm::length( w );
                            return l > 0.0f ? w / l : w;
                        };

                        Vertex bent = src;
                        bent.Position = curve.Position( t ) + up * ( p[ax.Up] + entry.CenterAdjust.y ) +
                                       right * ( glm::dot( p, ax.Side ) - extent.SideCentre + entry.CenterAdjust.x );
                        bent.Normal    = direction( src.Normal / entry.Scale );
                        bent.Tangent   = direction( src.Tangent * entry.Scale );
                        bent.Bitangent = direction( src.Bitangent * entry.Scale );
                        mesh.Vertices.push_back( bent );
                    }
                    const size_t firstTriangle = from.IndexOffset / 3u;
                    for ( size_t i = 0; i < from.IndexCount / 3u; ++i )
                    {
                        const Index& tri = source.Indices[firstTriangle + i];
                        mesh.Indices.push_back( { tri.V1 + base, tri.V2 + base, tri.V3 + base } );
                    }
                }
                to.VertexCount = static_cast<uint32_t>( mesh.Vertices.size() ) - to.VertexOffset;
                to.IndexCount  = static_cast<uint32_t>( mesh.Indices.size() * 3u ) - to.IndexOffset;
                glm::vec3 lo( 0.0f ), hi( 0.0f );
                for ( uint32_t i = 0; i < to.VertexCount; ++i )
                {
                    const glm::vec3& p = mesh.Vertices[to.VertexOffset + i].Position;
                    lo                 = i == 0u ? p : glm::min( lo, p );
                    hi                 = i == 0u ? p : glm::max( hi, p );
                }
                to.BoundingBox.Min = lo;
                to.BoundingBox.Max = hi;
                mesh.Submeshes.push_back( std::move( to ) );
            }
        }
        return out;
    }
} // namespace Desert::World::Landscape
