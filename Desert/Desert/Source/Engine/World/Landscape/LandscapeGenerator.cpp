// Ported from UE 5.8 Engine/Source/Editor/LandscapeEditor/Private/LandscapeEditorObject.cpp:32-34,99-104
// (NewLandscape_* defaults), Public/LandscapeEditorObject.h:838-846 (ClampLandscapeSize) and
// Private/LandscapeEditorDetailCustomization_NewLandscape.cpp:1145-1230 (OnCreateButtonClicked: the centring
// offset, the flat mid-value fill), adapted: one section per component, so a component is one tile entity; no
// rotation (our landscape frame has none); the fill can also be UE's Noise-tool field at a seeded offset, and the
// ported Erosion / Hydro Erosion strokes (LandscapeSculpt.cpp) can run over the whole map with a brush of 1 — UE's
// New Landscape offers neither, it creates flat or imports.

#include <Engine/World/Landscape/LandscapeGenerator.hpp>

#include <Common/Core/JobSystem.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace Desert::World::Landscape
{
    namespace
    {
        /// Where the seed puts the map in noise space. splitmix64, so neighbouring seeds land far apart; the
        /// range keeps every coordinate positive (UE's noise samples |x|, and a map straddling 0 would mirror)
        /// and small enough that float keeps whole samples exact (2^15 + 8191 << 2^24).
        struct NoiseOffset
        {
            int32_t X = 0;
            int32_t Z = 0;
        };

        NoiseOffset SeedOffset( uint32_t seed )
        {
            uint64_t v                = seed + 0x9E3779B97F4A7C15ull;
            v                         = ( v ^ ( v >> 30u ) ) * 0xBF58476D1CE4E5B9ull;
            v                         = ( v ^ ( v >> 27u ) ) * 0x94D049BB133111EBull;
            v                         = v ^ ( v >> 31u );
            constexpr uint64_t kRange = 1ull << 15u;
            return { static_cast<int32_t>( v % kRange ), static_cast<int32_t>( ( v >> 32u ) % kRange ) };
        }

        /// The progress side of a run: counts steps and answers "go on?". A run without a watcher counts nothing
        /// and always goes on, so the unwatched run is the same code path as the watched one.
        struct Steps
        {
            LandscapeGenerateProgress* Progress = nullptr;

            /// Before a step: false once Cancel is set.
            bool Go() const
            {
                return !Progress || !Progress->Cancel.load( std::memory_order_relaxed );
            }
            void Advance( uint32_t n = 1u ) const
            {
                if ( Progress )
                    Progress->Done.fetch_add( n, std::memory_order_relaxed );
            }
            /// A loop that settled early: the rest of its budget is done too.
            void AdvanceTo( uint32_t done ) const
            {
                if ( Progress )
                    Progress->Done.store( done, std::memory_order_relaxed );
            }
            uint32_t Now() const
            {
                return Progress ? Progress->Done.load( std::memory_order_relaxed ) : 0u;
            }
        };

        uint32_t TotalSteps( const LandscapeGenerateSettings& s, uint32_t samplesZ )
        {
            uint32_t total = s.Fill == LandscapeGenerateFill::Noise ? samplesZ : 0u;
            if ( s.Erosion )
                total += static_cast<uint32_t>( s.ErosionSettings.Iterations ) + 1u; // + its noise pass
            if ( s.HydroErosion )
                total += static_cast<uint32_t>( s.HydroSettings.Iterations );
            return total + static_cast<uint32_t>( s.TilesX ) * static_cast<uint32_t>( s.TilesZ );
        }

        bool PositiveFinite( float v )
        {
            return std::isfinite( v ) && v > 0.0f;
        }
    } // namespace

    int32_t ClampLandscapeTileCount( int32_t count, uint32_t quadsPerTile )
    {
        const int32_t bySide = static_cast<int32_t>( kLandscapeMaxQuadsPerSide / std::max( quadsPerTile, 1u ) );
        return std::clamp( count, 1, std::min( kLandscapeMaxTilesPerSide, bySide ) );
    }

    Common::BoolResultStr ValidateLandscapeGenerate( const LandscapeGenerateSettings& s )
    {
        LandscapeRoot root;
        root.QuadsPerTile = s.QuadsPerTile;
        root.SpacingCm    = s.SpacingCm;
        root.ZScale       = s.ZScale;
        auto rootValid    = ValidateLandscapeRoot( root );
        if ( !rootValid.IsSuccess() )
            return Common::MakeError( "new landscape: " + rootValid.GetError() );
        for ( const auto& [name, count] : { std::pair{ "X", s.TilesX }, std::pair{ "Z", s.TilesZ } } )
            if ( ClampLandscapeTileCount( count, s.QuadsPerTile ) != count )
                return Common::MakeError(
                     std::string( "new landscape: " ) + std::to_string( count ) + " tiles along " + name +
                     " outside 1.." +
                     std::to_string( ClampLandscapeTileCount( kLandscapeMaxTilesPerSide, s.QuadsPerTile ) ) +
                     " for " + std::to_string( s.QuadsPerTile ) + " quads a tile" );
        if ( !std::isfinite( s.LocationCm.x ) || !std::isfinite( s.LocationCm.y ) ||
             !std::isfinite( s.LocationCm.z ) )
            return Common::MakeError( "new landscape: the location is not finite" );
        if ( s.Fill == LandscapeGenerateFill::Noise )
        {
            if ( !std::isfinite( s.NoiseHeightCm ) || s.NoiseHeightCm < 0.0f )
                return Common::MakeError( "new landscape: noise height " + std::to_string( s.NoiseHeightCm ) +
                                          " cm is negative or not finite" );
            auto noise = ValidateLandscapeNoise( { LandscapeNoiseMode::Both, s.NoiseScale } );
            if ( !noise.IsSuccess() )
                return Common::MakeError( "new landscape: " + noise.GetError() );
        }
        if ( s.Erosion || s.HydroErosion )
            if ( !( s.ErosionStrength >= 0.0f && s.ErosionStrength <= 1.0f ) )
                return Common::MakeError( "new landscape: erosion strength " +
                                          std::to_string( s.ErosionStrength ) + " outside 0..1" );
        if ( s.Erosion )
        {
            auto erosion = ValidateLandscapeErosion( s.ErosionSettings );
            if ( !erosion.IsSuccess() )
                return Common::MakeError( "new landscape: " + erosion.GetError() );
        }
        if ( s.HydroErosion )
        {
            auto hydro = ValidateLandscapeHydroErosion( s.HydroSettings );
            if ( !hydro.IsSuccess() )
                return Common::MakeError( "new landscape: " + hydro.GetError() );
        }
        return Common::MakeSuccess( true );
    }

    float LandscapeGenerateProgress::Fraction() const
    {
        const uint32_t total = Total.load( std::memory_order_relaxed );
        return total == 0u ? 0.0f
                           : std::min( 1.0f, static_cast<float>( Done.load( std::memory_order_relaxed ) ) /
                                                  static_cast<float>( total ) );
    }

    Common::ResultStr<LandscapeGeneratedMap> GenerateLandscapeMap( const LandscapeGenerateSettings& s,
                                                                   LandscapeGenerateProgress*       progress )
    {
        auto valid = ValidateLandscapeGenerate( s );
        if ( !valid.IsSuccess() )
            return Common::MakeError<LandscapeGeneratedMap>( valid.GetError() );

        LandscapeGeneratedMap map;
        map.SamplesX = static_cast<uint32_t>( s.TilesX ) * s.QuadsPerTile + 1u;
        map.SamplesZ = static_cast<uint32_t>( s.TilesZ ) * s.QuadsPerTile + 1u;
        map.Samples.assign( static_cast<size_t>( map.SamplesX ) * map.SamplesZ, kLandscapeMidSample );

        const Steps steps{ progress };
        if ( progress )
            progress->Total.store( TotalSteps( s, map.SamplesZ ), std::memory_order_relaxed );
        const auto cancelled = []
        { return Common::MakeError<LandscapeGeneratedMap>( kLandscapeGenerateCancelled ); };

        const NoiseOffset offset = SeedOffset( s.Seed );
        if ( s.Fill == LandscapeGenerateFill::Noise )
        {
            // LandscapeSampleFromHeightCm's scale: one height step is ZScale / 128 cm.
            const float heightSteps = s.NoiseHeightCm * 128.0f / s.ZScale;
            for ( uint32_t z = 0; z < map.SamplesZ; ++z )
            {
                if ( !steps.Go() )
                    return cancelled();
                for ( uint32_t x = 0; x < map.SamplesX; ++x )
                {
                    const float n = LandscapeNoiseSample( offset.X + static_cast<int32_t>( x ),
                                                          offset.Z + static_cast<int32_t>( z ), s.NoiseScale );
                    const float v = std::round( static_cast<float>( kLandscapeMidSample ) + n * heightSteps );
                    map.Samples[static_cast<size_t>( z ) * map.SamplesX + x] =
                         static_cast<uint16_t>( std::clamp( v, 0.0f, 65535.0f ) );
                }
                steps.Advance();
            }
        }

        if ( s.Erosion || s.HydroErosion )
        {
            // The whole map is one stroke of a brush weighing 1 everywhere but the outermost ring, which the
            // ported loops read as neighbours and never shed from (LandscapeErosionField). The field lies at the
            // seed's offset, so the position-seeded noise of both passes follows the seed.
            LandscapeErosionField field;
            field.Rect  = { offset.X, offset.Z, offset.X + static_cast<int32_t>( map.SamplesX ) - 1,
                            offset.Z + static_cast<int32_t>( map.SamplesZ ) - 1 };
            field.Inner = { field.Rect.X1 + 1, field.Rect.Z1 + 1, field.Rect.X2 - 1, field.Rect.Z2 - 1 };
            field.Brush.assign( static_cast<size_t>( map.SamplesX - 2u ) * ( map.SamplesZ - 2u ), 1.0f );
            field.Heights = std::move( map.Samples );
            const LandscapeErosionIterationHook iteration = [&steps]
            {
                if ( !steps.Go() )
                    return false;
                steps.Advance();
                return true;
            };
            if ( s.Erosion )
            {
                const uint32_t end = steps.Now() + static_cast<uint32_t>( s.ErosionSettings.Iterations );
                map.ErosionIterations =
                     LandscapeThermalErosion( field, s.ErosionSettings, s.ErosionStrength, iteration );
                if ( !steps.Go() )
                    return cancelled();
                steps.AdvanceTo( end );
                LandscapeErosionNoise( field, s.ErosionSettings, s.ErosionStrength,
                                       kLandscapeNoiseMaximumValueRadiusCm );
                steps.Advance();
            }
            if ( s.HydroErosion )
            {
                const uint32_t end = steps.Now() + static_cast<uint32_t>( s.HydroSettings.Iterations );
                map.HydroErosionIterations =
                     LandscapeHydraulicErosion( field, s.HydroSettings, s.ErosionStrength, iteration );
                if ( !steps.Go() )
                    return cancelled();
                steps.AdvanceTo( end );
            }
            map.Samples = std::move( field.Heights );
        }
        return Common::MakeSuccess( std::move( map ) );
    }

    Common::ResultStr<LandscapeGenerated> GenerateLandscape( const LandscapeGenerateSettings& s,
                                                             LandscapeGenerateProgress*       progress )
    {
        const Steps steps{ progress };
        auto        made = GenerateLandscapeMap( s, progress );
        if ( !made.IsSuccess() )
            return Common::MakeError<LandscapeGenerated>( made.GetError() );
        const LandscapeGeneratedMap map = made.ExtractValue();

        LandscapeGenerated out;
        out.ErosionIterations      = map.ErosionIterations;
        out.HydroErosionIterations = map.HydroErosionIterations;
        out.Root.QuadsPerTile      = s.QuadsPerTile;
        out.Root.SpacingCm         = s.SpacingCm;
        out.Root.ZScale            = s.ZScale;
        // UE: Offset = -ComponentCount * QuadsPerComponent / 2 in quads, times the scale.
        out.Root.Origin =
             s.LocationCm +
             glm::vec3( -static_cast<float>( s.TilesX ) * LandscapeTileExtentCm( out.Root ) * 0.5f, 0.0f,
                        -static_cast<float>( s.TilesZ ) * LandscapeTileExtentCm( out.Root ) * 0.5f );

        const uint32_t q       = s.QuadsPerTile;
        const uint32_t samples = LandscapeTileSamples( out.Root );
        out.Tiles.reserve( static_cast<size_t>( s.TilesX ) * static_cast<size_t>( s.TilesZ ) );
        for ( int32_t tz = 0; tz < s.TilesZ; ++tz )
            for ( int32_t tx = 0; tx < s.TilesX; ++tx )
            {
                if ( !steps.Go() )
                    return Common::MakeError<LandscapeGenerated>( kLandscapeGenerateCancelled );
                std::vector<uint16_t> values( static_cast<size_t>( samples ) * samples );
                for ( uint32_t z = 0; z < samples; ++z )
                {
                    const auto row =
                         map.Samples.begin() +
                         static_cast<std::ptrdiff_t>( ( static_cast<size_t>( tz ) * q + z ) * map.SamplesX +
                                                      static_cast<size_t>( tx ) * q );
                    std::copy_n( row, samples, values.begin() + static_cast<std::ptrdiff_t>( z * samples ) );
                }
                auto tile = LandscapeTileData::FromSamples( samples, samples, std::move( values ) );
                if ( !tile.IsSuccess() )
                    return Common::MakeError<LandscapeGenerated>( "new landscape: tile (" + std::to_string( tx ) +
                                                                  ", " + std::to_string( tz ) +
                                                                  "): " + tile.GetError() );
                out.Tiles.push_back( { tx, tz, tile.ExtractValue() } );
                steps.Advance();
            }
        return Common::MakeSuccess( std::move( out ) );
    }

    LandscapeGenerateJob::~LandscapeGenerateJob()
    {
        Cancel();
        Wait();
    }

    Common::BoolResultStr LandscapeGenerateJob::Start( const LandscapeGenerateSettings& settings )
    {
        if ( Running() )
            return Common::MakeError( "new landscape: generation in progress (" +
                                      std::to_string( static_cast<int>( Fraction() * 100.0f ) ) +
                                      " %); wait for it or cancel it" );
        auto valid = ValidateLandscapeGenerate( settings );
        if ( !valid.IsSuccess() )
            return valid;
        m_Progress = std::make_shared<LandscapeGenerateProgress>();
        m_Run      = Common::JobSystem::Get().Async( [settings, progress = m_Progress]
                                                { return GenerateLandscape( settings, progress.get() ); } );
        return Common::MakeSuccess( true );
    }

    bool LandscapeGenerateJob::Running() const
    {
        return m_Run.valid();
    }

    float LandscapeGenerateJob::Fraction() const
    {
        return m_Progress ? m_Progress->Fraction() : 0.0f;
    }

    uint32_t LandscapeGenerateJob::StepsDone() const
    {
        return m_Progress ? m_Progress->Done.load( std::memory_order_relaxed ) : 0u;
    }

    void LandscapeGenerateJob::Cancel()
    {
        if ( m_Progress )
            m_Progress->Cancel.store( true, std::memory_order_relaxed );
    }

    std::optional<Common::ResultStr<LandscapeGenerated>> LandscapeGenerateJob::TakeFinished()
    {
        if ( !m_Run.valid() || m_Run.wait_for( std::chrono::seconds( 0 ) ) != std::future_status::ready )
            return std::nullopt;
        m_Progress.reset();
        return m_Run.get();
    }

    void LandscapeGenerateJob::Wait()
    {
        if ( m_Run.valid() )
            m_Run.wait();
    }
} // namespace Desert::World::Landscape
