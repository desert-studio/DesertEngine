#pragma once

#include <Engine/Animation/Pose.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/compatibility.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace Desert::Animation
{
    // Initialised for the same reason as the serialization mirrors in Assets/Serialization/Animation.hpp:
    // glm leaves its components indeterminate, and these are the values a clip is sampled from.
    struct PositionKeyFrame
    {
        float     Time     = 0.0f;
        glm::vec3 Position = glm::vec3( 0.0f );

        bool operator<( const PositionKeyFrame& other ) const
        {
            return Time < other.Time;
        }
        bool operator<( float time ) const
        {
            return Time < time;
        }
    };

    struct RotationKeyFrame
    {
        float     Time     = 0.0f;
        glm::quat Rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );

        bool operator<( const RotationKeyFrame& other ) const
        {
            return Time < other.Time;
        }
        bool operator<( float time ) const
        {
            return Time < time;
        }
    };

    struct ScaleKeyFrame
    {
        float     Time  = 0.0f;
        glm::vec3 Scale = glm::vec3( 1.0f );

        bool operator<( const ScaleKeyFrame& other ) const
        {
            return Time < other.Time;
        }
        bool operator<( float time ) const
        {
            return Time < time;
        }
    };

    // THE BONE NAME IS THE ONLY BINDING KEY. A `uint32_t BoneIndex` used to sit beside it, uninitialised, and
    // Animator::ResolveTrack has never once read it — it builds name -> track and binds by name, because a
    // clip and the character it drives come from different files with different bone orders. The index was a
    // second answer to a question only the name answers, and the Sequencer's "New Clip" left it unset all the
    // way into the .anim file.
    struct BoneTrack
    {
        std::string BoneName;

        std::vector<PositionKeyFrame> PositionKeys;
        std::vector<RotationKeyFrame> RotationKeys;
        std::vector<ScaleKeyFrame>    ScaleKeys;

        /**
         * @brief The track's value at `animationTime`, in the three quantities it is stored in.
         *
         * THE MATRIX IS GONE FROM THE MIDDLE. This used to be `GetTransform`, composing the interpolated
         * P/R/S into a mat4 — which `Animator::SampleLocalTransform` handed on and layer composition
         * immediately decomposed again, twice per bone per layer. A round trip with no consumer of the
         * matrix in it, on the hottest path the animation system has.
         */
        [[nodiscard]] BoneTransform Sample( float animationTime ) const
        {
            BoneTransform out;
            out.Translation = GetInterpolatedPosition( animationTime );
            out.Rotation    = GetInterpolatedRotation( animationTime );
            out.Scale       = GetInterpolatedScale( animationTime );
            return out;
        }

        /// True when the track carries anything at all. A track with three empty channels is a name with no
        /// animation behind it, and the pose it would produce is the bind pose — which the caller already
        /// has, and which is why every sampler checked this before reading.
        [[nodiscard]] bool HasKeys() const
        {
            return !PositionKeys.empty() || !RotationKeys.empty() || !ScaleKeys.empty();
        }

        [[nodiscard]] glm::vec3 GetInterpolatedPosition( float animationTime ) const
        {
            if ( PositionKeys.empty() )
                return glm::vec3( 0.0f );

            if ( PositionKeys.size() == 1 )
                return PositionKeys[0].Position;

            auto it = std::lower_bound( PositionKeys.begin(), PositionKeys.end(), animationTime );

            if ( it == PositionKeys.begin() )
                return PositionKeys.front().Position;
            if ( it == PositionKeys.end() )
                return PositionKeys.back().Position;

            auto prev = it - 1;
            auto next = it;

            float deltaTime = next->Time - prev->Time;
            float factor    = ( animationTime - prev->Time ) / deltaTime;

            return glm::lerp( prev->Position, next->Position, factor );
        }

        [[nodiscard]] glm::quat GetInterpolatedRotation( float animationTime ) const
        {
            if ( RotationKeys.empty() )
                return glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );

            if ( RotationKeys.size() == 1 )
                return RotationKeys[0].Rotation;

            auto it = std::lower_bound( RotationKeys.begin(), RotationKeys.end(), animationTime );

            if ( it == RotationKeys.begin() )
                return RotationKeys.front().Rotation;
            if ( it == RotationKeys.end() )
                return RotationKeys.back().Rotation;

            auto prev = it - 1;
            auto next = it;

            float deltaTime = next->Time - prev->Time;
            float factor    = ( animationTime - prev->Time ) / deltaTime;

            return glm::slerp( prev->Rotation, next->Rotation, factor );
        }

        [[nodiscard]] glm::vec3 GetInterpolatedScale( float animationTime ) const
        {
            if ( ScaleKeys.empty() )
                return glm::vec3( 1.0f );

            if ( ScaleKeys.size() == 1 )
                return ScaleKeys[0].Scale;

            auto it = std::lower_bound( ScaleKeys.begin(), ScaleKeys.end(), animationTime );

            if ( it == ScaleKeys.begin() )
                return ScaleKeys.front().Scale;
            if ( it == ScaleKeys.end() )
                return ScaleKeys.back().Scale;

            auto prev = it - 1;
            auto next = it;

            float deltaTime = next->Time - prev->Time;
            float factor    = ( animationTime - prev->Time ) / deltaTime;

            return glm::lerp( prev->Scale, next->Scale, factor );
        }
    };

    // Animation notify / event: a named marker at a time (same unit as Duration / key times). Fires once when
    // playback crosses it; the Animator queues crossed notifies and the ECS dispatches them to scripts.
    struct AnimationNotify
    {
        std::string Name;
        float       Time = 0.0f;
    };

    class AnimationClip
    {
    public:
        std::string AnimationName;
        float       Duration       = 0.0f;
        float       TicksPerSecond = 25.0f;
        // 0 = "no rig claimed", and it needed an initialiser: a default-constructed clip read back
        // whatever was on the heap, and this number is what the animation system matches a skeleton on —
        // so an unset one does not fail to match, it matches something arbitrary. Its neighbours all had
        // one; this field was the exception.
        uint64_t SkeletonSignature = 0;

        // Named tracks, in the order the source file listed them. THIS IS NOT INDEXED BY BONE: it used to be
        // scattered by a serialised bone index, which left unnamed holes wherever the source rig was sparse
        // and made the vector's length a property of the exporter. Playback resolves by name
        // (Animator::ResolveTrack), so position here means nothing and is not allowed to pretend otherwise.
        std::vector<BoneTrack> Tracks;

        /**
         * @brief Bumped whenever `Tracks` is REPLACED. The only honest key for a per-clip track cache.
         *
         * The address of the vector's storage is not one, and believing it was left a real hole in
         * `Animator::TrackBinding`: an unload frees a one-element track list and the reload allocates
         * another of the same size, so malloc hands back the identical block and BOTH `Tracks.data()` and
         * `Tracks.size()` come out unchanged across a complete replacement. The cache then kept a binding
         * built against the OLD list — bone names mapped to the wrong tracks, and a bone the new list
         * animates mapped to nothing at all. It is not a crash (the binding now stores indices, so it
         * cannot dangle) and that is exactly why it would have gone unnoticed: a character that plays the
         * wrong track after an eviction looks like bad animation data.
         *
         * NOT SERIALIZED: it describes this process's copy of the list, not the file.
         */
        uint32_t TrackRevision = 0;

        std::vector<AnimationNotify> Notifies; // sorted-by-time markers fired during playback
    };
} // namespace Desert::Animation
