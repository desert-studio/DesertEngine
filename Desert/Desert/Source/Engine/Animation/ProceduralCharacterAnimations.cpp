#include "ProceduralCharacterAnimations.hpp"

#include <algorithm>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/BoneInfo.hpp>

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
            clip.DurationTicks     = durationTicks;
            clip.TickRate          = PROJECT_TICK_RATE;
            // clip.Skeleton is stated by the generator (ProceduralCharacterFactory::WriteEngineAssets).
            // ONE TRACK PER ANIMATED BONE, NAMED. Playback binds a track by its bone name only; a bone with no
            // track holds its bind pose. (An index-aligned vector with unnamed filler tracks is refused by the
            // clip reader: a channel that names no bone can never reach a skeleton.)
            clip.Tracks.reserve( angleFns.size() );

            for ( const auto& [boneName, fn] : angleFns )
            {
                auto it = nameToIdx.find( boneName );
                if ( it == nameToIdx.end() )
                    continue;
                const uint32_t idx      = it->second;
                const glm::vec3 bindPos = glm::vec3( bones[idx].LocalBindTransform[3] );

                BoneTrack& t = clip.Tracks.emplace_back();
                t.BoneName   = boneName;
                // constant -> keeps the bone at its bind offset
                t.PositionKeys.push_back( { FrameNumber{ 0 }, bindPos } );
                for ( int i = 0; i <= kSamples; ++i )
                {
                    const float       u    = static_cast<float>( i ) / kSamples;
                    const FrameNumber tick = NearestTick(
                         SecondsToFrameTime( static_cast<double>( duration ) * u, PROJECT_TICK_RATE ) );
                    t.RotationKeys.push_back( { tick, glm::angleAxis( fn( u ), kAxisX ) } );
                }
            }
            // In skeleton order, so a regeneration writes the same bytes (angleFns is unordered).
            std::sort( clip.Tracks.begin(), clip.Tracks.end(), [&]( const BoneTrack& a, const BoneTrack& b )
                       { return nameToIdx.at( a.BoneName ) < nameToIdx.at( b.BoneName ); } );
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
