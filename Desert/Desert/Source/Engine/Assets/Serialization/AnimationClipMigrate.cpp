#include "AnimationClipMigrate.hpp"

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Animation/KeyInterpolation.hpp>
#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <ranges>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace Desert::Assets::Serialization
{
    // A NAMED namespace and not an anonymous one, and reflect-cpp is the reason rather than taste: its
    // field-counting trick needs the type to have LINKAGE, and a struct with no linkage cannot be the type
    // of a reflected field ("cannot be defined in any other translation unit because its type does not
    // have linkage"). `LegacyFrameRate` inside an `optional` is what found it.
    namespace Legacy
    {
        // GENERATIONS 0 AND 1, SPELLED OUT IN ONE MIRROR. `Time` is generation 0's float seconds and
        // `Tick` is generation 1's integer; exactly one of them is present in any real file, and which one
        // is what the version field says. They live here rather than in the live schema for the reason
        // SceneFormat.hpp gives for its own steps: the runtime must know nothing about the old format, and
        // the only code in this engine that can read one is the code whose job is to delete it.
        struct LegacyKeyPosition
        {
            std::optional<float>   Time;
            std::optional<int32_t> Tick;
            glm::vec3              Value = glm::vec3( 0.0f );
        };
        struct LegacyKeyRotation
        {
            std::optional<float>   Time;
            std::optional<int32_t> Tick;
            glm::quat              Value = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        };
        struct LegacyKeyScale
        {
            std::optional<float>   Time;
            std::optional<int32_t> Tick;
            glm::vec3              Value = glm::vec3( 1.0f );
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
            std::string            Name;
            std::optional<float>   Time;
            std::optional<int32_t> Tick;
        };
        struct LegacyFrameRate
        {
            int32_t Numerator   = 24000;
            int32_t Denominator = 1;
        };
        struct LegacyAnimation
        {
            std::optional<int> Version;

            std::string Name;

            // Generation 0 only.
            std::optional<float> Duration;
            std::optional<float> TicksPerSecond;

            // Generation 1 onwards.
            std::optional<LegacyFrameRate> TickRate;
            std::optional<LegacyFrameRate> DisplayRate;
            std::optional<int32_t>         DurationTicks;

            uint64_t                   SkeletonSignature = 0;
            std::vector<LegacyChannel> Channels;
            std::vector<LegacyNotify>  Notifies;
        };
    } // namespace Legacy

    namespace
    {
    } // namespace

    namespace
    {
        /// The standard display grids, coarsest LAST — the search below wants the coarsest that fits, and
        /// walking from the end is how it finds one without sorting.
        constexpr std::array<int32_t, 17> STANDARD_DISPLAY_RATES = { 120, 100, 60, 50, 48, 30, 25, 24, 20,
                                                                     15,  12,  10, 8,  6,  5,  4,  1 };

        /// Whether every time in `seconds` lands exactly on a grid of `rate` frames per second.
        [[nodiscard]] bool EveryTimeLandsOn( const std::vector<double>& seconds, int32_t rate )
        {
            return std::ranges::all_of( seconds,
                                        [rate]( const double t )
                                        {
                                            const double frames = t * static_cast<double>( rate );
                                            return std::fabs( frames - std::round( frames ) ) <= 1.0e-6;
                                        } );
        }
    } // namespace

    Common::ResultStr<std::string> MigrateAnimationJson( const std::string&        json,
                                                         AnimationMigrationReport& report )
    {
        report = AnimationMigrationReport{};

        const auto parsed = rfl::json::read<Legacy::LegacyAnimation, rfl::DefaultIfMissing>( json );
        if ( !parsed.has_value() )
        {
            return Common::MakeFormattedError<std::string>( "not a readable `.anim`: {}", parsed.error().what() );
        }
        const Legacy::LegacyAnimation& legacy = parsed.value();

        // ABSENT MEANS 0 MEANS PRE-TICK. Never "already current": a file whose version field is missing is
        // exactly the file this function exists for, and treating it as converted would read its float
        // seconds as integer ticks and put every key on tick 0.
        report.FromVersion = legacy.Version.value_or( 0 );
        if ( report.FromVersion >= kAnimationVersion )
        {
            return Common::MakeFormattedError<std::string>(
                 "clip '{}' is already at `.anim` generation {}. A migration step is not idempotent and "
                 "this one says so rather than doubling its own work.",
                 legacy.Name, report.FromVersion );
        }

        // ---- generation 0 -> ticks. Generations 1 and up already have them. ---------------------------
        const bool fromSeconds = report.FromVersion < 1;

        double sourceRate = 1.0;
        if ( fromSeconds )
        {
            sourceRate = static_cast<double>( legacy.TicksPerSecond.value_or( 25.0F ) );
            if ( !( sourceRate > 0.0 ) )
            {
                return Common::MakeFormattedError<std::string>(
                     "clip '{}' states {} ticks per second, so none of its key times can be placed in time "
                     "at all.",
                     legacy.Name, sourceRate );
            }
        }

        const double projectRate = Animation::PROJECT_TICK_RATE.AsDouble();

        // The one place either generation's spelling of "when" becomes this one's.
        const auto toTick = [&]( const std::optional<float>&   seconds,
                                 const std::optional<int32_t>& ticks ) -> int32_t
        {
            if ( !fromSeconds )
            {
                return ticks.value_or( 0 );
            }
            const double exact   = ( static_cast<double>( seconds.value_or( 0.0F ) ) / sourceRate ) * projectRate;
            const double rounded = std::round( exact );
            if ( std::fabs( exact - rounded ) > 1.0e-9 )
            {
                ++report.KeysMoved;
                const auto micro  = static_cast<int64_t>( std::llround( ( rounded - exact ) * 1.0e6 ) );
                report.WorstMicro = std::max( report.WorstMicro, micro < 0 ? -micro : micro );
            }
            return static_cast<int32_t>( rounded );
        };

        // ---- the display grid ------------------------------------------------------------------------
        //
        // Derived from the keys for a generation-0 file (see the header); CARRIED for a generation-1 one,
        // which already states a grid its author chose. Re-deriving it there would overwrite an authored
        // value with a guess, which is the shape a migration must never have.
        int32_t displayRate = Animation::DEFAULT_DISPLAY_RATE.Numerator;
        bool    fellBack    = true;
        if ( fromSeconds )
        {
            std::vector<double> everyTimeInSeconds;
            const auto          collect = [&]( const std::optional<float>& time )
            { everyTimeInSeconds.push_back( static_cast<double>( time.value_or( 0.0F ) ) / sourceRate ); };
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

            for ( const int32_t candidate : std::ranges::reverse_view( STANDARD_DISPLAY_RATES ) )
            {
                if ( EveryTimeLandsOn( everyTimeInSeconds, candidate ) )
                {
                    displayRate = candidate;
                    fellBack    = false;
                    break;
                }
            }
        }
        else if ( legacy.DisplayRate.has_value() )
        {
            displayRate = legacy.DisplayRate->Numerator;
            fellBack    = false;
        }
        report.DisplayRateNumerator   = displayRate;
        report.DisplayRateDenominator = 1;
        report.DisplayRateIsAFallback = fellBack;

        // ---- the shape every key now STATES ----------------------------------------------------------
        //
        // THIS IS THE CONDITION THE VERSION STEP WAS GRANTED ON. `rfl::DefaultIfMissing` would invent
        // `Linear`/`Auto` for a file that says nothing, and an invented default is indistinguishable from
        // an authored one for ever after — so the conversion WRITES them. `Linear` because that is what
        // every clip in this engine did before per-key interpolation existed: a migration states the
        // behaviour a file already had, it does not choose a new one.
        KeyShape statedShape;
        statedShape.Interp = static_cast<int>( Animation::KeyInterp::Linear );
        statedShape.Mode   = static_cast<int>( Animation::TangentMode::Auto );

        constexpr Animation::FrameRate TICKS = Animation::PROJECT_TICK_RATE;

        AnimationAssetData out;
        out.Version           = kAnimationVersion;
        out.Name              = legacy.Name;
        out.TickRate          = { TICKS.Numerator, TICKS.Denominator };
        out.DisplayRate       = { displayRate, 1 };
        out.DurationTicks     = toTick( legacy.Duration, legacy.DurationTicks );
        out.SkeletonSignature = legacy.SkeletonSignature;

        out.Channels.reserve( legacy.Channels.size() );
        for ( const auto& channel : legacy.Channels )
        {
            ChannelData converted;
            converted.BoneName = channel.BoneName;
            converted.Positions.reserve( channel.Positions.size() );
            for ( const auto& k : channel.Positions )
            {
                converted.Positions.push_back( KeyPosition{ toTick( k.Time, k.Tick ), k.Value, statedShape,
                                                            glm::vec3( 0.0f ), glm::vec3( 0.0f ) } );
                ++report.ShapesWritten;
            }
            converted.Rotations.reserve( channel.Rotations.size() );
            for ( const auto& k : channel.Rotations )
            {
                converted.Rotations.push_back( KeyRotation{ toTick( k.Time, k.Tick ), k.Value, statedShape } );
                ++report.ShapesWritten;
            }
            converted.Scales.reserve( channel.Scales.size() );
            for ( const auto& k : channel.Scales )
            {
                converted.Scales.push_back( KeyScale{ toTick( k.Time, k.Tick ), k.Value, statedShape,
                                                      glm::vec3( 0.0f ), glm::vec3( 0.0f ) } );
                ++report.ShapesWritten;
            }
            out.Channels.push_back( std::move( converted ) );
        }

        out.Notifies.reserve( legacy.Notifies.size() );
        for ( const auto& n : legacy.Notifies )
        {
            out.Notifies.push_back( NotifyData{ n.Name, toTick( n.Time, n.Tick ) } );
        }

        // ---- generation 3: the clip STATES what its values mean ---------------------------------------
        //
        // One Absolute section over the whole clip at full weight, which is exactly what every file of
        // every earlier generation did. §938's point is that the file should SAY it: an implicit reading
        // and an authored one are indistinguishable for ever after, and the step after this one — a
        // section owning its own keys — converts files that say what they meant rather than guessing.
        EnsureStatedSections( out );
        report.SectionsWritten = static_cast<int>( out.Sections.size() );

        return Common::MakeSuccess( rfl::json::write( out ) );
    }
} // namespace Desert::Assets::Serialization
