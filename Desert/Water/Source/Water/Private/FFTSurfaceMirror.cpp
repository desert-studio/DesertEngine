#include <Water/Public/FFTSurfaceMirror.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace Desert::Water
{
    namespace
    {
        using BandWeights = std::array<float, kFFTOceanCascades>;

        constexpr int32_t kSide = static_cast<int32_t>( kFFTReadbackSize );

        // One read's displacement of cascade @p cascade at the plane point @p plane, bilinear and periodic over
        // the cascade's patch, whose lattice is turned by kFFTOceanLatticeDegrees: the point is turned into the
        // lattice and the sampled horizontal displacement back. The GPU
        // samples its 256^2 texels with their centres at (i + 0.5) / 256 of the patch; read texel j is GPU texel j * kFFTReadbackStride.
        glm::vec3 CascadeAt( const std::vector<glm::vec4>& texels, const uint32_t cascade, const float patch,
                             const glm::vec2 plane ) noexcept
        {
            const float       turn = glm::radians( kFFTOceanLatticeDegrees[cascade] );
            const float       cosT = std::cos( turn );
            const float       sinT = std::sin( turn );
            const glm::vec2   turned( cosT * plane.x + sinT * plane.y, cosT * plane.y - sinT * plane.x );
            const glm::vec2   gpu  = turned / patch * static_cast<float>( kFFTOceanResolution ) - 0.5f;
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
            const glm::vec3 d = glm::mix( glm::mix( at( x0, z0 ), at( x1, z0 ), f.x ),
                                          glm::mix( at( x0, z1 ), at( x1, z1 ), f.x ), f.y );
            return glm::vec3( cosT * d.x - sinT * d.z, d.y, sinT * d.x + cosT * d.z );
        }

        // The summed cascades' displacement of the still point @p plane at @p time: the newest read, moved along
        // the per-texel velocity of the last two reads (linear extrapolation in time, held past the horizon).
        glm::vec3 DisplacementAt( const FFTSurfaceSnapshot& snapshot, const glm::vec2 plane,
                                  const double time, const BandWeights& weights ) noexcept
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
                    sum += now * weights[c];
                    continue;
                }
                const glm::vec3 before = CascadeAt( snapshot.Previous.Texels, c, sizes[c], plane );
                sum += ( now + ( now - before ) * share ) * weights[c];
            }
            return sum;
        }

    } // namespace

    glm::vec3 FFTDisplacementAt( const FFTSurfaceSnapshot& snapshot, const glm::vec2 plane, const double time,
                                 const BandWeights& bandWeights ) noexcept
    {
        return DisplacementAt( snapshot, plane, time, bandWeights );
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
