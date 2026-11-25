#include <Water/Public/WaterShoreField.hpp>

#include <LandscapeCore/Public/LandscapeData.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>

namespace Desert::Water
{
    namespace
    {
        constexpr double  kNoFeature = 1.0e12; // squared texels: no such grid
        constexpr int32_t kNoIndex   = -1;
        constexpr float   kBeachCm   = 15000.0f; // the near water the beach slope is the mean of

        // The exact Euclidean distance transform of a binary grid with the index of the nearest feature
        // (Felzenszwalb & Huttenlocher 2012: the lower envelope of parabolas, the columns and then the rows).
        class DistanceTransform
        {
        public:
            DistanceTransform( const uint32_t width, const uint32_t height )
                 : m_Width( static_cast<int32_t>( width ) ), m_Height( static_cast<int32_t>( height ) ),
                   m_SquaredTexels( static_cast<std::size_t>( width ) * height ),
                   m_Nearest( static_cast<std::size_t>( width ) * height, kNoIndex )
            {
            }

            template <typename Feature>
            void Run( const Feature& isFeature )
            {
                const auto           n = static_cast<std::size_t>( std::max( m_Width, m_Height ) );
                std::vector<double>  f( n );
                std::vector<double>  d( n );
                std::vector<double>  z( n + 1 );
                std::vector<int32_t> v( n );
                std::vector<int32_t> arg( n );
                std::vector<int32_t> columnRow( m_Nearest.size(), kNoIndex );
                for ( int32_t x = 0; x < m_Width; ++x )
                {
                    for ( int32_t y = 0; y < m_Height; ++y )
                        f[y] = isFeature( static_cast<std::size_t>( At( x, y ) ) ) ? 0.0 : kNoFeature;
                    Line( f, d, arg, v, z, m_Height );
                    for ( int32_t y = 0; y < m_Height; ++y )
                    {
                        m_SquaredTexels[At( x, y )] = d[y];
                        columnRow[At( x, y )]       = arg[y];
                    }
                }
                for ( int32_t y = 0; y < m_Height; ++y )
                {
                    for ( int32_t x = 0; x < m_Width; ++x )
                        f[x] = m_SquaredTexels[At( x, y )];
                    Line( f, d, arg, v, z, m_Width );
                    for ( int32_t x = 0; x < m_Width; ++x )
                    {
                        const bool found            = d[x] < 0.5 * kNoFeature;
                        m_SquaredTexels[At( x, y )] = d[x];
                        m_Nearest[At( x, y )]       = found ? At( arg[x], columnRow[At( arg[x], y )] ) : kNoIndex;
                    }
                }
            }

            // The nearest feature's texel, or kNoIndex on a grid with none.
            [[nodiscard]] int32_t Nearest( const std::size_t i ) const
            {
                return m_Nearest[i];
            }

            [[nodiscard]] float DistanceTexels( const std::size_t i ) const
            {
                return static_cast<float>( std::sqrt( m_SquaredTexels[i] ) );
            }

        private:
            [[nodiscard]] int32_t At( const int32_t x, const int32_t y ) const
            {
                return y * m_Width + x;
            }

            static void Line( const std::vector<double>& f, std::vector<double>& d, std::vector<int32_t>& arg,
                              std::vector<int32_t>& v, std::vector<double>& z, const int32_t n )
            {
                int32_t k = 0;
                v[0]      = 0;
                z[0]      = -std::numeric_limits<double>::infinity();
                z[1]      = std::numeric_limits<double>::infinity();
                for ( int32_t q = 1; q < n; ++q )
                {
                    const auto meet = [&]( const int32_t p ) {
                        return ( ( f[q] + double( q ) * q ) - ( f[p] + double( p ) * p ) ) /
                               ( 2.0 * ( double( q ) - p ) );
                    };
                    double s = meet( v[k] );
                    while ( k > 0 && s <= z[k] )
                        s = meet( v[--k] );
                    ++k;
                    v[k]     = q;
                    z[k]     = s;
                    z[k + 1] = std::numeric_limits<double>::infinity();
                }
                k = 0;
                for ( int32_t q = 0; q < n; ++q )
                {
                    while ( z[k + 1] < q )
                        ++k;
                    const double along = double( q ) - v[k];
                    d[q]               = along * along + f[v[k]];
                    arg[q]             = v[k];
                }
            }

            int32_t              m_Width  = 0;
            int32_t              m_Height = 0;
            std::vector<double>  m_SquaredTexels;
            std::vector<int32_t> m_Nearest;
        };

        float RelativeClamp( const float heightCm, const float referenceCm )
        {
            return std::clamp( heightCm, referenceCm - kShoreMaxRelativeCm, referenceCm + kShoreMaxRelativeCm );
        }

        void Grow( WaterShoreField::Rect& rect, const uint32_t x, const uint32_t z )
        {
            if ( rect.Empty() )
                rect = { x, z, x + 1u, z + 1u };
            else
                rect = { std::min( rect.X0, x ), std::min( rect.Z0, z ), std::max( rect.X1, x + 1u ),
                         std::max( rect.Z1, z + 1u ) };
        }

        // The GPU's texels, the bake's number and what differs from the field it replaces.
        void Finish( WaterShoreField& field, const WaterShoreField* base )
        {
            static std::atomic<uint64_t> s_Last{ 0u };
            field.Revision = s_Last.fetch_add( 1u, std::memory_order_relaxed ) + 1u;
            field.Packed.reserve( field.Texels.size() );
            for ( const WaterShoreField::Texel& texel : field.Texels )
                field.Packed.push_back( PackShoreTexel( texel, field.ReferenceLevelCm ) );
            if ( base == nullptr || base->Width != field.Width || base->Height != field.Height ||
                 base->MinCm != field.MinCm || base->TexelCm != field.TexelCm ||
                 base->ReferenceLevelCm != field.ReferenceLevelCm )
                return;
            field.BaseRevision = base->Revision;
            for ( uint32_t z = 0; z < field.Height; ++z )
                for ( uint32_t x = 0; x < field.Width; ++x )
                {
                    const std::size_t i = static_cast<std::size_t>( z ) * field.Width + x;
                    if ( field.Packed[i] != base->Packed[i] )
                        Grow( field.Changed, x, z );
                    if ( field.Texels[i].GroundCm != base->Texels[i].GroundCm )
                        Grow( field.ChangedGround, x, z );
                }
        }

        struct Bounds
        {
            glm::vec2 Min{ std::numeric_limits<float>::max() };
            glm::vec2 Max{ std::numeric_limits<float>::lowest() };

            void Add( const glm::vec2 lo, const glm::vec2 hi )
            {
                Min = glm::min( Min, lo );
                Max = glm::max( Max, hi );
            }
        };

        Bounds TileBounds( const World::Landscape::LandscapeRayTile& tile )
        {
            Bounds b;
            b.Min = glm::vec2( tile.Frame.OriginX, tile.Frame.OriginZ );
            b.Max = b.Min + glm::vec2( static_cast<float>( tile.Heights->SamplesX() - 1u ),
                                       static_cast<float>( tile.Heights->SamplesZ() - 1u ) ) *
                                 tile.Frame.SpacingCm;
            return b;
        }

        // The still level over a point: of the bodies whose footprint holds it the highest, else the reference.
        float LevelAt( const std::span<const WaterBodyLevel> bodies, const float reference, const glm::vec2 at )
        {
            std::optional<float> level;
            for ( const WaterBodyLevel& body : bodies )
                if ( body.HasShore && at.x >= body.FootprintMinMax.x && at.y >= body.FootprintMinMax.y &&
                     at.x <= body.FootprintMinMax.z && at.y <= body.FootprintMinMax.w )
                    level = level ? std::max( *level, body.StillLevelCm ) : body.StillLevelCm;
            return level.value_or( reference );
        }

        void BakeGround( WaterShoreField& field, const std::span<const World::Landscape::LandscapeRayTile> ground )
        {
            std::vector<uint8_t> covered( field.Texels.size(), 0u );
            for ( const World::Landscape::LandscapeRayTile& tile : ground )
            {
                if ( tile.Heights == nullptr )
                    continue;
                const Bounds tileBounds = TileBounds( tile );
                const auto   texelOf    = [&]( const float at, const float low, const uint32_t count )
                {
                    return static_cast<int32_t>(
                         std::clamp( std::floor( ( at - low ) / field.TexelCm ), -1.0f, float( count ) ) );
                };
                const int32_t x0 = std::max( 0, texelOf( tileBounds.Min.x, field.MinCm.x, field.Width ) );
                const int32_t z0 = std::max( 0, texelOf( tileBounds.Min.y, field.MinCm.y, field.Height ) );
                const int32_t x1 = std::min( static_cast<int32_t>( field.Width ) - 1,
                                             texelOf( tileBounds.Max.x, field.MinCm.x, field.Width ) );
                const int32_t z1 = std::min( static_cast<int32_t>( field.Height ) - 1,
                                             texelOf( tileBounds.Max.y, field.MinCm.y, field.Height ) );
                for ( int32_t z = z0; z <= z1; ++z )
                    for ( int32_t x = x0; x <= x1; ++x )
                    {
                        const std::size_t i = static_cast<std::size_t>( z ) * field.Width + x;
                        if ( covered[i] != 0u )
                            continue; // neighbouring tiles share their seam samples
                        const glm::vec2 at = field.MinCm + ( glm::vec2( x, z ) + 0.5f ) * field.TexelCm;
                        if ( const std::optional<float> height = World::Landscape::SampleLandscapeHeight(
                                  *tile.Heights, tile.Frame, at.x, at.y ) )
                        {
                            field.Texels[i].GroundCm = *height;
                            covered[i]               = 1u;
                        }
                    }
            }
            // A height the texture cannot hold is the farthest one it can (a 650 m cliff is no water's floor).
            const auto clampGround = [&field]
            {
                for ( WaterShoreField::Texel& texel : field.Texels )
                    texel.GroundCm = RelativeClamp( texel.GroundCm, field.ReferenceLevelCm );
            };
            if ( std::find( covered.begin(), covered.end(), uint8_t( 0u ) ) == covered.end() )
            {
                clampGround();
                return;
            }
            // A gap between tiles is the skirt of the nearest landscape, as outside the whole field.
            DistanceTransform nearest( field.Width, field.Height );
            nearest.Run( [&]( const std::size_t i ) { return covered[i] != 0u; } );
            for ( std::size_t i = 0; i < field.Texels.size(); ++i )
            {
                if ( covered[i] != 0u )
                    continue;
                const int32_t from       = nearest.Nearest( i );
                field.Texels[i].GroundCm = field.OpenSeaFloorCm;
                if ( from == kNoIndex )
                    continue;
                const float edge = field.Texels[from].GroundCm;
                field.Texels[i].GroundCm =
                     std::max( edge - kShoreSkirtSlope * nearest.DistanceTexels( i ) * field.TexelCm,
                               std::min( edge, field.OpenSeaFloorCm ) );
            }
            clampGround();
        }

        void BakeShore( WaterShoreField& field, const std::span<const WaterBodyLevel> bodies )
        {
            std::vector<uint8_t> water( field.Texels.size() );
            std::vector<float>   depth( field.Texels.size() );
            for ( uint32_t z = 0; z < field.Height; ++z )
                for ( uint32_t x = 0; x < field.Width; ++x )
                {
                    const std::size_t i  = static_cast<std::size_t>( z ) * field.Width + x;
                    const glm::vec2   at = field.MinCm + ( glm::vec2( x, z ) + 0.5f ) * field.TexelCm;
                    depth[i] = LevelAt( bodies, field.ReferenceLevelCm, at ) - field.Texels[i].GroundCm;
                    water[i] = depth[i] > 0.0f ? 1u : 0u;
                }
            DistanceTransform toLand( field.Width, field.Height );
            DistanceTransform toWater( field.Width, field.Height );
            toLand.Run( [&]( const std::size_t i ) { return water[i] == 0u; } );
            toWater.Run( [&]( const std::size_t i ) { return water[i] != 0u; } );

            double slopes  = 0.0;
            double samples = 0.0;
            for ( std::size_t i = 0; i < field.Texels.size(); ++i )
            {
                const DistanceTransform& other = water[i] != 0u ? toLand : toWater;
                const float              sign  = water[i] != 0u ? 1.0f : -1.0f;
                const int32_t            from  = other.Nearest( i );
                WaterShoreField::Texel&  texel = field.Texels[i];
                texel.ShoreDistanceCm          = sign * kShoreFarCm;
                texel.ShoreDirX                = 0.0f;
                texel.ShoreDirZ                = 0.0f;
                if ( from == kNoIndex )
                    continue;
                const float shore = ( other.DistanceTexels( i ) - 0.5f ) * field.TexelCm;
                if ( shore >= kShoreFarCm )
                    continue;
                texel.ShoreDistanceCm = sign * shore;
                const auto      width = static_cast<int32_t>( field.Width );
                const auto      here  = static_cast<int32_t>( i );
                const glm::vec2 to( static_cast<float>( from % width - here % width ),
                                    static_cast<float>( from / width - here / width ) );
                const glm::vec2 landward = sign * to / glm::length( to );
                texel.ShoreDirX          = landward.x;
                texel.ShoreDirZ          = landward.y;
                if ( water[i] != 0u && shore < kBeachCm )
                {
                    slopes += depth[i] / std::max( shore, 0.5f * field.TexelCm );
                    samples += 1.0;
                }
            }
            field.BeachSlope =
                 samples > 0.0 ? std::clamp( static_cast<float>( slopes / samples ), 0.01f, 0.3f ) : 0.0f;
        }
    } // namespace

    Common::ResultStr<std::shared_ptr<const WaterShoreField>>
    BakeWaterShoreField( const std::span<const World::Landscape::LandscapeRayTile> ground,
                         const std::span<const WaterBodyLevel> bodies, const float spacingCm,
                         const WaterShoreField* const base )
    {
        using Field = std::shared_ptr<const WaterShoreField>;
        if ( bodies.empty() )
            return Common::MakeError<Field>( "water shore field: no body" );
        bool anyGround = false;
        for ( const World::Landscape::LandscapeRayTile& tile : ground )
            anyGround = anyGround || tile.Heights != nullptr;
        if ( anyGround && !( spacingCm > 0.0f ) )
            return Common::MakeError<Field>( "water shore field: the landscape spacing must be above 0" );

        auto   field = std::make_shared<WaterShoreField>();
        Bounds bodyBounds;
        float  lowest = std::numeric_limits<float>::max();
        float  widest = -1.0f;
        bool anyShore = false;
        for ( const WaterBodyLevel& body : bodies )
        {
            const glm::vec2 low( body.FootprintMinMax.x, body.FootprintMinMax.y );
            const glm::vec2 high( body.FootprintMinMax.z, body.FootprintMinMax.w );
            bodyBounds.Add( low, high );
            lowest           = std::min( lowest, body.StillLevelCm );
            const float area = ( high.x - low.x ) * ( high.y - low.y );
            // The widest standing water; of a world of rivers alone, the widest river.
            if ( ( body.HasShore && !anyShore ) || ( body.HasShore == anyShore && area > widest ) )
            {
                anyShore                = body.HasShore;
                widest                  = area;
                field->ReferenceLevelCm = body.StillLevelCm;
            }
        }
        field->OpenSeaFloorCm = RelativeClamp( lowest - kOpenSeaDepthCm, field->ReferenceLevelCm );

        Bounds landscape;
        for ( const World::Landscape::LandscapeRayTile& tile : ground )
            if ( tile.Heights != nullptr )
            {
                const Bounds b = TileBounds( tile );
                landscape.Add( b.Min, b.Max );
            }
        const glm::vec2 lo = glm::max( landscape.Min, bodyBounds.Min );
        const glm::vec2 hi = glm::min( landscape.Max, bodyBounds.Max );
        if ( !anyGround || lo.x >= hi.x || lo.y >= hi.y )
        {
            field->MinCm   = bodyBounds.Min;
            field->TexelCm = 100.0f;
            field->Width   = 1u;
            field->Height  = 1u;
            field->Texels.push_back( { field->OpenSeaFloorCm, kShoreFarCm, 0.0f, 0.0f } );
            Finish( *field, base );
            return Common::MakeSuccess( Field( std::move( field ) ) );
        }
        const glm::vec2 extent = hi - lo;
        field->MinCm           = lo;
        field->TexelCm =
             std::max( spacingCm, std::max( extent.x, extent.y ) / static_cast<float>( kShoreFieldMaxTexels ) );
        const auto count = [&]( const float side ) {
            return std::clamp( static_cast<uint32_t>( std::ceil( side / field->TexelCm ) ), 1u,
                               kShoreFieldMaxTexels );
        };
        field->Width  = count( extent.x );
        field->Height = count( extent.y );
        field->Texels.resize( static_cast<std::size_t>( field->Width ) * field->Height );
        BakeGround( *field, ground );
        BakeShore( *field, bodies );
        Finish( *field, base );
        return Common::MakeSuccess( Field( std::move( field ) ) );
    }

    uint64_t PackShoreTexel( const WaterShoreField::Texel& texel, const float referenceLevelCm ) noexcept
    {
        return glm::packHalf4x16( glm::vec4( texel.GroundCm - referenceLevelCm, texel.ShoreDistanceCm,
                                             texel.ShoreDirX, texel.ShoreDirZ ) );
    }

    WaterShoreSample SampleWaterShoreField( const WaterShoreField& field, const glm::vec2 worldXZ ) noexcept
    {
        const glm::vec2 size( static_cast<float>( field.Width ), static_cast<float>( field.Height ) );
        const glm::vec2 inside =
             glm::clamp( ( worldXZ - field.MinCm ) / field.TexelCm - 0.5f, glm::vec2( 0.0f ), size - 1.0f );
        const glm::vec2 low = glm::floor( inside );
        const glm::vec2 f   = inside - low;
        const auto      x0  = static_cast<uint32_t>( low.x );
        const auto      z0  = static_cast<uint32_t>( low.y );
        const uint32_t  x1  = std::min( x0 + 1u, field.Width - 1u );
        const uint32_t  z1  = std::min( z0 + 1u, field.Height - 1u );
        const auto      at  = [&]( const uint32_t x, const uint32_t z )
        {
            const WaterShoreField::Texel& t = field.Texels[static_cast<std::size_t>( z ) * field.Width + x];
            return glm::vec4( t.GroundCm, t.ShoreDistanceCm, t.ShoreDirX, t.ShoreDirZ );
        };
        const glm::vec4 t = glm::mix( glm::mix( at( x0, z0 ), at( x1, z0 ), f.x ),
                                      glm::mix( at( x0, z1 ), at( x1, z1 ), f.x ), f.y );
        // Past the field the floor falls by the skirt and the shore recedes (WaterDepthField.glslh WaterShoreAt).
        const float      beyond = glm::length( worldXZ - ( field.MinCm + ( inside + 0.5f ) * field.TexelCm ) );
        WaterShoreSample sample;
        sample.GroundCm = std::max( t.x - kShoreSkirtSlope * beyond, std::min( t.x, field.OpenSeaFloorCm ) );
        sample.ShoreDistanceCm = std::clamp( t.y + beyond, -kShoreFarCm, kShoreFarCm );
        sample.ShoreDirection  = glm::vec2( t.z, t.w );
        return sample;
    }
} // namespace Desert::Water
