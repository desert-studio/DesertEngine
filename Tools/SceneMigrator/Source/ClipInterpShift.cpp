#include "ClipInterpShift.hpp"

#include <Engine/Animation/KeyInterpolation.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <tuple>
#include <variant>

namespace Desert::Migration
{
    using Animation::FrameNumber;
    using Animation::FrameRate;
    using Animation::FrameTime;
    using Animation::KeyInterp;
    using Animation::ScalarKey;
    namespace Timeline = Animation::Timeline;

    namespace
    {
        /// The v5 segment: (prev, next], shaped by `next.Interp` — the sampler ANIM v5 was read with, frozen
        /// here term for term (bracket, factor and span arithmetic are Timeline/Channel.cpp's) so the proof
        /// compares the same floating-point operations. @pre `keys` is not empty.
        struct ArrivingBracket
        {
            bool   Inside = false;
            size_t Hold   = 0; ///< the key held when not inside
            size_t Next   = 0;
            float  Factor = 0.0F;
            double Span   = 0.0;
        };

        ArrivingBracket Bracket( const std::vector<ScalarKey>& keys, const FrameTime at, const FrameRate rate )
        {
            const double t = at.AsTicks();
            const auto   next =
                 std::lower_bound( keys.begin(), keys.end(), t, []( const ScalarKey& key, double tick )
                                   { return static_cast<double>( key.Tick.Value ) < tick; } );
            ArrivingBracket out;
            if ( next == keys.begin() )
                return out;
            if ( next == keys.end() )
            {
                out.Hold = keys.size() - 1;
                return out;
            }
            const auto prev = next - 1;
            const auto span = static_cast<double>( next->Tick.Value - prev->Tick.Value );
            out.Inside      = true;
            out.Next        = static_cast<size_t>( next - keys.begin() );
            out.Factor      = static_cast<float>( ( t - static_cast<double>( prev->Tick.Value ) ) / span );
            out.Span = span * static_cast<double>( rate.Denominator ) / static_cast<double>( rate.Numerator );
            return out;
        }

        float SampleArriving( const Timeline::FloatChannel& channel, const FrameTime at, const FrameRate rate )
        {
            const std::vector<ScalarKey>& keys = channel.Keys;
            if ( keys.empty() )
                return channel.Default;
            const ArrivingBracket b = Bracket( keys, at, rate );
            if ( !b.Inside )
                return keys[b.Hold].Value;
            const ScalarKey& prev = keys[b.Next - 1];
            const ScalarKey& next = keys[b.Next];
            return Animation::EvaluateSegment( prev.Value, prev.LeaveTangent, next.Value, next.ArriveTangent,
                                               next.Interp, b.Span, b.Factor );
        }

        glm::quat QuatAt( const Timeline::RotationChannel& channel, const size_t i )
        {
            return glm::quat( channel.W.Keys[i].Value, channel.X.Keys[i].Value, channel.Y.Keys[i].Value,
                              channel.Z.Keys[i].Value );
        }

        glm::quat SampleArriving( const Timeline::RotationChannel& channel, const FrameTime at,
                                  const FrameRate rate )
        {
            const std::vector<ScalarKey>& keys = channel.X.Keys;
            if ( keys.empty() )
                return glm::quat( channel.W.Default, channel.X.Default, channel.Y.Default, channel.Z.Default );
            const ArrivingBracket b = Bracket( keys, at, rate );
            if ( !b.Inside )
                return QuatAt( channel, b.Hold );
            const glm::quat prev = QuatAt( channel, b.Next - 1 );
            if ( keys[b.Next].Interp == KeyInterp::Constant )
                return b.Factor < 1.0F ? prev : QuatAt( channel, b.Next );
            return glm::slerp( prev, QuatAt( channel, b.Next ), b.Factor );
        }

        /// The integer ticks where a channel's two samplers can differ: its keyed range.
        template <typename TCompare>
        std::string EveryTick( const std::vector<ScalarKey>& keys, TCompare&& compare, std::size_t& samples )
        {
            if ( keys.empty() )
                return {};
            for ( int32_t tick = keys.front().Tick.Value; tick <= keys.back().Tick.Value; ++tick )
            {
                ++samples;
                if ( std::string refusal = compare( FrameTime{ FrameNumber{ tick }, 0.0F } ); !refusal.empty() )
                    return refusal;
            }
            return {};
        }

        std::string CompareFloat( const Timeline::FloatChannel& arriving, const Timeline::FloatChannel& leaving,
                                  const FrameRate rate, std::size_t& samples )
        {
            if ( arriving.Keys.size() != leaving.Keys.size() )
                return std::format( "{} keys became {}", arriving.Keys.size(), leaving.Keys.size() );
            return EveryTick(
                 leaving.Keys,
                 [&]( const FrameTime at ) -> std::string
                 {
                     const float was = SampleArriving( arriving, at, rate );
                     const float is  = Timeline::Evaluate( leaving, at, rate );
                     if ( was == is )
                         return {};
                     return std::format( "tick {}: {} became {}", at.Frame.Value, was, is );
                 },
                 samples );
        }

        std::string CompareVector( const Timeline::VectorChannel& a, const Timeline::VectorChannel& b,
                                   const FrameRate rate, std::size_t& samples )
        {
            for ( const auto& [x, y, name] :
                  { std::tuple{ &a.X, &b.X, "X" }, std::tuple{ &a.Y, &b.Y, "Y" }, std::tuple{ &a.Z, &b.Z, "Z" } } )
            {
                if ( std::string refusal = CompareFloat( *x, *y, rate, samples ); !refusal.empty() )
                    return std::format( "{}: {}", name, refusal );
            }
            return {};
        }

        std::string CompareRotation( const Timeline::RotationChannel& arriving,
                                     const Timeline::RotationChannel& leaving, const FrameRate rate,
                                     std::size_t& samples )
        {
            if ( arriving.X.Keys.size() != leaving.X.Keys.size() )
                return std::format( "rotation: {} keys became {}", arriving.X.Keys.size(), leaving.X.Keys.size() );
            return EveryTick(
                 leaving.X.Keys,
                 [&]( const FrameTime at ) -> std::string
                 {
                     const glm::quat was = SampleArriving( arriving, at, rate );
                     const glm::quat is  = Timeline::Evaluate( leaving, at, rate );
                     if ( was == is )
                         return {};
                     return std::format( "rotation differs at tick {}", at.Frame.Value );
                 },
                 samples );
        }

        std::string CompareChannel( const Timeline::Channel& arriving, const Timeline::Channel& leaving,
                                    const FrameRate rate, std::size_t& samples )
        {
            if ( arriving.index() != leaving.index() )
                return "the channel changed kind";
            if ( const auto* a = std::get_if<Timeline::FloatChannel>( &arriving ) )
                return CompareFloat( *a, std::get<Timeline::FloatChannel>( leaving ), rate, samples );
            if ( const auto* a = std::get_if<Timeline::VectorChannel>( &arriving ) )
                return CompareVector( *a, std::get<Timeline::VectorChannel>( leaving ), rate, samples );
            if ( const auto* a = std::get_if<Timeline::RotationChannel>( &arriving ) )
                return CompareRotation( *a, std::get<Timeline::RotationChannel>( leaving ), rate, samples );
            if ( const auto* a = std::get_if<Timeline::TransformChannel>( &arriving ) )
            {
                const auto& b = std::get<Timeline::TransformChannel>( leaving );
                if ( std::string r = CompareVector( a->Translation, b.Translation, rate, samples ); !r.empty() )
                    return "translation " + r;
                if ( std::string r = CompareRotation( a->Rotation, b.Rotation, rate, samples ); !r.empty() )
                    return r;
                if ( std::string r = CompareVector( a->Scale, b.Scale, rate, samples ); !r.empty() )
                    return "scale " + r;
                return {};
            }
            if ( const auto* a = std::get_if<Timeline::BoolChannel>( &arriving ) )
                return CompareFloat( a->Bits, std::get<Timeline::BoolChannel>( leaving ).Bits, rate, samples );
            return {}; // Event: no modes
        }

        void ShiftVector( Timeline::VectorChannel& channel, std::size_t& shifted )
        {
            for ( Timeline::FloatChannel* c : { &channel.X, &channel.Y, &channel.Z } )
            {
                ShiftInterpToLeavingKey( c->Keys );
                ++shifted;
            }
        }

        void ShiftRotation( Timeline::RotationChannel& channel, std::size_t& shifted )
        {
            for ( Timeline::FloatChannel* c : { &channel.X, &channel.Y, &channel.Z, &channel.W } )
            {
                ShiftInterpToLeavingKey( c->Keys );
                ++shifted;
            }
        }
    } // namespace

    void ShiftInterpToLeavingKey( std::vector<ScalarKey>& keys )
    {
        for ( std::size_t i = 0; i + 1 < keys.size(); ++i )
            keys[i].Interp = keys[i + 1].Interp;
    }

    std::size_t ShiftInterpToLeavingKey( Timeline::Sequence& sequence )
    {
        std::size_t shifted = 0;
        for ( Timeline::Track& track : sequence.Tracks )
        {
            for ( Timeline::Section& section : track.Sections )
            {
                ShiftInterpToLeavingKey( section.Weight );
                ++shifted;
                auto* channel = std::get_if<Timeline::Channel>( &section.Content );
                if ( channel == nullptr )
                    continue;
                if ( auto* c = std::get_if<Timeline::FloatChannel>( channel ) )
                {
                    ShiftInterpToLeavingKey( c->Keys );
                    ++shifted;
                }
                else if ( auto* c = std::get_if<Timeline::VectorChannel>( channel ) )
                    ShiftVector( *c, shifted );
                else if ( auto* c = std::get_if<Timeline::RotationChannel>( channel ) )
                    ShiftRotation( *c, shifted );
                else if ( auto* c = std::get_if<Timeline::TransformChannel>( channel ) )
                {
                    ShiftVector( c->Translation, shifted );
                    ShiftRotation( c->Rotation, shifted );
                    ShiftVector( c->Scale, shifted );
                }
                else if ( auto* c = std::get_if<Timeline::BoolChannel>( channel ) )
                {
                    ShiftInterpToLeavingKey( c->Bits.Keys );
                    ++shifted;
                }
            }
        }
        return shifted;
    }

    Common::ResultStr<std::size_t> VerifyInterpShift( const Timeline::Sequence& arriving,
                                                      const Timeline::Sequence& leaving )
    {
        if ( arriving.Tracks.size() != leaving.Tracks.size() )
            return Common::MakeFormattedError<std::size_t>( "{} tracks became {}", arriving.Tracks.size(),
                                                            leaving.Tracks.size() );
        const FrameRate rate    = leaving.TickRate;
        std::size_t     samples = 0;
        for ( std::size_t t = 0; t < leaving.Tracks.size(); ++t )
        {
            const Timeline::Track& a = arriving.Tracks[t];
            const Timeline::Track& b = leaving.Tracks[t];
            if ( a.Sections.size() != b.Sections.size() )
                return Common::MakeFormattedError<std::size_t>( "track {} ('{}'): {} sections became {}", t,
                                                                b.Property, a.Sections.size(), b.Sections.size() );
            for ( std::size_t s = 0; s < b.Sections.size(); ++s )
            {
                const Timeline::Section& sa = arriving.Tracks[t].Sections[s];
                const Timeline::Section& sb = leaving.Tracks[t].Sections[s];
                std::string              refusal;
                if ( sa.Weight.size() != sb.Weight.size() )
                    refusal = "the weight's keys changed";
                else if ( !sb.Weight.empty() )
                    refusal = EveryTick(
                         sb.Weight,
                         [&]( const FrameTime at ) -> std::string
                         {
                             const float was =
                                  SampleArriving( Timeline::FloatChannel{ sa.Weight, 1.0F }, at, rate );
                             const float is = Timeline::WeightAt( sb, at, rate );
                             return was == is ? std::string{}
                                              : std::format( "weight at tick {}: {} became {}", at.Frame.Value,
                                                             was, is );
                         },
                         samples );
                if ( refusal.empty() )
                {
                    const auto* ca = std::get_if<Timeline::Channel>( &sa.Content );
                    const auto* cb = std::get_if<Timeline::Channel>( &sb.Content );
                    if ( ( ca == nullptr ) != ( cb == nullptr ) )
                        refusal = "the section changed content kind";
                    else if ( ca != nullptr )
                        refusal = CompareChannel( *ca, *cb, rate, samples );
                }
                if ( !refusal.empty() )
                    return Common::MakeFormattedError<std::size_t>( "track {} ('{}') section {}: {}", t,
                                                                    b.Property, s, refusal );
            }
        }
        return Common::MakeSuccess( samples );
    }
} // namespace Desert::Migration
