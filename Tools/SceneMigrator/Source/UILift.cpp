// THE UI LIFT: a v40 `UIAnim` block → a UIAnimation-host Sequence (Engine/Animation/Timeline/Hosts.hpp, the table
// under "UI animation"). The migrator's, beside ClipLift.cpp: the engine reads no v40 block.
//
// It speaks the v40 integers and converts them here, checked, rather than through `PresetOf` (Hosts.cpp): that
// file pins `ECS::UIEasing` and so includes the ECS components, and the lift builds wherever the timeline core
// builds (the migrator, its step suites, the contract suite).

#include "UILift.hpp"

#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Animation::Timeline
{
    namespace
    {
        struct PropertyRow
        {
            const char* Name;
            TrackKind   Kind;
            int         Components; ///< how many of the key's vec4 lanes the property reads
        };

        // `ECS::UITweenProperty` in declaration order — the one table from the v40 integer to the track.
        constexpr PropertyRow kProperties[] = {
             { "Offset", TrackKind::Vector, 2 },
             { "Size", TrackKind::Vector, 2 },
             { "Opacity", TrackKind::Float, 1 },
             { "Color", TrackKind::Vector, 3 },
        };

        constexpr int kLastEasing = static_cast<int>( EasingPreset::BounceOut );

        // The component channels a property's keys land in: X only for a Float track, X/Y(/Z) for a Vector.
        std::vector<FloatChannel*> LanesOf( Channel& channel, const int components )
        {
            if ( auto* f = std::get_if<FloatChannel>( &channel ) )
                return { f };
            auto&                      v     = std::get<VectorChannel>( channel );
            std::vector<FloatChannel*> lanes = { &v.X, &v.Y, &v.Z };
            lanes.resize( static_cast<size_t>( components ) );
            return lanes;
        }
    } // namespace

    Common::ResultStr<UILiftResult> LiftUIAnimation( const UIAnimationV40&  legacy,
                                                     const std::string_view widgetLocator,
                                                     const FrameRate tickRate, const FrameRate displayRate )
    {
        UILiftResult result;
        Sequence&    sequence = result.Lifted;
        sequence.Host         = SequenceHost::UIAnimation;
        sequence.TickRate     = tickRate;
        sequence.DisplayRate  = displayRate;

        Binding widget;
        widget.Guid    = BindingGuid::Generate();
        widget.Kind    = BindingKind::Widget;
        widget.Locator = std::string( widgetLocator );
        widget.Label   = std::string( widgetLocator );
        sequence.Bindings.push_back( widget );

        // A float second → the nearest tick; the distance is the report's, never silent.
        const auto tickOf = [&]( const float seconds )
        {
            const FrameNumber tick = NearestTick( SecondsToFrameTime( static_cast<double>( seconds ), tickRate ) );
            const double      back = FrameTimeToSeconds( FrameTime{ tick, 0.0F }, tickRate );
            const auto        rounding = static_cast<float>( std::abs( back - static_cast<double>( seconds ) ) );
            if ( rounding > 0.0F )
            {
                ++result.Report.RoundedKeys;
                result.Report.MaxRoundingSeconds = std::max( result.Report.MaxRoundingSeconds, rounding );
            }
            return tick;
        };

        FrameNumber start;
        // The range end is not a key: its rounding is not the report's (a Duration off the grid moves no value).
        FrameNumber end = NearestTick(
             SecondsToFrameTime( static_cast<double>( std::max( legacy.Duration, 0.0F ) ), tickRate ) );
        // The keys' ticks, per track, computed once (rounding is counted once per key).
        std::vector<std::vector<FrameNumber>> ticks( legacy.Tracks.size() );
        for ( size_t t = 0; t < legacy.Tracks.size(); ++t )
        {
            const UIAnimationTrackV40& track = legacy.Tracks[t];
            if ( track.Property < 0 || track.Property >= static_cast<int>( std::size( kProperties ) ) )
            {
                return Common::MakeFormattedError<UILiftResult>(
                     "LiftUIAnimation: widget {} track {} has property {}, which is none of "
                     "Offset/Size/Opacity/Color "
                     "(0..3)",
                     widgetLocator, t, track.Property );
            }
            const char* name = kProperties[track.Property].Name;
            for ( size_t k = 0; k < track.Keys.size(); ++k )
            {
                const UIAnimationKeyV40& key = track.Keys[k];
                if ( key.Easing < 0 || key.Easing > kLastEasing )
                {
                    return Common::MakeFormattedError<UILiftResult>(
                         "LiftUIAnimation: widget {} track '{}' key {} has easing {}, which is no UIEasing "
                         "(0..{})",
                         widgetLocator, name, k, key.Easing, kLastEasing );
                }
                if ( k > 0 && key.Time < track.Keys[k - 1].Time )
                {
                    return Common::MakeFormattedError<UILiftResult>(
                         "LiftUIAnimation: widget {} track '{}' keys are not sorted: key {} at {} s follows {} s",
                         widgetLocator, name, k, key.Time, track.Keys[k - 1].Time );
                }
                const FrameNumber tick = tickOf( key.Time );
                if ( k > 0 && !( ticks[t].back() < tick ) )
                {
                    return Common::MakeFormattedError<UILiftResult>(
                         "LiftUIAnimation: widget {} track '{}' keys {} and {} ({} s, {} s) land on one tick {} "
                         "of "
                         "{}/{}",
                         widgetLocator, name, k - 1, k, track.Keys[k - 1].Time, key.Time, tick.Value,
                         tickRate.Numerator, tickRate.Denominator );
                }
                ticks[t].push_back( tick );
                start = std::min( start, tick );
                end   = std::max( end, tick );
            }
        }
        sequence.Start = start;
        sequence.End   = end;

        for ( size_t t = 0; t < legacy.Tracks.size(); ++t )
        {
            const UIAnimationTrackV40& source = legacy.Tracks[t];
            const PropertyRow&         row    = kProperties[source.Property];

            Track track;
            track.Binding    = widget.Guid;
            track.Property   = row.Name;
            track.Kind       = row.Kind;
            Section& section = AddSection( track, start, end );

            std::vector<FloatChannel*> lanes = LanesOf( std::get<Channel>( section.Content ), row.Components );
            for ( size_t c = 0; c < lanes.size(); ++c )
            {
                std::vector<ScalarKey>& keys = lanes[c]->Keys;
                for ( size_t k = 0; k < source.Keys.size(); ++k )
                {
                    ScalarKey key;
                    key.Tick  = ticks[t][k];
                    key.Value = source.Keys[k].Value[static_cast<glm::length_t>( c )];
                    keys.push_back( key );
                }
                // Each segment takes the easing of the key it ENDS at (v40's rule, and the preset's). Last
                // segment first: an InOut preset inserts a key inside its own segment, which shifts only the
                // indices after it.
                for ( size_t k = source.Keys.size(); k-- > 1; )
                {
                    const auto preset = static_cast<EasingPreset>( source.Keys[k].Easing );
                    const Common::ResultStr<EasingResult> eased =
                         ApplyEasingPreset( keys, k, preset, tickRate, displayRate );
                    if ( !eased )
                    {
                        return Common::MakeFormattedError<UILiftResult>(
                             "LiftUIAnimation: widget {} track '{}': {}", widgetLocator, row.Name,
                             eased.GetError() );
                    }
                    result.Report.MaxEasingDeviation =
                         std::max( result.Report.MaxEasingDeviation, eased.GetValue().MaxDeviation );
                }
            }
            sequence.Tracks.push_back( std::move( track ) );
        }

        if ( const Common::BoolResultStr valid = Validate( sequence ); !valid.IsSuccess() )
        {
            return Common::MakeFormattedError<UILiftResult>( "LiftUIAnimation: widget {}: {}", widgetLocator,
                                                             valid.GetError() );
        }
        return Common::MakeSuccess( std::move( result ) );
    }
} // namespace Desert::Animation::Timeline
