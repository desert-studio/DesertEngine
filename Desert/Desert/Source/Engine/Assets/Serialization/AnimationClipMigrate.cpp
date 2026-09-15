#include "AnimationClipMigrate.hpp"

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace Desert::Assets::Serialization
{
    namespace
    {
        // GENERATION 0, SPELLED OUT. These mirror what a pre-tick `.anim` holds, and they live here rather
        // than in the live schema for the reason SceneFormat.hpp gives for its own steps: the runtime must
        // know nothing about the old format. The only code in this engine that can read a v0 file is this
        // file, and it exists to turn v0 files into v1 files and then be unreachable.
        struct LegacyKeyPosition
        {
            float     Time  = 0.0f;
            glm::vec3 Value = glm::vec3( 0.0f );
        };
        struct LegacyKeyRotation
        {
            float     Time  = 0.0f;
            glm::quat Value = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        };
        struct LegacyKeyScale
        {
            float     Time  = 0.0f;
            glm::vec3 Value = glm::vec3( 1.0f );
        };
        struct LegacyChannel
        {
            std::string                    BoneName;
            std::vector<LegacyKeyPosition> Positions;
            std::vector<LegacyKeyRotation> Rotations;
            std::vector<LegacyKeyScale>    Scales;
        };
        struct LegacyNotify
        {
            std::string Name;
            float       Time = 0.0f;
        };
        struct LegacyAnimation
        {
            // Present only on a file this migration has already produced; its absence is what says v0.
            std::optional<int> Version;

            std::string                Name;
            float                      Duration          = 0.0f;
            float                      TicksPerSecond    = 25.0f;
            uint64_t                   SkeletonSignature = 0;
            std::vector<LegacyChannel> Channels;
            std::vector<LegacyNotify>  Notifies;
        };

        /// The standard display grids, coarsest LAST — the search below wants the coarsest that fits, and
        /// walking from the end is how it finds one without sorting.
        constexpr std::array<int32_t, 17> STANDARD_DISPLAY_RATES = { 120, 100, 60, 50, 48, 30, 25, 24, 20,
                                                                     15,  12,  10, 8,  6,  5,  4,  1 };

        /// Whether every time in `seconds` lands exactly on a grid of `rate` frames per second.
        [[nodiscard]] bool EveryTimeLandsOn( const std::vector<double>& seconds, int32_t rate )
        {
            for ( const double t : seconds )
            {
                const double frames = t * static_cast<double>( rate );
                if ( std::fabs( frames - std::round( frames ) ) > 1.0e-6 )
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    Common::ResultStr<std::string> MigrateAnimationJson( const std::string&        json,
                                                         AnimationMigrationReport& report )
    {
        report = AnimationMigrationReport{};

        const auto parsed = rfl::json::read<LegacyAnimation, rfl::DefaultIfMissing>( json );
        if ( !parsed.has_value() )
        {
            return Common::MakeFormattedError<std::string>( "not a readable `.anim`: {}",
                                                            parsed.error().what() );
        }
        const LegacyAnimation& legacy = parsed.value();

        // ABSENT MEANS 0 MEANS PRE-TICK. Never "already current": a file whose version field is missing is
        // exactly the file this function exists for, and treating it as converted would read its float
        // seconds as integer ticks and put every key on tick 0.
        report.FromVersion = legacy.Version.value_or( 0 );
        if ( report.FromVersion >= kAnimationVersion )
        {
            return Common::MakeFormattedError<std::string>(
                 "clip '{}' is already at `.anim` generation {}. Converting it again would read its integer "
                 "ticks as though they were seconds; a migration step is not idempotent and this one says "
                 "so rather than doubling its own work.",
                 legacy.Name, report.FromVersion );
        }

        if ( !( legacy.TicksPerSecond > 0.0F ) )
        {
            return Common::MakeFormattedError<std::string>(
                 "clip '{}' states {} ticks per second, so none of its key times can be placed in time at "
                 "all.",
                 legacy.Name, legacy.TicksPerSecond );
        }

        const double sourceRate = static_cast<double>( legacy.TicksPerSecond );
        const double projectRate = Animation::PROJECT_TICK_RATE.AsDouble();

        std::vector<double> everyTimeInSeconds;
        const auto          collect = [&]( float time ) { everyTimeInSeconds.push_back( time / sourceRate ); };
        for ( const auto& channel : legacy.Channels )
        {
            for ( const auto& k : channel.Positions )
            {
                collect( k.Time );
            }
            for ( const auto& k : channel.Rotations )
            {
                collect( k.Time );
            }
            for ( const auto& k : channel.Scales )
            {
                collect( k.Time );
            }
        }
        for ( const auto& n : legacy.Notifies )
        {
            collect( n.Time );
        }

        // The coarsest standard grid every key already lies on — see the header for why this is derived
        // and not defaulted.
        int32_t displayRate = Animation::DEFAULT_DISPLAY_RATE.Numerator;
        bool    fellBack    = true;
        for ( auto it = STANDARD_DISPLAY_RATES.rbegin(); it != STANDARD_DISPLAY_RATES.rend(); ++it )
        {
            if ( EveryTimeLandsOn( everyTimeInSeconds, *it ) )
            {
                displayRate = *it;
                fellBack    = false;
                break;
            }
        }
        report.DisplayRateNumerator   = displayRate;
        report.DisplayRateDenominator = 1;
        report.DisplayRateIsAFallback = fellBack;

        const auto toTick = [&]( float time ) -> int32_t
        {
            const double exact   = ( static_cast<double>( time ) / sourceRate ) * projectRate;
            const double rounded = std::round( exact );
            if ( std::fabs( exact - rounded ) > 1.0e-9 )
            {
                ++report.KeysMoved;
                const int64_t micro = static_cast<int64_t>( std::llround( ( rounded - exact ) * 1.0e6 ) );
                report.WorstMicro   = std::max( report.WorstMicro, micro < 0 ? -micro : micro );
            }
            return static_cast<int32_t>( rounded );
        };

        AnimationAssetData out;
        out.Version           = kAnimationVersion;
        out.Name              = legacy.Name;
        out.TickRate          = { Animation::PROJECT_TICK_RATE.Numerator,
                                  Animation::PROJECT_TICK_RATE.Denominator };
        out.DisplayRate       = { displayRate, 1 };
        out.DurationTicks     = toTick( legacy.Duration );
        out.SkeletonSignature = legacy.SkeletonSignature;

        out.Channels.reserve( legacy.Channels.size() );
        for ( const auto& channel : legacy.Channels )
        {
            ChannelData converted;
            converted.BoneName = channel.BoneName;
            converted.Positions.reserve( channel.Positions.size() );
            for ( const auto& k : channel.Positions )
            {
                converted.Positions.push_back( KeyPosition{ toTick( k.Time ), k.Value } );
            }
            converted.Rotations.reserve( channel.Rotations.size() );
            for ( const auto& k : channel.Rotations )
            {
                converted.Rotations.push_back( KeyRotation{ toTick( k.Time ), k.Value } );
            }
            converted.Scales.reserve( channel.Scales.size() );
            for ( const auto& k : channel.Scales )
            {
                converted.Scales.push_back( KeyScale{ toTick( k.Time ), k.Value } );
            }
            out.Channels.push_back( std::move( converted ) );
        }

        out.Notifies.reserve( legacy.Notifies.size() );
        for ( const auto& n : legacy.Notifies )
        {
            out.Notifies.push_back( NotifyData{ n.Name, toTick( n.Time ) } );
        }

        return Common::MakeSuccess( rfl::json::write( out ) );
    }
} // namespace Desert::Assets::Serialization
