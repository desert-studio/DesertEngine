#pragma once

// Clips the Animator suites play (AnimatorBlending, AnimatorPose): ONE spelling of "a clip whose data is its
// Timeline::Sequence", built the way the engine builds one (ProceduralCharacterAnimations): a Bone binding with
// a Transform track per animated bone, the clip's notifies as keys of an Event track on the Sequence (master)
// binding, its curves as Float tracks named by Property on that same binding. Every track holds ONE section
// spanning the clip's playback range, Absolute at full weight — what every migrated `.anim` carries — and the
// suites that need a section's Weight/Blend get that section back to edit. Included by relative path;
// header-only.

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/KeyInterpolation.hpp>
#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Channel.hpp>
#include <Engine/Animation/Timeline/Section.hpp>
#include <Engine/Animation/Timeline/Track.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace ClipFixture
{
    namespace Animation = Desert::Animation;
    namespace Timeline  = Desert::Animation::Timeline;

    /// An empty clip on the project tick grid, playing [0, @p duration].
    inline Animation::AnimationClip Clip( std::string name, Animation::FrameNumber duration )
    {
        Animation::AnimationClip clip;
        clip.AnimationName     = std::move( name );
        clip.Sequence.TickRate = Animation::PROJECT_TICK_RATE;
        clip.Sequence.Start    = Animation::FrameNumber{ 0 };
        clip.Sequence.End      = duration;
        return clip;
    }

    /// A section over the clip's whole playback range holding @p content, Absolute at full weight.
    inline Timeline::Section WholeClipSection( const Animation::AnimationClip& clip, Timeline::Channel content )
    {
        Timeline::Section section;
        section.Start   = clip.Sequence.Start;
        section.End     = clip.Sequence.End;
        section.Blend   = Timeline::SectionBlendType::Absolute;
        section.Content = std::move( content );
        return section;
    }

    /// The clip's Sequence (master) binding — the one its notifies and curves live on; added on first use.
    inline Timeline::BindingGuid MasterBinding( Animation::AnimationClip& clip )
    {
        for ( const Timeline::Binding& binding : clip.Sequence.Bindings )
        {
            if ( binding.Kind == Timeline::BindingKind::Sequence )
            {
                return binding.Guid;
            }
        }
        Timeline::Binding binding;
        binding.Guid  = Timeline::BindingGuid::Generate();
        binding.Kind  = Timeline::BindingKind::Sequence;
        binding.Label = clip.AnimationName;
        clip.Sequence.Bindings.push_back( binding );
        return binding.Guid;
    }

    inline Animation::ScalarKey Key( Animation::FrameNumber tick, float value )
    {
        Animation::ScalarKey key;
        key.Tick  = tick;
        key.Value = value;
        return key;
    }

    /**
     * @brief Bind @p bone and hold it at @p position / @p rotation / @p scale for the whole clip: one key per
     * component at the clip's first tick. Returns the track's one section, for a suite that sets its Weight or
     * Blend. The reference is into `clip.Sequence.Tracks`; it dies with the next track added.
     */
    inline Timeline::Section& AddStaticBone( Animation::AnimationClip& clip, const std::string& bone,
                                             const glm::vec3& position,
                                             const glm::quat& rotation = glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ),
                                             const glm::vec3& scale    = glm::vec3( 1.0F ) )
    {
        Timeline::Binding binding;
        binding.Guid    = Timeline::BindingGuid::Generate();
        binding.Kind    = Timeline::BindingKind::Bone;
        binding.Locator = bone;
        binding.Label   = bone;
        clip.Sequence.Bindings.push_back( binding );

        const Animation::FrameNumber at = clip.Sequence.Start;
        Timeline::TransformChannel   channel;
        channel.Translation.X.Keys.push_back( Key( at, position.x ) );
        channel.Translation.Y.Keys.push_back( Key( at, position.y ) );
        channel.Translation.Z.Keys.push_back( Key( at, position.z ) );
        channel.Rotation.X.Keys.push_back( Key( at, rotation.x ) );
        channel.Rotation.Y.Keys.push_back( Key( at, rotation.y ) );
        channel.Rotation.Z.Keys.push_back( Key( at, rotation.z ) );
        channel.Rotation.W.Keys.push_back( Key( at, rotation.w ) );
        channel.Scale.X.Keys.push_back( Key( at, scale.x ) );
        channel.Scale.Y.Keys.push_back( Key( at, scale.y ) );
        channel.Scale.Z.Keys.push_back( Key( at, scale.z ) );

        Timeline::Track track;
        track.Binding = binding.Guid;
        track.Kind    = Timeline::TrackKind::Transform;
        track.Sections.push_back( WholeClipSection( clip, Timeline::Channel{ std::move( channel ) } ) );
        clip.Sequence.Tracks.push_back( std::move( track ) );
        return clip.Sequence.Tracks.back().Sections.back();
    }

    /// A clip of @p duration holding @p bone still — the one-key clip most Animator tests play.
    inline Animation::AnimationClip
    StaticBoneClip( std::string name, Animation::FrameNumber duration, const std::string& bone,
                    const glm::vec3& position, const glm::quat& rotation = glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) )
    {
        Animation::AnimationClip clip = Clip( std::move( name ), duration );
        (void)AddStaticBone( clip, bone, position, rotation );
        return clip;
    }

    /**
     * @brief A notify at @p tick (UE's AnimNotify), or a notify STATE over [tick, tick + duration) when
     * @p duration > 0 (UE's AnimNotifyState): a key of the clip's one Event track on the master binding.
     * Keys stay sorted by tick; several on one tick keep the order they were added in (they fire in it).
     */
    inline void AddNotify( Animation::AnimationClip& clip, std::string name, Animation::FrameNumber tick,
                           Animation::FrameNumber duration = Animation::FrameNumber{ 0 } )
    {
        const Timeline::BindingGuid master = MasterBinding( clip );
        auto                        it = std::find_if( clip.Sequence.Tracks.begin(), clip.Sequence.Tracks.end(),
                                                       [&master]( const Timeline::Track& track )
                                                       { return track.Kind == Timeline::TrackKind::Event && track.Binding == master; } );
        if ( it == clip.Sequence.Tracks.end() )
        {
            Timeline::Track track;
            track.Binding = master;
            track.Kind    = Timeline::TrackKind::Event;
            track.Sections.push_back( WholeClipSection( clip, Timeline::Channel{ Timeline::EventChannel{} } ) );
            clip.Sequence.Tracks.push_back( std::move( track ) );
            it = std::prev( clip.Sequence.Tracks.end() );
        }
        auto& keys =
             std::get<Timeline::EventChannel>( std::get<Timeline::Channel>( it->Sections.front().Content ) ).Keys;
        const auto at = std::upper_bound( keys.begin(), keys.end(), tick,
                                          []( Animation::FrameNumber t, const Timeline::EventKey& key )
                                          { return t < key.Tick; } );
        keys.insert( at, Timeline::EventKey{ tick, duration, std::move( name ), 0 } );
    }

    /// An anim curve (UE's FFloatCurve): a Float track named @p name on the master binding.
    inline void AddCurve( Animation::AnimationClip& clip, std::string name,
                          std::vector<Animation::ScalarKey> keys )
    {
        Timeline::Track track;
        track.Binding  = MasterBinding( clip );
        track.Property = std::move( name );
        track.Kind     = Timeline::TrackKind::Float;
        Timeline::FloatChannel channel;
        channel.Keys = std::move( keys );
        track.Sections.push_back( WholeClipSection( clip, Timeline::Channel{ std::move( channel ) } ) );
        clip.Sequence.Tracks.push_back( std::move( track ) );
    }
} // namespace ClipFixture
