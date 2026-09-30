#include "TrackEditing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <variant>

#include <glm/gtx/matrix_decompose.hpp>

namespace Desert::Animation
{
    namespace
    {
        using Timeline::FloatChannel;
        using Timeline::TransformChannel;
        using Timeline::VectorChannel;

        /// The components one part of the channel is made of: three for translation and scale, the four
        /// quaternion components (keyed together) for rotation.
        std::vector<FloatChannel*> ComponentsOf( TransformChannel& channel, TrackChannel part )
        {
            switch ( part )
            {
                case TrackChannel::Position:
                    return { &channel.Translation.X, &channel.Translation.Y, &channel.Translation.Z };
                case TrackChannel::Scale:
                    return { &channel.Scale.X, &channel.Scale.Y, &channel.Scale.Z };
                case TrackChannel::Rotation:
                    return { &channel.Rotation.X, &channel.Rotation.Y, &channel.Rotation.Z, &channel.Rotation.W };
            }
            return {};
        }

        std::vector<const FloatChannel*> ComponentsOf( const TransformChannel& channel, TrackChannel part )
        {
            switch ( part )
            {
                case TrackChannel::Position:
                    return { &channel.Translation.X, &channel.Translation.Y, &channel.Translation.Z };
                case TrackChannel::Scale:
                    return { &channel.Scale.X, &channel.Scale.Y, &channel.Scale.Z };
                case TrackChannel::Rotation:
                    return { &channel.Rotation.X, &channel.Rotation.Y, &channel.Rotation.Z, &channel.Rotation.W };
            }
            return {};
        }

        [[nodiscard]] ScalarKey* KeyOn( FloatChannel& component, FrameNumber tick )
        {
            const auto it =
                 std::lower_bound( component.Keys.begin(), component.Keys.end(), tick,
                                   []( const ScalarKey& key, FrameNumber at ) { return key.Tick < at; } );
            return ( it != component.Keys.end() && it->Tick == tick ) ? &*it : nullptr;
        }

        /// Insert keeping the channel's invariant (sorted by tick, one key per tick). The caller has checked
        /// the tick is free.
        void InsertSorted( FloatChannel& component, const ScalarKey& key )
        {
            const auto it = std::lower_bound( component.Keys.begin(), component.Keys.end(), key.Tick,
                                              []( const ScalarKey& k, FrameNumber at ) { return k.Tick < at; } );
            component.Keys.insert( it, key );
        }

        /// The mode of the segment a key inserted at @p tick SPLITS — its earlier key's (UE's rule: a key's
        /// mode shapes the segment leaving it). The inserted key takes it, so both halves keep the shape the
        /// segment had. Outside the keyed range the channel holds a constant, and @p outside is the mode.
        KeyInterp SplitSegmentInterp( const std::vector<ScalarKey>& keys, FrameNumber tick, KeyInterp outside )
        {
            const auto after = std::lower_bound( keys.begin(), keys.end(), tick,
                                                 []( const ScalarKey& k, FrameNumber at ) { return k.Tick < at; } );
            if ( after == keys.begin() || after == keys.end() )
            {
                return outside;
            }
            return ( after - 1 )->Interp;
        }

        /// Upsert: an existing key keeps its shape and takes the value; a new key gets @p interp / Auto.
        void Upsert( FloatChannel& component, FrameNumber tick, float value, KeyInterp interp )
        {
            if ( ScalarKey* existing = KeyOn( component, tick ) )
            {
                existing->Value = value;
                return;
            }
            ScalarKey key;
            key.Tick   = tick;
            key.Value  = value;
            key.Interp = interp;
            key.Mode   = TangentMode::Auto;
            InsertSorted( component, key );
        }

        void RefreshComponent( FloatChannel& component, FrameRate tickRate )
        {
            if ( component.Keys.size() < 2 )
            {
                // One key is a constant with no neighbours to take a slope from. THE MODE CHECK IS THE
                // CONTRACT `AutoSetTangents` states ("`User` and `Break` keys are left exactly as they are"):
                // zeroing an authored slope here was invisible until a second key arrived (T5.3).
                for ( ScalarKey& key : component.Keys )
                {
                    if ( key.Mode == TangentMode::Auto )
                    {
                        key.ArriveTangent = 0.0F;
                        key.LeaveTangent  = 0.0F;
                    }
                }
                return;
            }
            AutoSetTangents( component.Keys, tickRate );
        }

        void RefreshVector( VectorChannel& vector, FrameRate tickRate )
        {
            RefreshComponent( vector.X, tickRate );
            RefreshComponent( vector.Y, tickRate );
            RefreshComponent( vector.Z, tickRate );
        }

        [[nodiscard]] bool PartHasKeys( const TransformChannel& channel, TrackChannel part )
        {
            for ( const FloatChannel* component : ComponentsOf( channel, part ) )
            {
                if ( !component->Keys.empty() )
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool PartHasKeyOn( TransformChannel& channel, TrackChannel part, FrameNumber tick )
        {
            for ( FloatChannel* component : ComponentsOf( channel, part ) )
            {
                if ( KeyOn( *component, tick ) != nullptr )
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] TransformChannel* TransformOf( Timeline::Section& section )
        {
            auto* channel = std::get_if<Timeline::Channel>( &section.Content );
            return channel != nullptr ? std::get_if<TransformChannel>( channel ) : nullptr;
        }

        /// Section indices in REVERSE fold order: highest row first, and within a row the later one first —
        /// the first match is the section whose value the fold shows.
        [[nodiscard]] std::vector<size_t> TopmostFirst( const Timeline::Track& track )
        {
            std::vector<size_t> order( track.Sections.size() );
            for ( size_t i = 0; i < order.size(); ++i )
            {
                order[i] = i;
            }
            std::stable_sort( order.begin(), order.end(),
                              [&]( size_t a, size_t b )
                              {
                                  if ( track.Sections[a].Row != track.Sections[b].Row )
                                  {
                                      return track.Sections[a].Row > track.Sections[b].Row;
                                  }
                                  return a > b;
                              } );
            return order;
        }

        /// The topmost section whose @p part holds a key on @p tick — keys may lie outside a section's range
        /// (they shape the curve entering it), so a key is found where it IS, not where its section covers.
        [[nodiscard]] TransformChannel* ChannelHoldingKey( Timeline::Track& track, TrackChannel part,
                                                           FrameNumber tick )
        {
            for ( const size_t index : TopmostFirst( track ) )
            {
                TransformChannel* channel = TransformOf( track.Sections[index] );
                if ( channel != nullptr && PartHasKeyOn( *channel, part, tick ) )
                {
                    return channel;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const char* PartName( TrackChannel part )
        {
            switch ( part )
            {
                case TrackChannel::Position:
                    return "position";
                case TrackChannel::Rotation:
                    return "rotation";
                case TrackChannel::Scale:
                    return "scale";
            }
            return "?";
        }

        [[nodiscard]] bool InClip( const Timeline::Sequence& sequence, FrameNumber tick )
        {
            return !( tick < sequence.Start ) && !( sequence.End < tick );
        }
    } // namespace

    // ── Channel rules ────────────────────────────────────────────────────────────────────────────────

    bool HasKeys( const Timeline::TransformChannel& channel )
    {
        return PartHasKeys( channel, TrackChannel::Position ) || PartHasKeys( channel, TrackChannel::Rotation ) ||
               PartHasKeys( channel, TrackChannel::Scale );
    }

    void RefreshTangents( Timeline::TransformChannel& channel, FrameRate tickRate )
    {
        RefreshVector( channel.Translation, tickRate );
        RefreshVector( channel.Scale, tickRate );
    }

    std::vector<ScalarKey> LiftChannel( const Timeline::TransformChannel& channel, TrackChannel part,
                                        int component )
    {
        if ( component < 0 || component > 2 || part == TrackChannel::Rotation )
        {
            return {}; // not a gap — see the header: a quaternion's components are not curves
        }
        return ComponentsOf( channel, part )[static_cast<size_t>( component )]->Keys;
    }

    bool ApplyChannel( Timeline::TransformChannel& channel, TrackChannel part, int component,
                       const std::vector<ScalarKey>& scalars )
    {
        if ( component < 0 || component > 2 || part == TrackChannel::Rotation )
        {
            return false;
        }
        std::vector<ScalarKey>& keys = ComponentsOf( channel, part )[static_cast<size_t>( component )]->Keys;
        if ( keys.size() != scalars.size() )
        {
            return false;
        }
        for ( size_t i = 0; i < keys.size(); ++i )
        {
            if ( keys[i].Tick != scalars[i].Tick )
            {
                return false; // a retime is `MoveKey`
            }
        }
        for ( size_t i = 0; i < keys.size(); ++i )
        {
            keys[i].Value         = scalars[i].Value;
            keys[i].ArriveTangent = scalars[i].ArriveTangent;
            keys[i].LeaveTangent  = scalars[i].LeaveTangent;
            keys[i].Interp        = scalars[i].Interp;
            keys[i].Mode          = scalars[i].Mode;
        }
        return true;
    }

    bool InsertKeyFromCurve( Timeline::TransformChannel& channel, TrackChannel part, FrameNumber tick,
                             FrameRate tickRate )
    {
        if ( !PartHasKeys( channel, part ) || PartHasKeyOn( channel, part, tick ) )
        {
            return false;
        }
        const FrameTime at{ tick, 0.0F };

        if ( part == TrackChannel::Rotation )
        {
            // Sampled as a whole (slerp) and written to all four components on one tick — the rotation
            // invariant (identical tick lists, identical interp); the mode is the split segment's.
            const glm::quat q      = Timeline::Evaluate( channel.Rotation, at, tickRate );
            const KeyInterp interp = SplitSegmentInterp( channel.Rotation.X.Keys, tick, KeyInterp::Linear );
            const std::array<std::pair<Timeline::FloatChannel*, float>, 4> parts{ {
                 { &channel.Rotation.X, q.x },
                 { &channel.Rotation.Y, q.y },
                 { &channel.Rotation.Z, q.z },
                 { &channel.Rotation.W, q.w },
            } };
            for ( const auto& [component, value] : parts )
            {
                ScalarKey key;
                key.Tick   = tick;
                key.Value  = value;
                key.Interp = interp;
                InsertSorted( *component, key );
            }
            return true;
        }

        for ( Timeline::FloatChannel* component : ComponentsOf( channel, part ) )
        {
            // `User`, seeded with the slope the curve already has: the auto pass must not replace these
            // tangents with the ones the neighbours imply — that is what would move the pose (§936).
            ScalarKey key;
            key.Tick          = tick;
            key.Value         = Timeline::Evaluate( *component, at, tickRate );
            key.Interp        = SplitSegmentInterp( component->Keys, tick, KeyInterp::Cubic );
            key.Mode          = TangentMode::User;
            const float slope = SlopeAt( component->Keys, tick, tickRate );
            key.ArriveTangent = slope;
            key.LeaveTangent  = slope;
            InsertSorted( *component, key );
        }
        return true;
    }

    bool InsertFirstKeyFromPose( Timeline::TransformChannel& channel, TrackChannel part, FrameNumber tick,
                                 const glm::mat4& localPose )
    {
        if ( PartHasKeys( channel, part ) )
        {
            return false;
        }
        glm::vec3 scale;
        glm::quat rotation;
        glm::vec3 translation;
        glm::vec3 skew;
        glm::vec4 perspective;
        if ( !glm::decompose( localPose, scale, rotation, translation, skew, perspective ) )
        {
            return false;
        }

        const auto write =
             [&]( std::vector<Timeline::FloatChannel*> components, std::vector<float> values, KeyInterp interp )
        {
            for ( size_t i = 0; i < components.size(); ++i )
            {
                ScalarKey key;
                key.Tick   = tick;
                key.Value  = values[i];
                key.Interp = interp;
                key.Mode   = TangentMode::Auto;
                components[i]->Keys.push_back( key );
            }
        };
        switch ( part )
        {
            case TrackChannel::Position:
                write( ComponentsOf( channel, part ), { translation.x, translation.y, translation.z },
                       KeyInterp::Cubic );
                return true;
            case TrackChannel::Rotation:
                write( ComponentsOf( channel, part ), { rotation.x, rotation.y, rotation.z, rotation.w },
                       KeyInterp::Linear );
                return true;
            case TrackChannel::Scale:
                write( ComponentsOf( channel, part ), { scale.x, scale.y, scale.z }, KeyInterp::Cubic );
                return true;
        }
        return false;
    }

    bool SetTransformKey( Timeline::TransformChannel& channel, FrameNumber tick, const BoneTransform& pose,
                          FrameRate tickRate )
    {
        const auto finite3 = []( const glm::vec3& v )
        { return std::isfinite( v.x ) && std::isfinite( v.y ) && std::isfinite( v.z ); };
        if ( !finite3( pose.Translation ) || !finite3( pose.Scale ) || !std::isfinite( pose.Rotation.x ) ||
             !std::isfinite( pose.Rotation.y ) || !std::isfinite( pose.Rotation.z ) ||
             !std::isfinite( pose.Rotation.w ) )
        {
            // A NaN written into a clip is a NaN in every pose sampled from it, blamed on the sampler.
            return false;
        }

        Upsert( channel.Translation.X, tick, pose.Translation.x, KeyInterp::Cubic );
        Upsert( channel.Translation.Y, tick, pose.Translation.y, KeyInterp::Cubic );
        Upsert( channel.Translation.Z, tick, pose.Translation.z, KeyInterp::Cubic );
        Upsert( channel.Rotation.X, tick, pose.Rotation.x, KeyInterp::Linear );
        Upsert( channel.Rotation.Y, tick, pose.Rotation.y, KeyInterp::Linear );
        Upsert( channel.Rotation.Z, tick, pose.Rotation.z, KeyInterp::Linear );
        Upsert( channel.Rotation.W, tick, pose.Rotation.w, KeyInterp::Linear );
        Upsert( channel.Scale.X, tick, pose.Scale.x, KeyInterp::Cubic );
        Upsert( channel.Scale.Y, tick, pose.Scale.y, KeyInterp::Cubic );
        Upsert( channel.Scale.Z, tick, pose.Scale.z, KeyInterp::Cubic );

        // The new key is a new neighbour for the keys around it (§969 item 1).
        RefreshTangents( channel, tickRate );
        return true;
    }

    bool RemoveKey( Timeline::TransformChannel& channel, TrackChannel part, FrameNumber tick, FrameRate tickRate )
    {
        bool removed = false;
        for ( Timeline::FloatChannel* component : ComponentsOf( channel, part ) )
        {
            if ( ScalarKey* key = KeyOn( *component, tick ) )
            {
                component->Keys.erase( component->Keys.begin() + ( key - component->Keys.data() ) );
                removed = true;
            }
        }
        if ( removed )
        {
            RefreshTangents( channel, tickRate ); // both former neighbours lost one
        }
        return removed;
    }

    bool MoveKey( Timeline::TransformChannel& channel, TrackChannel part, FrameNumber from, FrameNumber to,
                  FrameRate tickRate )
    {
        if ( from == to || !PartHasKeyOn( channel, part, from ) || PartHasKeyOn( channel, part, to ) )
        {
            return false;
        }
        for ( Timeline::FloatChannel* component : ComponentsOf( channel, part ) )
        {
            if ( ScalarKey* key = KeyOn( *component, from ) )
            {
                ScalarKey moved = *key;
                moved.Tick      = to;
                component->Keys.erase( component->Keys.begin() + ( key - component->Keys.data() ) );
                InsertSorted( *component, moved );
            }
        }
        RefreshTangents( channel, tickRate );
        return true;
    }

    // ── Sequence edits ───────────────────────────────────────────────────────────────────────────────

    const Timeline::Track* FindBoneTrack( const Timeline::Sequence& sequence, std::string_view bone )
    {
        for ( const Timeline::Binding& binding : sequence.Bindings )
        {
            if ( binding.Kind == Timeline::BindingKind::Bone && binding.Locator == bone )
            {
                const Timeline::Track* track = Timeline::FindTrack( sequence, binding.Guid, "" );
                return ( track != nullptr && track->Kind == Timeline::TrackKind::Transform ) ? track : nullptr;
            }
        }
        return nullptr;
    }

    Timeline::Track* FindBoneTrack( Timeline::Sequence& sequence, std::string_view bone )
    {
        const Timeline::Track* found = FindBoneTrack( std::as_const( sequence ), bone );
        if ( found == nullptr )
        {
            return nullptr;
        }
        // The same element of the same vector, reached through the non-const owner.
        return &sequence.Tracks[static_cast<size_t>( found - sequence.Tracks.data() )];
    }

    Timeline::Track& AddBoneTrack( Timeline::Sequence& sequence, const std::string& bone )
    {
        if ( Timeline::Track* existing = FindBoneTrack( sequence, bone ) )
        {
            return *existing;
        }

        const Timeline::Binding* binding = nullptr;
        for ( const Timeline::Binding& candidate : sequence.Bindings )
        {
            if ( candidate.Kind == Timeline::BindingKind::Bone && candidate.Locator == bone )
            {
                binding = &candidate;
                break;
            }
        }
        Timeline::BindingGuid guid;
        if ( binding != nullptr )
        {
            guid = binding->Guid; // a binding with no Transform track yet (e.g. only its notifies were cut)
        }
        else
        {
            Timeline::Binding created;
            created.Guid    = Timeline::BindingGuid::ForObject( Timeline::BindingKind::Bone, bone );
            created.Kind    = Timeline::BindingKind::Bone;
            created.Locator = bone;
            created.Label   = bone;
            guid            = created.Guid;
            sequence.Bindings.push_back( std::move( created ) );
        }

        Timeline::Track track;
        track.Binding = guid;
        track.Kind    = Timeline::TrackKind::Transform;
        sequence.Tracks.push_back( std::move( track ) );
        ++sequence.Revision;
        return sequence.Tracks.back();
    }

    bool HasKeys( const Timeline::Track& track )
    {
        for ( const Timeline::Section& section : track.Sections )
        {
            const auto* channel = std::get_if<Timeline::Channel>( &section.Content );
            const auto* transform =
                 channel != nullptr ? std::get_if<Timeline::TransformChannel>( channel ) : nullptr;
            if ( transform != nullptr && HasKeys( *transform ) )
            {
                return true;
            }
        }
        return false;
    }

    Timeline::TransformChannel* KeyedChannelAt( Timeline::Track& track, FrameNumber tick )
    {
        for ( const size_t index : TopmostFirst( track ) )
        {
            Timeline::Section& section = track.Sections[index];
            if ( section.Covers( tick ) )
            {
                if ( Timeline::TransformChannel* channel = TransformOf( section ) )
                {
                    return channel;
                }
            }
        }
        return nullptr;
    }

    Timeline::TransformChannel& ChannelForKey( Timeline::Sequence& sequence, Timeline::Track& track,
                                               FrameNumber tick )
    {
        if ( Timeline::TransformChannel* channel = KeyedChannelAt( track, tick ) )
        {
            return *channel;
        }
        // BELOW every existing row: the new section fills the gap the tick fell in and hides nothing an
        // animator authored (a later section on the same row would win everywhere it overlaps).
        int32_t row = 0;
        for ( const Timeline::Section& section : track.Sections )
        {
            row = std::min( row, section.Row - 1 );
        }
        Timeline::Section section;
        section.Start   = sequence.Start;
        section.End     = sequence.End;
        section.Blend   = SectionBlendType::Absolute;
        section.Row     = track.Sections.empty() ? 0 : row;
        section.Content = Timeline::MakeChannel( Timeline::ChannelKind::Transform );
        track.Sections.push_back( std::move( section ) );
        ++sequence.Revision;
        return *TransformOf( track.Sections.back() );
    }

    Common::BoolResultStr SetBoneKey( Timeline::Sequence& sequence, const std::string& bone, FrameNumber tick,
                                      const BoneTransform& pose )
    {
        if ( !InClip( sequence, tick ) )
        {
            return Common::MakeFormattedError<bool>(
                 "'{}': tick {} is outside the clip [{}, {}] — a key there is never sampled, and lengthening the "
                 "clip is a separate edit",
                 bone, tick.Value, sequence.Start.Value, sequence.End.Value );
        }
        Timeline::Track&            track   = AddBoneTrack( sequence, bone );
        Timeline::TransformChannel& channel = ChannelForKey( sequence, track, tick );
        if ( !SetTransformKey( channel, tick, pose, sequence.TickRate ) )
        {
            return Common::MakeFormattedError<bool>( "'{}': a non-finite pose cannot be keyed at tick {}", bone,
                                                     tick.Value );
        }
        ++sequence.Revision;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr InsertBoneKey( Timeline::Sequence& sequence, std::string_view bone, TrackChannel part,
                                         FrameNumber tick )
    {
        Timeline::Track*            track   = FindBoneTrack( sequence, bone );
        Timeline::TransformChannel* channel = track != nullptr ? KeyedChannelAt( *track, tick ) : nullptr;
        if ( channel == nullptr || !InsertKeyFromCurve( *channel, part, tick, sequence.TickRate ) )
        {
            return Common::MakeFormattedError<bool>(
                 "'{}' {}: no key inserted at tick {} — no section covers it, the part has no curve to read, or "
                 "a key is already there",
                 bone, PartName( part ), tick.Value );
        }
        ++sequence.Revision;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr RemoveBoneKey( Timeline::Sequence& sequence, std::string_view bone, TrackChannel part,
                                         FrameNumber tick )
    {
        Timeline::Track*            track   = FindBoneTrack( sequence, bone );
        Timeline::TransformChannel* channel = track != nullptr ? ChannelHoldingKey( *track, part, tick ) : nullptr;
        if ( channel == nullptr || !RemoveKey( *channel, part, tick, sequence.TickRate ) )
        {
            return Common::MakeFormattedError<bool>( "'{}' {}: no key at tick {}", bone, PartName( part ),
                                                     tick.Value );
        }
        ++sequence.Revision;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr MoveBoneKey( Timeline::Sequence& sequence, std::string_view bone, TrackChannel part,
                                       FrameNumber from, FrameNumber to )
    {
        if ( !InClip( sequence, to ) )
        {
            return Common::MakeFormattedError<bool>( "'{}' {}: tick {} is outside the clip [{}, {}]", bone,
                                                     PartName( part ), to.Value, sequence.Start.Value,
                                                     sequence.End.Value );
        }
        Timeline::Track*            track   = FindBoneTrack( sequence, bone );
        Timeline::TransformChannel* channel = track != nullptr ? ChannelHoldingKey( *track, part, from ) : nullptr;
        if ( channel == nullptr || !MoveKey( *channel, part, from, to, sequence.TickRate ) )
        {
            return Common::MakeFormattedError<bool>(
                 "'{}' {}: the key at tick {} cannot move to {} — none is there, or the target is occupied", bone,
                 PartName( part ), from.Value, to.Value );
        }
        ++sequence.Revision;
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Animation
