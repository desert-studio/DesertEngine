// THE TIMELINE FORMAT, TMLN v1: ONE WRITER AND ONE READER FOR EVERY HOST (Sequence.hpp §Serialization).
//
// A text block, like every other text asset: a JSON document laid out by the one canonical writer
// (Common/Content/CanonicalText.hpp) whose FIRST member is the text asset header, which states the version
// under `TMLN` — the one place a version is stated, read by the same gate every text asset has. The block
// states no identity of its own (Kind and Guid empty): the host asset around it is the thing with an
// identity. Its Dependencies are the clips its Animation sections play, so a dependency walk reads them
// from the header prefix without parsing a key.
//
// THE LAYOUT MIRRORS THE DATA, with the contract's rules for what is stored how:
//   * every enum as its integer (their orders are append-only);
//   * GUIDs as 32 hex (AssetGuidToText); a null Parent as the empty string;
//   * a key as ONE array of the ScalarKey fields in declaration order, reserved weights included —
//     [Tick, Value, ArriveTangent, LeaveTangent, ArriveWeight, LeaveWeight, Interp, Mode] — which the
//     canonical writer keeps on one line, so an edited key is a one-line diff;
//   * a channel as its kind plus its FloatChannel components in declaration order (Float 1, Vector XYZ,
//     Rotation XYZW, Transform T.XYZ R.XYZW S.XYZ, Bool 1) or its event keys — one shape for all six.

#include "Sequence.hpp"

#include <Engine/Assets/TextAssetHeaderStamp.hpp>

#include <Common/Content/CanonicalText.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Json/Json.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace Desert::Animation::Timeline::Format
{
    using KeyData = std::tuple<int32_t, float, float, float, float, float, uint8_t, uint8_t>;

    struct FloatChannelData
    {
        std::vector<KeyData> Keys;
        float                Default = 0.0F;
    };

    struct EventKeyData
    {
        int32_t     Tick     = 0;
        int32_t     Duration = 0;
        std::string Name;
        int32_t     Row = 0;
    };

    struct ChannelData
    {
        uint8_t                       Kind = 0;
        std::vector<FloatChannelData> Components;
        std::vector<EventKeyData>     Events;
    };

    struct AnimationData
    {
        std::string Clip;
        int32_t     StartOffset = 0;
        double      PlayRate    = 1.0;
        bool        Loop        = false;
    };

    struct CameraCutData
    {
        std::string Camera;
    };

    /// Exactly one of the three content members is present — the SectionContent alternative.
    struct SectionData
    {
        int32_t                      Start = 0;
        int32_t                      End   = 0;
        uint8_t                      Blend = 0;
        std::vector<KeyData>         Weight;
        int32_t                      Row = 0;
        std::string                  Name;
        std::optional<ChannelData>   Channel;
        std::optional<AnimationData> Animation;
        std::optional<CameraCutData> CameraCut;
    };

    struct TrackData
    {
        std::string              Binding;
        std::string              Property;
        uint8_t                  Kind  = 0;
        bool                     Muted = false;
        std::vector<SectionData> Sections;
    };

    struct BindingData
    {
        std::string Guid;
        uint8_t     Kind = 0;
        std::string Locator;
        std::string Label;
        std::string Parent;
    };

    struct SequenceData
    {
        Common::Content::TextAssetHeaderSerialized Header;
        uint8_t                                    Host = 0;
        std::array<int32_t, 2>                     TickRate{};
        std::array<int32_t, 2>                     DisplayRate{};
        int32_t                                    Start = 0;
        int32_t                                    End   = 0;
        std::vector<BindingData>                   Bindings;
        std::vector<TrackData>                     Tracks;
    };
} // namespace Desert::Animation::Timeline::Format

namespace Desert::Animation::Timeline
{
    namespace
    {
        using namespace Format;
        using Common::Content::AssetGuidFromText;
        using Common::Content::AssetGuidToText;

        template <typename T>
        using Result = Common::ResultStr<T>;

        // ── Write ─────────────────────────────────────────────────────────────────────────────────────

        [[nodiscard]] KeyData ToData( const ScalarKey& key )
        {
            return { key.Tick.Value,
                     key.Value,
                     key.ArriveTangent,
                     key.LeaveTangent,
                     key.ArriveWeight,
                     key.LeaveWeight,
                     static_cast<uint8_t>( key.Interp ),
                     static_cast<uint8_t>( key.Mode ) };
        }

        [[nodiscard]] std::vector<KeyData> ToData( const std::vector<ScalarKey>& keys )
        {
            std::vector<KeyData> out;
            out.reserve( keys.size() );
            for ( const ScalarKey& key : keys )
            {
                out.push_back( ToData( key ) );
            }
            return out;
        }

        [[nodiscard]] FloatChannelData ToData( const FloatChannel& channel )
        {
            return { ToData( channel.Keys ), channel.Default };
        }

        void Append( std::vector<FloatChannelData>& out, const VectorChannel& channel )
        {
            out.push_back( ToData( channel.X ) );
            out.push_back( ToData( channel.Y ) );
            out.push_back( ToData( channel.Z ) );
        }

        void Append( std::vector<FloatChannelData>& out, const RotationChannel& channel )
        {
            out.push_back( ToData( channel.X ) );
            out.push_back( ToData( channel.Y ) );
            out.push_back( ToData( channel.Z ) );
            out.push_back( ToData( channel.W ) );
        }

        [[nodiscard]] ChannelData ToData( const Channel& channel )
        {
            ChannelData out;
            out.Kind = static_cast<uint8_t>( KindOf( channel ) );
            std::visit(
                 [&]<typename T>( const T& typed )
                 {
                     if constexpr ( std::is_same_v<T, FloatChannel> )
                     {
                         out.Components.push_back( ToData( typed ) );
                     }
                     else if constexpr ( std::is_same_v<T, VectorChannel> || std::is_same_v<T, RotationChannel> )
                     {
                         Append( out.Components, typed );
                     }
                     else if constexpr ( std::is_same_v<T, TransformChannel> )
                     {
                         Append( out.Components, typed.Translation );
                         Append( out.Components, typed.Rotation );
                         Append( out.Components, typed.Scale );
                     }
                     else if constexpr ( std::is_same_v<T, BoolChannel> )
                     {
                         out.Components.push_back( ToData( typed.Bits ) );
                     }
                     else
                     {
                         for ( const EventKey& key : typed.Keys )
                         {
                             out.Events.push_back(
                                  EventKeyData{ key.Tick.Value, key.Duration.Value, key.Name, key.Row } );
                         }
                     }
                 },
                 channel );
            return out;
        }

        [[nodiscard]] std::string ToText( const BindingGuid& guid )
        {
            return guid.IsNull() ? std::string() : AssetGuidToText( guid.Value );
        }

        [[nodiscard]] SectionData ToData( const Section& section, std::vector<std::string>& clips )
        {
            SectionData out;
            out.Start  = section.Start.Value;
            out.End    = section.End.Value;
            out.Blend  = static_cast<uint8_t>( section.Blend );
            out.Weight = ToData( section.Weight );
            out.Row    = section.Row;
            out.Name   = section.Name;
            if ( const auto* channel = std::get_if<Channel>( &section.Content ) )
            {
                out.Channel = ToData( *channel );
            }
            else if ( const auto* anim = std::get_if<AnimationSectionContent>( &section.Content ) )
            {
                out.Animation = AnimationData{ AssetGuidToText( anim->Clip ), anim->StartOffset.Value,
                                               anim->PlayRate, anim->Loop };
                clips.push_back( out.Animation->Clip );
            }
            else
            {
                out.CameraCut =
                     CameraCutData{ ToText( std::get<CameraCutSectionContent>( section.Content ).Camera ) };
            }
            return out;
        }

        // ── Read ──────────────────────────────────────────────────────────────────────────────────────

        [[nodiscard]] ScalarKey FromData( const KeyData& data )
        {
            ScalarKey key;
            key.Tick          = FrameNumber{ std::get<0>( data ) };
            key.Value         = std::get<1>( data );
            key.ArriveTangent = std::get<2>( data );
            key.LeaveTangent  = std::get<3>( data );
            key.ArriveWeight  = std::get<4>( data );
            key.LeaveWeight   = std::get<5>( data );
            key.Interp        = static_cast<KeyInterp>( std::get<6>( data ) );
            key.Mode          = static_cast<TangentMode>( std::get<7>( data ) );
            return key;
        }

        [[nodiscard]] std::vector<ScalarKey> FromData( const std::vector<KeyData>& data )
        {
            std::vector<ScalarKey> keys;
            keys.reserve( data.size() );
            for ( const KeyData& key : data )
            {
                keys.push_back( FromData( key ) );
            }
            return keys;
        }

        [[nodiscard]] FloatChannel FromData( const FloatChannelData& data )
        {
            return FloatChannel{ FromData( data.Keys ), data.Default };
        }

        [[nodiscard]] size_t ComponentsOf( const ChannelKind kind )
        {
            switch ( kind )
            {
                case ChannelKind::Float:
                case ChannelKind::Bool:
                    return 1;
                case ChannelKind::Vector:
                    return 3;
                case ChannelKind::Rotation:
                    return 4;
                case ChannelKind::Transform:
                    return 10;
                case ChannelKind::Event:
                    return 0;
            }
            return 0;
        }

        [[nodiscard]] Result<Channel> FromData( const ChannelData& data )
        {
            if ( data.Kind > static_cast<uint8_t>( ChannelKind::Event ) )
            {
                return Common::MakeFormattedError<Channel>( "unknown channel kind {}", data.Kind );
            }
            const auto kind = static_cast<ChannelKind>( data.Kind );
            if ( data.Components.size() != ComponentsOf( kind ) )
            {
                return Common::MakeFormattedError<Channel>( "a {} channel has {} components, not {}",
                                                            ToString( kind ), ComponentsOf( kind ),
                                                            data.Components.size() );
            }
            if ( kind != ChannelKind::Event && !data.Events.empty() )
            {
                return Common::MakeFormattedError<Channel>( "a {} channel holds event keys", ToString( kind ) );
            }
            const auto& c      = data.Components;
            const auto  vector = [&]( const size_t i )
            { return VectorChannel{ FromData( c[i] ), FromData( c[i + 1] ), FromData( c[i + 2] ) }; };
            const auto rotation = [&]( const size_t i ) {
                return RotationChannel{ FromData( c[i] ), FromData( c[i + 1] ), FromData( c[i + 2] ),
                                        FromData( c[i + 3] ) };
            };
            switch ( kind )
            {
                case ChannelKind::Float:
                    return Common::MakeSuccess( Channel{ FromData( c[0] ) } );
                case ChannelKind::Vector:
                    return Common::MakeSuccess( Channel{ vector( 0 ) } );
                case ChannelKind::Rotation:
                    return Common::MakeSuccess( Channel{ rotation( 0 ) } );
                case ChannelKind::Transform:
                    return Common::MakeSuccess(
                         Channel{ TransformChannel{ vector( 0 ), rotation( 3 ), vector( 7 ) } } );
                case ChannelKind::Bool:
                    return Common::MakeSuccess( Channel{ BoolChannel{ FromData( c[0] ) } } );
                case ChannelKind::Event:
                    break;
            }
            EventChannel events;
            for ( const EventKeyData& key : data.Events )
            {
                events.Keys.push_back(
                     EventKey{ FrameNumber{ key.Tick }, FrameNumber{ key.Duration }, key.Name, key.Row } );
            }
            return Common::MakeSuccess( Channel{ std::move( events ) } );
        }

        [[nodiscard]] Result<BindingGuid> GuidFromText( const std::string& text, const bool nullable )
        {
            if ( text.empty() && nullable )
            {
                return Common::MakeSuccess( BindingGuid{} );
            }
            auto guid = AssetGuidFromText( text );
            if ( !guid )
            {
                return Common::MakeError<BindingGuid>( guid.GetError() );
            }
            return Common::MakeSuccess( BindingGuid{ guid.GetValue() } );
        }

        [[nodiscard]] Result<Section> FromData( const SectionData& data )
        {
            Section section;
            section.Start  = FrameNumber{ data.Start };
            section.End    = FrameNumber{ data.End };
            section.Weight = FromData( data.Weight );
            section.Row    = data.Row;
            section.Name   = data.Name;
            if ( data.Blend > static_cast<uint8_t>( SectionBlendType::Additive ) )
            {
                return Common::MakeFormattedError<Section>( "unknown blend type {}", data.Blend );
            }
            section.Blend = static_cast<SectionBlendType>( data.Blend );

            const int contents = int( data.Channel.has_value() ) + int( data.Animation.has_value() ) +
                                 int( data.CameraCut.has_value() );
            if ( contents != 1 )
            {
                return Common::MakeFormattedError<Section>(
                     "states {} of Channel / Animation / CameraCut; a section holds exactly one", contents );
            }
            if ( data.Channel )
            {
                auto channel = FromData( *data.Channel );
                if ( !channel )
                {
                    return Common::MakeError<Section>( channel.GetError() );
                }
                section.Content = std::move( channel.GetValue() );
            }
            else if ( data.Animation )
            {
                auto clip = AssetGuidFromText( data.Animation->Clip );
                if ( !clip )
                {
                    return Common::MakeFormattedError<Section>( "Animation clip: {}", clip.GetError() );
                }
                section.Content =
                     AnimationSectionContent{ clip.GetValue(), FrameNumber{ data.Animation->StartOffset },
                                              data.Animation->PlayRate, data.Animation->Loop };
            }
            else
            {
                auto camera = GuidFromText( data.CameraCut->Camera, false );
                if ( !camera )
                {
                    return Common::MakeFormattedError<Section>( "Camera Cut camera: {}", camera.GetError() );
                }
                section.Content = CameraCutSectionContent{ camera.GetValue() };
            }
            return Common::MakeSuccess( std::move( section ) );
        }

        [[nodiscard]] Result<Sequence> FromData( const SequenceData& data )
        {
            Sequence sequence;
            if ( data.Host > static_cast<uint8_t>( SequenceHost::LevelSequence ) )
            {
                return Common::MakeFormattedError<Sequence>( "unknown host {}", data.Host );
            }
            sequence.Host        = static_cast<SequenceHost>( data.Host );
            sequence.TickRate    = FrameRate{ data.TickRate[0], data.TickRate[1] };
            sequence.DisplayRate = FrameRate{ data.DisplayRate[0], data.DisplayRate[1] };
            sequence.Start       = FrameNumber{ data.Start };
            sequence.End         = FrameNumber{ data.End };

            for ( size_t i = 0; i < data.Bindings.size(); ++i )
            {
                const BindingData& in = data.Bindings[i];
                Binding            binding;
                auto               guid   = GuidFromText( in.Guid, false );
                auto               parent = GuidFromText( in.Parent, true );
                if ( !guid || !parent )
                {
                    return Common::MakeFormattedError<Sequence>( "binding {} '{}': {}", i, in.Label,
                                                                 !guid ? guid.GetError() : parent.GetError() );
                }
                if ( in.Kind > static_cast<uint8_t>( BindingKind::Widget ) )
                {
                    return Common::MakeFormattedError<Sequence>( "binding {} '{}': unknown kind {}", i, in.Label,
                                                                 in.Kind );
                }
                binding.Guid    = guid.GetValue();
                binding.Kind    = static_cast<BindingKind>( in.Kind );
                binding.Locator = in.Locator;
                binding.Label   = in.Label;
                binding.Parent  = parent.GetValue();
                sequence.Bindings.push_back( std::move( binding ) );
            }

            for ( size_t t = 0; t < data.Tracks.size(); ++t )
            {
                const TrackData& in   = data.Tracks[t];
                auto             guid = GuidFromText( in.Binding, false );
                if ( !guid )
                {
                    return Common::MakeFormattedError<Sequence>( "track {} '{}': {}", t, in.Property,
                                                                 guid.GetError() );
                }
                if ( in.Kind > static_cast<uint8_t>( TrackKind::CameraCut ) )
                {
                    return Common::MakeFormattedError<Sequence>( "track {} '{}': unknown kind {}", t, in.Property,
                                                                 in.Kind );
                }
                Track track;
                track.Binding  = guid.GetValue();
                track.Property = in.Property;
                track.Kind     = static_cast<TrackKind>( in.Kind );
                track.Muted    = in.Muted;
                for ( size_t s = 0; s < in.Sections.size(); ++s )
                {
                    auto section = FromData( in.Sections[s] );
                    if ( !section )
                    {
                        return Common::MakeFormattedError<Sequence>( "track {} '{}' section {}: {}", t,
                                                                     in.Property, s, section.GetError() );
                    }
                    track.Sections.push_back( std::move( section.GetValue() ) );
                }
                sequence.Tracks.push_back( std::move( track ) );
            }
            return Common::MakeSuccess( std::move( sequence ) );
        }
    } // namespace

    Common::ResultStr<std::vector<uint8_t>> WriteSequence( const Sequence& sequence )
    {
        SequenceData data;
        data.Host        = static_cast<uint8_t>( sequence.Host );
        data.TickRate    = { sequence.TickRate.Numerator, sequence.TickRate.Denominator };
        data.DisplayRate = { sequence.DisplayRate.Numerator, sequence.DisplayRate.Denominator };
        data.Start       = sequence.Start.Value;
        data.End         = sequence.End.Value;
        for ( const Binding& binding : sequence.Bindings )
        {
            data.Bindings.push_back( BindingData{ ToText( binding.Guid ), static_cast<uint8_t>( binding.Kind ),
                                                  binding.Locator, binding.Label, ToText( binding.Parent ) } );
        }
        std::vector<std::string> clips;
        for ( const Track& track : sequence.Tracks )
        {
            TrackData out{
                 ToText( track.Binding ), track.Property, static_cast<uint8_t>( track.Kind ), track.Muted, {} };
            for ( const Section& section : track.Sections )
            {
                out.Sections.push_back( ToData( section, clips ) );
            }
            data.Tracks.push_back( std::move( out ) );
        }

        // The header: the version under TMLN, the played clips as dependencies (sorted, once each, so the same
        // sequence always writes the same bytes).
        const Common::Content::SubsystemVersion version{ Assets::kTimelineSchemaTag, kTimelineFormatVersion };
        data.Header.Versions[Common::Content::FourCCToString( version.Tag )] = version.Version;
        std::ranges::sort( clips );
        clips.erase( std::unique( clips.begin(), clips.end() ), clips.end() );
        data.Header.Dependencies = std::move( clips );

        // A layout failure is REFUSED with its reason (lead decision 09-30): an error text written as the block
        // would be a file that says nothing about the sequence, found only by the next reader.
        const std::string                    raw  = Common::Json::Write( data );
        const Common::ResultStr<std::string> text = Common::Content::CanonicalJsonTextOfWriterOutput( raw );
        if ( !text )
        {
            return Common::MakeFormattedError<std::vector<uint8_t>>( "timeline block: {}", text.GetError() );
        }
        return Common::MakeSuccess( std::vector<uint8_t>( text.GetValue().begin(), text.GetValue().end() ) );
    }

    Common::ResultStr<Sequence> ReadSequence( const std::span<const uint8_t> bytes )
    {
        const std::string_view text( reinterpret_cast<const char*>( bytes.data() ), bytes.size() );
        auto                   parsed = Common::Json::Read<SequenceData>( text );
        if ( !parsed )
        {
            return Common::MakeFormattedError<Sequence>( "timeline block ({} bytes) is not a TMLN document: {}",
                                                         bytes.size(), parsed.GetError() );
        }
        const auto stated =
             Common::Content::TextHeaderVersion( parsed.GetValue().Header, Assets::kTimelineSchemaTag );
        if ( !stated )
        {
            return Common::MakeFormattedError<Sequence>( "timeline block states no TMLN version in its header" );
        }
        if ( *stated == kTimelineLastArrivingInterpVersion )
        {
            return Common::MakeFormattedError<Sequence>(
                 "timeline block is TMLN v{}: its key modes shape the segment ARRIVING at a key, v{} the segment "
                 "leaving it; run SceneMigrator (scripts/Dev/migrate.sh --write) to shift them",
                 *stated, kTimelineFormatVersion );
        }
        if ( *stated != kTimelineFormatVersion )
        {
            return Common::MakeFormattedError<Sequence>( "timeline block is TMLN v{}; this build reads v{} only",
                                                         *stated, kTimelineFormatVersion );
        }
        auto sequence = FromData( parsed.GetValue() );
        if ( !sequence )
        {
            return Common::MakeFormattedError<Sequence>( "timeline block: {}", sequence.GetError() );
        }
        if ( auto valid = Validate( sequence.GetValue() ); !valid.IsSuccess() )
        {
            return Common::MakeFormattedError<Sequence>( "timeline block: {}", valid.GetError() );
        }
        return sequence;
    }
} // namespace Desert::Animation::Timeline
