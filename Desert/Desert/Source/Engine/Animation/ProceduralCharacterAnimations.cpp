#include "ProceduralCharacterAnimations.hpp"

#include <algorithm>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Animation/Timeline/Section.hpp>
#include <Engine/Animation/Timeline/Track.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <format>
#include <functional>
#include <unordered_map>

namespace Desert::Animation
{
    namespace
    {
        constexpr int   kSamples = 16;          // keyframes per cycle (endpoint included for a seamless loop)
        const glm::vec3 kAxisX( 1.0f, 0.0f, 0.0f ); // sagittal swing axis (character faces +Z)

        // Builds a clip from per-bone angle functions. `angleFns[name] = f(u)` where u in [0,1] is the cycle
        // fraction and f returns the rotation angle (radians) about X. Bones with no entry keep their bind
        // pose. Every authored track also carries the bone's constant bind-local translation.
        AnimationClip BuildClip( const Skeleton& skeleton, const char* name, float duration,
                                 const std::unordered_map<std::string, std::function<float( float )>>& angleFns )
        {
            const auto& bones = skeleton.GetBones();

            std::unordered_map<std::string, uint32_t> nameToIdx;
            for ( uint32_t i = 0; i < bones.size(); ++i )
                nameToIdx[bones[i].Name] = i; // the position IS the index; BoneInfo no longer repeats it

            // GENERATED ONTO THE PROJECT TICK GRID, like every clip that comes off disk. These four clips
            // used to set `TicksPerSecond` to 1 and write key times in seconds — the same fiction the six
            // shipped `.anim` files carried, and the reason the field's name was never true.
            const FrameNumber durationTicks =
                 FrameNumber{ NearestTick( SecondsToFrameTime( duration, PROJECT_TICK_RATE ) ) };

            AnimationClip clip;
            clip.AnimationName     = name;
            clip.Sequence.TickRate = PROJECT_TICK_RATE;
            clip.Sequence.Start    = FrameNumber{ 0 };
            clip.Sequence.End      = durationTicks;
            // clip.Skeleton is stated by the generator (ProceduralCharacterFactory::WriteEngineAssets).
            // ONE NAMED BONE BINDING PER ANIMATED BONE, IN SKELETON ORDER (angleFns is unordered): playback binds
            // a track by its bone name only; a bone with no binding holds its bind pose.
            std::vector<std::pair<uint32_t, std::string>> animated;
            for ( const auto& [boneName, fn] : angleFns )
                if ( const auto it = nameToIdx.find( boneName ); it != nameToIdx.end() )
                    animated.emplace_back( it->second, boneName );
            std::sort( animated.begin(), animated.end() );
            for ( const auto& [idx, boneName] : animated )
            {
                const auto&     fn      = angleFns.at( boneName );
                const glm::vec3 bindPos = glm::vec3( bones[idx].LocalBindTransform[3] );

                Timeline::Binding binding;
                binding.Guid    = Timeline::BindingGuid::ForObject( Timeline::BindingKind::Bone, boneName );
                binding.Kind    = Timeline::BindingKind::Bone;
                binding.Locator = boneName;
                binding.Label   = boneName;
                clip.Sequence.Bindings.push_back( binding );

                Timeline::TransformChannel channel;
                // constant -> keeps the bone at its bind offset
                channel.Translation.X.Keys.push_back( ScalarKey{ FrameNumber{ 0 }, bindPos.x } );
                channel.Translation.Y.Keys.push_back( ScalarKey{ FrameNumber{ 0 }, bindPos.y } );
                channel.Translation.Z.Keys.push_back( ScalarKey{ FrameNumber{ 0 }, bindPos.z } );
                for ( int i = 0; i <= kSamples; ++i )
                {
                    const float       u    = static_cast<float>( i ) / kSamples;
                    const FrameNumber tick = NearestTick(
                         SecondsToFrameTime( static_cast<double>( duration ) * u, PROJECT_TICK_RATE ) );
                    const glm::quat q = glm::angleAxis( fn( u ), kAxisX );
                    channel.Rotation.X.Keys.push_back( ScalarKey{ tick, q.x } );
                    channel.Rotation.Y.Keys.push_back( ScalarKey{ tick, q.y } );
                    channel.Rotation.Z.Keys.push_back( ScalarKey{ tick, q.z } );
                    channel.Rotation.W.Keys.push_back( ScalarKey{ tick, q.w } );
                }
                Timeline::Section section;
                section.Start   = clip.Sequence.Start;
                section.End     = clip.Sequence.End;
                section.Content = Timeline::Channel{ std::move( channel ) };
                Timeline::Track track;
                track.Binding = binding.Guid;
                track.Kind    = Timeline::TrackKind::Transform;
                track.Sections.push_back( std::move( section ) );
                clip.Sequence.Tracks.push_back( std::move( track ) );
            }
            return clip;
        }

        constexpr float kTau = 6.2831853f;

        AnimationClip Idle( const Skeleton& skeleton )
        {
            // Subtle breathing/sway so a standing character isn't a frozen statue.
            return BuildClip( skeleton, "Idle", 3.0f,
                              { { "Spine", []( float u ) { return 0.04f * std::sin( kTau * u ); } },
                                { "Chest", []( float u ) { return 0.03f * std::sin( kTau * u ); } },
                                { "Shoulder.L", []( float u ) { return 0.05f * std::sin( kTau * u ); } },
                                { "Shoulder.R", []( float u ) { return 0.05f * std::sin( kTau * u ); } },
                                { "Elbow.L", []( float ) { return -0.15f; } },
                                { "Elbow.R", []( float ) { return -0.15f; } } } );
        }

        AnimationClip Walk( const Skeleton& skeleton )
        {
            constexpr float A_hip = 0.5f, A_knee = 0.7f, A_arm = 0.4f;
            return BuildClip(
                 skeleton, "Walk", 1.0f,
                 { // legs swing opposite; arms counter-swing the legs
                   { "Hip.L", []( float u ) { return A_hip * std::sin( kTau * u ); } },
                   { "Hip.R", []( float u ) { return A_hip * std::sin( kTau * u + kTau * 0.5f ); } },
                   { "Knee.L",
                     []( float u ) { return A_knee * ( 0.5f - 0.5f * std::cos( kTau * u - kTau * 0.25f ) ); } },
                   { "Knee.R",
                     []( float u ) { return A_knee * ( 0.5f - 0.5f * std::cos( kTau * u + kTau * 0.25f ) ); } },
                   { "Shoulder.L", []( float u ) { return A_arm * std::sin( kTau * u + kTau * 0.5f ); } },
                   { "Shoulder.R", []( float u ) { return A_arm * std::sin( kTau * u ); } },
                   { "Elbow.L", []( float ) { return -0.25f; } },
                   { "Elbow.R", []( float ) { return -0.25f; } } } );
        }

        AnimationClip Run( const Skeleton& skeleton )
        {
            constexpr float A_hip = 0.85f, A_knee = 1.1f, A_arm = 0.7f;
            return BuildClip(
                 skeleton, "Run", 0.6f,
                 { { "Spine", []( float ) { return 0.20f; } }, // forward lean
                   { "Hip.L", []( float u ) { return A_hip * std::sin( kTau * u ); } },
                   { "Hip.R", []( float u ) { return A_hip * std::sin( kTau * u + kTau * 0.5f ); } },
                   { "Knee.L",
                     []( float u ) { return A_knee * ( 0.5f - 0.5f * std::cos( kTau * u - kTau * 0.25f ) ); } },
                   { "Knee.R",
                     []( float u ) { return A_knee * ( 0.5f - 0.5f * std::cos( kTau * u + kTau * 0.25f ) ); } },
                   { "Shoulder.L", []( float u ) { return A_arm * std::sin( kTau * u + kTau * 0.5f ); } },
                   { "Shoulder.R", []( float u ) { return A_arm * std::sin( kTau * u ); } },
                   { "Elbow.L", []( float ) { return -0.6f; } },
                   { "Elbow.R", []( float ) { return -0.6f; } } } );
        }

        AnimationClip Jump( const Skeleton& skeleton )
        {
            // A held airborne pose: knees tucked, slight forward lean, arms raised forward. Constant (it just
            // holds while the character is off the ground); loops trivially.
            return BuildClip( skeleton, "Jump", 0.5f,
                              { { "Spine", []( float ) { return 0.12f; } },
                                { "Hip.L", []( float ) { return -0.25f; } },
                                { "Hip.R", []( float ) { return -0.25f; } },
                                { "Knee.L", []( float ) { return 0.9f; } },
                                { "Knee.R", []( float ) { return 0.9f; } },
                                { "Shoulder.L", []( float ) { return -0.45f; } },
                                { "Shoulder.R", []( float ) { return -0.45f; } },
                                { "Elbow.L", []( float ) { return -0.6f; } },
                                { "Elbow.R", []( float ) { return -0.6f; } } } );
        }

    } // namespace

    Common::ResultStr<AnimationClip> ProceduralCharacterAnimations::Build( const Skeleton&  skeleton,
                                                                           std::string_view name )
    {
        if ( name == "Idle" )
            return Common::MakeSuccess( Idle( skeleton ) );
        if ( name == "Walk" )
            return Common::MakeSuccess( Walk( skeleton ) );
        if ( name == "Run" )
            return Common::MakeSuccess( Run( skeleton ) );
        if ( name == "Jump" )
            return Common::MakeSuccess( Jump( skeleton ) );
        return Common::MakeFormattedError<AnimationClip>( "the humanoid has no locomotion clip named '{}'", name );
    }
} // namespace Desert::Animation
