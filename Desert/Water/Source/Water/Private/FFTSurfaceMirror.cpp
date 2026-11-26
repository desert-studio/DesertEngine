#include <Water/Public/FFTSurfaceMirror.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace Desert::Water
{
    namespace
    {
        constexpr int32_t kSide = static_cast<int32_t>( kFFTReadbackSize );

        // One read's displacement of cascade @p cascade at the plane point @p plane, bilinear and periodic over
        // the cascade's patch. The GPU samples its 256^2 texels with their centres at (i + 0.5) / 256 of the
        // patch; read texel j is GPU texel j * kFFTReadbackStride.
        glm::vec3 CascadeAt( const std::vector<glm::vec4>& texels, const uint32_t cascade, const float patch,
                             const glm::vec2 plane ) noexcept
        {
            const glm::vec2   gpu  = plane / patch * static_cast<float>( kFFTOceanResolution ) - 0.5f;
            const glm::vec2   q    = gpu / static_cast<float>( kFFTReadbackStride );
            const glm::vec2   lo   = glm::floor( q );
            const glm::vec2   f    = q - lo;
            const auto        wrap = []( const int32_t i ) { return ( ( i % kSide ) + kSide ) % kSide; };
            const int32_t     x0   = wrap( static_cast<int32_t>( lo.x ) );
            const int32_t     z0   = wrap( static_cast<int32_t>( lo.y ) );
            const int32_t     x1   = wrap( x0 + 1 );
            const int32_t     z1   = wrap( z0 + 1 );
            const std::size_t base = static_cast<std::size_t>( cascade ) * kSide * kSide;
            const auto        at   = [&]( const int32_t x, const int32_t z )
            { return glm::vec3( texels[base + static_cast<std::size_t>( z ) * kSide + x] ); };
            return glm::mix( glm::mix( at( x0, z0 ), at( x1, z0 ), f.x ),
                             glm::mix( at( x0, z1 ), at( x1, z1 ), f.x ), f.y );
        }

        // The summed cascades' displacement of the still point @p plane at @p time: the newest read, moved along
        // the per-texel velocity of the last two reads (linear extrapolation in time, held past the horizon).
        glm::vec3 DisplacementAt( const FFTSurfaceSnapshot& snapshot, const glm::vec2 plane,
                                  const double time ) noexcept
        {
            const auto& sizes  = snapshot.Spectrum->Params.CascadeSizes;
            const bool  moving = snapshot.Previous.Texels.size() == snapshot.Current.Texels.size() &&
                                snapshot.Current.Time > snapshot.Previous.Time;
            const double ahead = std::clamp( time - snapshot.Current.Time, -kFFTMirrorMaxExtrapolationSeconds,
                                             kFFTMirrorMaxExtrapolationSeconds );
            const float  share =
                 moving ? static_cast<float>( ahead / ( snapshot.Current.Time - snapshot.Previous.Time ) ) : 0.0f;
            glm::vec3 sum( 0.0f );
            for ( uint32_t c = 0; c < kFFTOceanCascades; ++c )
            {
                const glm::vec3 now = CascadeAt( snapshot.Current.Texels, c, sizes[c], plane );
                if ( !moving )
                {
                    sum += now;
                    continue;
                }
                const glm::vec3 before = CascadeAt( snapshot.Previous.Texels, c, sizes[c], plane );
                sum += now + ( now - before ) * share;
            }
            return sum;
        }

        // The displaced surface's height over the world plane point @p plane: the still point x0 with
        // x0 + D_xz(x0) = plane found by fixed-point iteration (choppiness < 1 contracts it), then D_y(x0).
        float HeightAt( const FFTSurfaceSnapshot& snapshot, const glm::vec2 plane, const double time ) noexcept
        {
            glm::vec2 still = plane;
            glm::vec3 d     = DisplacementAt( snapshot, still, time );
            for ( int i = 0; i < 4; ++i )
            {
                still = plane - glm::vec2( d.x, d.z );
                d     = DisplacementAt( snapshot, still, time );
            }
            return d.y;
        }
    } // namespace

    std::optional<FFTSurfaceSample> FFTSurfaceAt( const FFTSurfaceSnapshot& snapshot, const glm::vec2 plane,
                                                  const double time ) noexcept
    {
        if ( snapshot.Spectrum == nullptr || snapshot.Current.Texels.size() != kFFTReadbackTexels )
            return std::nullopt;
        FFTSurfaceSample sample;
        sample.Height = HeightAt( snapshot, plane, time );
        // The normal from central differences half a read texel of the finest cascade apart (the finest detail
        // the read holds).
        const float e = 0.5f * snapshot.Spectrum->Params.CascadeSizes[kFFTOceanCascades - 1] /
                        static_cast<float>( kFFTReadbackSize );
        const float hx = HeightAt( snapshot, plane + glm::vec2( e, 0.0f ), time ) -
                         HeightAt( snapshot, plane - glm::vec2( e, 0.0f ), time );
        const float hz = HeightAt( snapshot, plane + glm::vec2( 0.0f, e ), time ) -
                         HeightAt( snapshot, plane - glm::vec2( 0.0f, e ), time );
        const glm::vec3 n( -hx, 2.0f * e, -hz );
        sample.Normal = glm::normalize( n );
        return sample;
    }

    void FFTSurfaceMirror::Publish( const std::shared_ptr<const FFTOceanSpectrum>& spectrum,
                                    FFTSurfaceRead                                 read ) noexcept
    {
        if ( spectrum == nullptr || read.Texels.size() != kFFTReadbackTexels )
            return;
        auto                   next = std::make_shared<FFTSurfaceSnapshot>();
        const std::scoped_lock lock( m_Mutex );
        next->Spectrum = spectrum;
        // The previous newest read is this ocean's only while it is the same sea at an earlier time.
        if ( m_Snapshot != nullptr && m_Snapshot->Spectrum == spectrum && m_Snapshot->Current.Time < read.Time )
            next->Previous = m_Snapshot->Current;
        next->Current = std::move( read );
        m_Snapshot    = std::move( next );
    }

    std::shared_ptr<const FFTSurfaceSnapshot>
    FFTSurfaceMirror::Latest( const FFTOceanSpectrum* spectrum ) const noexcept
    {
        const std::scoped_lock lock( m_Mutex );
        if ( m_Snapshot == nullptr || m_Snapshot->Spectrum.get() != spectrum )
            return nullptr;
        return m_Snapshot;
    }

    void FFTSurfaceMirror::Clear() noexcept
    {
        const std::scoped_lock lock( m_Mutex );
        m_Snapshot.reset();
    }
} // namespace Desert::Water
