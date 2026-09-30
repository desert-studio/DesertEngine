#include "Sequence.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <type_traits>

namespace Desert::Animation::Timeline
{
    const char* ToString( const SequenceHost host )
    {
        switch ( host )
        {
            case SequenceHost::AnimationClip:
                return "AnimationClip";
            case SequenceHost::UIAnimation:
                return "UIAnimation";
            case SequenceHost::LevelSequence:
                return "LevelSequence";
        }
        // A host outside the enum is refused where it is READ; this names it for the log that says so.
        return "Unknown";
    }

    const Binding* FindBinding( const Sequence& sequence, const BindingGuid& guid )
    {
        const auto found = std::find_if( sequence.Bindings.begin(), sequence.Bindings.end(),
                                         [&]( const Binding& binding ) { return binding.Guid == guid; } );
        return found == sequence.Bindings.end() ? nullptr : &*found;
    }

    const Track* FindTrack( const Sequence& sequence, const BindingGuid& binding, const std::string_view property )
    {
        const auto found = std::find_if( sequence.Tracks.begin(), sequence.Tracks.end(), [&]( const Track& track )
                                         { return track.Binding == binding && track.Property == property; } );
        return found == sequence.Tracks.end() ? nullptr : &*found;
    }

    namespace
    {
        using Check = Common::BoolResultStr;

        Check Pass()
        {
            return Common::MakeSuccess( true );
        }

        // ── Keys ──────────────────────────────────────────────────────────────────────────────────────

        /// Sorted by tick, one key per tick: the invariant every sampler's bracket search stands on.
        Check CheckKeyList( const std::vector<ScalarKey>& keys, const std::string_view what )
        {
            for ( size_t i = 1; i < keys.size(); ++i )
            {
                if ( !( keys[i - 1].Tick < keys[i].Tick ) )
                {
                    return Common::MakeFormattedError<bool>(
                         "{}key {} at tick {} does not follow tick {} (keys are sorted, one per tick)", what, i,
                         keys[i].Tick.Value, keys[i - 1].Tick.Value );
                }
            }
            return Pass();
        }

        Check CheckVector( const VectorChannel& channel, const std::string_view what )
        {
            for ( const auto& [component, name] :
                  { std::pair{ &channel.X, "X" }, std::pair{ &channel.Y, "Y" }, std::pair{ &channel.Z, "Z" } } )
            {
                if ( auto check = CheckKeyList( component->Keys, std::format( "{}{} ", what, name ) );
                     !check.IsSuccess() )
                {
                    return check;
                }
            }
            return Pass();
        }

        /// Four components keyed together (same ticks, same interp per key), Constant or Linear only.
        Check CheckRotation( const RotationChannel& channel, const std::string_view what )
        {
            const std::pair<const FloatChannel*, const char*> components[] = {
                 { &channel.X, "X" }, { &channel.Y, "Y" }, { &channel.Z, "Z" }, { &channel.W, "W" } };
            for ( const auto& [component, name] : components )
            {
                if ( auto check = CheckKeyList( component->Keys, std::format( "{}{} ", what, name ) );
                     !check.IsSuccess() )
                {
                    return check;
                }
                if ( component->Keys.size() != channel.X.Keys.size() )
                {
                    return Common::MakeFormattedError<bool>( "{}rotation keys are not aligned (X has {} ticks, {} "
                                                             "has {})",
                                                             what, channel.X.Keys.size(), name,
                                                             component->Keys.size() );
                }
            }
            for ( size_t i = 0; i < channel.X.Keys.size(); ++i )
            {
                const ScalarKey& x = channel.X.Keys[i];
                for ( const auto& [component, name] : components )
                {
                    const ScalarKey& key = component->Keys[i];
                    if ( key.Tick != x.Tick || key.Interp != x.Interp )
                    {
                        return Common::MakeFormattedError<bool>(
                             "{}rotation key {} is not aligned (X at tick {} {}, {} at tick {} {})", what, i,
                             x.Tick.Value, ToString( x.Interp ), name, key.Tick.Value, ToString( key.Interp ) );
                    }
                }
                if ( x.Interp == KeyInterp::Cubic )
                {
                    return Common::MakeFormattedError<bool>(
                         "{}rotation key {} is Cubic; a rotation interpolates Constant or Linear (slerp) until "
                         "squad lands",
                         what, i );
                }
            }
            return Pass();
        }

        Check CheckBool( const BoolChannel& channel )
        {
            if ( auto check = CheckKeyList( channel.Bits.Keys, "bool " ); !check.IsSuccess() )
            {
                return check;
            }
            for ( size_t i = 0; i < channel.Bits.Keys.size(); ++i )
            {
                const ScalarKey& key = channel.Bits.Keys[i];
                if ( key.Interp != KeyInterp::Constant || ( key.Value != 0.0F && key.Value != 1.0F ) )
                {
                    return Common::MakeFormattedError<bool>(
                         "bool key {} is {} with value {}; a bool key is Constant and exactly 0 or 1", i,
                         ToString( key.Interp ), key.Value );
                }
            }
            return Pass();
        }

        Check CheckEvents( const EventChannel& channel )
        {
            for ( size_t i = 0; i < channel.Keys.size(); ++i )
            {
                const EventKey& key = channel.Keys[i];
                if ( key.Duration.Value < 0 )
                {
                    return Common::MakeFormattedError<bool>( "event {} '{}' lasts {} ticks; a duration is >= 0", i,
                                                             key.Name, key.Duration.Value );
                }
                if ( i > 0 && key.Tick < channel.Keys[i - 1].Tick )
                {
                    return Common::MakeFormattedError<bool>(
                         "event {} '{}' at tick {} comes before tick {} (events are sorted by tick)", i, key.Name,
                         key.Tick.Value, channel.Keys[i - 1].Tick.Value );
                }
            }
            return Pass();
        }

        Check CheckChannel( const Channel& channel )
        {
            return std::visit(
                 []( const auto& held ) -> Check
                 {
                     using Held = std::decay_t<decltype( held )>;
                     if constexpr ( std::is_same_v<Held, FloatChannel> )
                     {
                         return CheckKeyList( held.Keys, "" );
                     }
                     else if constexpr ( std::is_same_v<Held, VectorChannel> )
                     {
                         return CheckVector( held, "" );
                     }
                     else if constexpr ( std::is_same_v<Held, RotationChannel> )
                     {
                         return CheckRotation( held, "" );
                     }
                     else if constexpr ( std::is_same_v<Held, TransformChannel> )
                     {
                         if ( auto check = CheckVector( held.Translation, "translation " ); !check.IsSuccess() )
                         {
                             return check;
                         }
                         if ( auto check = CheckRotation( held.Rotation, "" ); !check.IsSuccess() )
                         {
                             return check;
                         }
                         return CheckVector( held.Scale, "scale " );
                     }
                     else if constexpr ( std::is_same_v<Held, BoolChannel> )
                     {
                         return CheckBool( held );
                     }
                     else
                     {
                         static_assert( std::is_same_v<Held, EventChannel> );
                         return CheckEvents( held );
                     }
                 },
                 channel );
        }

        // ── The host's restrictions (Sequence.hpp's table) ────────────────────────────────────────────

        /// May @p host's sequence hold a binding of @p kind at all?
        bool HostHoldsBinding( const SequenceHost host, const BindingKind kind )
        {
            switch ( host )
            {
                case SequenceHost::AnimationClip:
                    return kind == BindingKind::Bone || kind == BindingKind::Sequence;
                case SequenceHost::UIAnimation:
                    return kind == BindingKind::Widget;
                case SequenceHost::LevelSequence:
                    return kind == BindingKind::Sequence || kind == BindingKind::Entity ||
                           kind == BindingKind::Bone;
            }
            return false;
        }

        /**
         * May a track of @p track kind sit on a binding of @p binding kind in @p host's sequence?
         *
         * Bones take transforms only; the master (Sequence) binding takes the sequence-level tracks — a
         * clip's named float curves and its one notify track, a level sequence's Camera Cut and events;
         * a skeletal Animation section plays on an entity; a UI clip animates Vector/Float properties.
         */
        bool HostHoldsTrack( const SequenceHost host, const BindingKind binding, const TrackKind track )
        {
            if ( binding == BindingKind::Bone )
            {
                return track == TrackKind::Transform;
            }
            switch ( host )
            {
                case SequenceHost::AnimationClip:
                    return binding == BindingKind::Sequence &&
                           ( track == TrackKind::Float || track == TrackKind::Event );
                case SequenceHost::UIAnimation:
                    return binding == BindingKind::Widget &&
                           ( track == TrackKind::Vector || track == TrackKind::Float );
                case SequenceHost::LevelSequence:
                    if ( binding == BindingKind::Sequence )
                    {
                        return track == TrackKind::CameraCut || track == TrackKind::Event;
                    }
                    return binding == BindingKind::Entity && track != TrackKind::CameraCut &&
                           track != TrackKind::Event;
            }
            return false;
        }

        // ── Bindings ──────────────────────────────────────────────────────────────────────────────────

        Check CheckBindings( const Sequence& sequence )
        {
            const auto fail = [&]( const size_t index, const std::string& why ) {
                return Common::MakeFormattedError<bool>( "binding {} '{}': {}", index,
                                                         sequence.Bindings[index].Label, why );
            };
            for ( size_t i = 0; i < sequence.Bindings.size(); ++i )
            {
                const Binding& binding = sequence.Bindings[i];
                if ( binding.Guid.IsNull() )
                {
                    return fail( i, "the GUID is null" );
                }
                for ( size_t j = 0; j < i; ++j )
                {
                    if ( sequence.Bindings[j].Guid == binding.Guid )
                    {
                        return fail( i, std::format( "the GUID is binding {}'s too", j ) );
                    }
                }
                if ( ( binding.Kind == BindingKind::Sequence ) != binding.Locator.empty() )
                {
                    return fail(
                         i, binding.Kind == BindingKind::Sequence
                                 ? std::format( "a Sequence binding names nothing, yet the locator is '{}'",
                                                binding.Locator )
                                 : std::format( "a {} binding has an empty locator", ToString( binding.Kind ) ) );
                }
                if ( !HostHoldsBinding( sequence.Host, binding.Kind ) )
                {
                    return fail( i, std::format( "a {} cannot hold a {} binding", ToString( sequence.Host ),
                                                 ToString( binding.Kind ) ) );
                }
                // Any number of Widget bindings (UE's UWidgetAnimation binds any widget of its tree): that each
                // lies inside the owner's tree is the UI host's Resolve, and a stray one lands in ApplyReport.
                if ( sequence.Host == SequenceHost::AnimationClip && binding.Kind == BindingKind::Bone )
                {
                    for ( size_t j = 0; j < i; ++j )
                    {
                        const Binding& other = sequence.Bindings[j];
                        if ( other.Kind == BindingKind::Bone && other.Locator == binding.Locator )
                        {
                            return fail( i, std::format( "bone '{}' is bound twice (binding {} too)",
                                                         binding.Locator, j ) );
                        }
                    }
                }

                // The parent chain: every link a binding of this sequence, no cycle. A bone in a level
                // sequence belongs to the entity whose skeleton it is.
                if ( sequence.Host == SequenceHost::LevelSequence && binding.Kind == BindingKind::Bone )
                {
                    const Binding* parent =
                         binding.Parent.IsNull() ? nullptr : FindBinding( sequence, binding.Parent );
                    if ( parent == nullptr || parent->Kind != BindingKind::Entity )
                    {
                        return fail( i, "a bone in a level sequence sits under the Entity binding it belongs to" );
                    }
                }
                const Binding* link = &binding;
                for ( size_t depth = 0; !link->Parent.IsNull(); ++depth )
                {
                    const Binding* parent = FindBinding( sequence, link->Parent );
                    if ( parent == nullptr )
                    {
                        return fail( i, "a parent in its chain is not a binding of this sequence" );
                    }
                    if ( parent == &binding || depth >= sequence.Bindings.size() )
                    {
                        return fail( i, "its parent chain is a cycle" );
                    }
                    link = parent;
                }
            }
            return Pass();
        }

        // ── Sections ──────────────────────────────────────────────────────────────────────────────────

        Check CheckSection( const Sequence& sequence, const Track& track, const Section& section )
        {
            if ( section.End < section.Start )
            {
                return Common::MakeFormattedError<bool>( "ends at tick {} before it starts at tick {}",
                                                         section.End.Value, section.Start.Value );
            }
            if ( auto check = CheckKeyList( section.Weight, "weight " ); !check.IsSuccess() )
            {
                return check;
            }
            const TrackKind held = TrackKindOf( section.Content );
            if ( held != track.Kind )
            {
                return Common::MakeFormattedError<bool>( "holds {} content on a {} track", ToString( held ),
                                                         ToString( track.Kind ) );
            }
            if ( track.Kind == TrackKind::Bool && section.Blend == SectionBlendType::Additive )
            {
                return Common::MakeFormattedError<bool>(
                     "an Additive section on a Bool track: a flag has no additive — make the section Absolute" );
            }
            if ( const auto* channel = std::get_if<Channel>( &section.Content ) )
            {
                return CheckChannel( *channel );
            }
            if ( const auto* animation = std::get_if<AnimationSectionContent>( &section.Content ) )
            {
                if ( animation->Clip.IsNull() )
                {
                    return Common::MakeFormattedError<bool>( "an Animation section names no clip" );
                }
                if ( !std::isfinite( animation->PlayRate ) || !( animation->PlayRate > 0.0 ) )
                {
                    return Common::MakeFormattedError<bool>( "play rate {} is not a positive rate",
                                                             animation->PlayRate );
                }
                return Pass();
            }
            const auto& cut = std::get<CameraCutSectionContent>( section.Content );
            if ( section.Blend != SectionBlendType::Absolute || !section.Weight.empty() )
            {
                return Common::MakeFormattedError<bool>( "a Camera Cut is Absolute at full weight" );
            }
            const Binding* camera = FindBinding( sequence, cut.Camera );
            if ( camera == nullptr || camera->Kind != BindingKind::Entity )
            {
                return Common::MakeFormattedError<bool>( "the camera is not an Entity binding of this sequence" );
            }
            return Pass();
        }

        /// Camera cuts on one row never overlap: at every tick one camera speaks.
        Check CheckCutRows( const Track& track )
        {
            for ( size_t i = 0; i < track.Sections.size(); ++i )
            {
                for ( size_t j = 0; j < i; ++j )
                {
                    const Section& a = track.Sections[i];
                    const Section& b = track.Sections[j];
                    if ( a.Row == b.Row && !( a.End < b.Start ) && !( b.End < a.Start ) )
                    {
                        return Common::MakeFormattedError<bool>(
                             "section {}: overlaps Camera Cut section {} on row {}", i, j, a.Row );
                    }
                }
            }
            return Pass();
        }

        // ── Tracks ────────────────────────────────────────────────────────────────────────────────────

        Check CheckTracks( const Sequence& sequence )
        {
            size_t clipEventTracks = 0;
            for ( size_t t = 0; t < sequence.Tracks.size(); ++t )
            {
                const Track&   track   = sequence.Tracks[t];
                const Binding* binding = FindBinding( sequence, track.Binding );
                const auto     fail    = [&]( const std::string& why )
                {
                    return Common::MakeFormattedError<bool>( "track '{}' / '{}': {}",
                                                             binding != nullptr ? binding->Label : "<no binding>",
                                                             track.Property, why );
                };
                if ( binding == nullptr )
                {
                    return fail( std::format( "track {} points at no binding of this sequence", t ) );
                }
                if ( !HostHoldsTrack( sequence.Host, binding->Kind, track.Kind ) )
                {
                    return fail( std::format( "a {} cannot hold a {} track on a {} binding",
                                              ToString( sequence.Host ), ToString( track.Kind ),
                                              ToString( binding->Kind ) ) );
                }
                if ( sequence.Host == SequenceHost::AnimationClip && track.Kind == TrackKind::Event &&
                     ++clipEventTracks > 1 )
                {
                    return fail( "a clip has one notify (Event) track" );
                }
                for ( size_t other = 0; other < t; ++other )
                {
                    const Track& earlier = sequence.Tracks[other];
                    if ( earlier.Binding == track.Binding && earlier.Property == track.Property &&
                         earlier.Kind == track.Kind )
                    {
                        return fail(
                             std::format( "track {} drives the same property as track {}: two answers with "
                                          "no rule between them (sections are the rule)",
                                          t, other ) );
                    }
                }
                for ( size_t s = 0; s < track.Sections.size(); ++s )
                {
                    if ( auto check = CheckSection( sequence, track, track.Sections[s] ); !check.IsSuccess() )
                    {
                        return fail( std::format( "section {}: {}", s, check.GetError() ) );
                    }
                }
                if ( track.Kind == TrackKind::CameraCut )
                {
                    if ( auto check = CheckCutRows( track ); !check.IsSuccess() )
                    {
                        return fail( check.GetError() );
                    }
                }
            }
            return Pass();
        }
    } // namespace

    Common::BoolResultStr Validate( const Sequence& sequence )
    {
        if ( !sequence.TickRate.IsValid() || !sequence.DisplayRate.IsValid() )
        {
            return Common::MakeFormattedError<bool>(
                 "tick rate {}/{} or display rate {}/{} is not a rate", sequence.TickRate.Numerator,
                 sequence.TickRate.Denominator, sequence.DisplayRate.Numerator, sequence.DisplayRate.Denominator );
        }
        if ( sequence.End < sequence.Start )
        {
            return Common::MakeFormattedError<bool>(
                 "the playback range ends at tick {} before it starts at tick {}", sequence.End.Value,
                 sequence.Start.Value );
        }
        if ( auto check = CheckBindings( sequence ); !check.IsSuccess() )
        {
            return check;
        }
        return CheckTracks( sequence );
    }
} // namespace Desert::Animation::Timeline
