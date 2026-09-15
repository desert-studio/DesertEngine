#pragma once

#include <Engine/Animation/KeyInterpolation.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/TimeModel.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/compatibility.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace Desert::Animation
{
    // A KEY SITS ON A TICK, NOT AT A FLOAT NUMBER OF SECONDS. `float Time` used to live here, and the
    // ordering predicates below were the whole reason it hurt: `lower_bound` over floats decides which two
    // keys bracket the playhead, so "the playhead is exactly on this key" depended on two float paths
    // producing bit-identical values. On the tick grid the comparison is an integer one.
    struct PositionKeyFrame
    {
        FrameNumber Tick;
        glm::vec3   Position = glm::vec3( 0.0f );

        /// The shape of the segment ENDING at this key, and the slopes that shape it. The convention that
        /// the later key owns the rule is taken from `UIAnimKey::Easing` rather than invented beside it.
        KeyInterp   Interp        = KeyInterp::Linear;
        TangentMode Mode          = TangentMode::Auto;
        glm::vec3   ArriveTangent = glm::vec3( 0.0f ); // value units per SECOND, one per component
        glm::vec3   LeaveTangent  = glm::vec3( 0.0f );
        glm::vec3   ArriveWeight  = glm::vec3( 0.0f ); // RESERVED: unweighted ships first (§969)
        glm::vec3   LeaveWeight   = glm::vec3( 0.0f );

        bool operator<( const PositionKeyFrame& other ) const
        {
            return Tick < other.Tick;
        }
        bool operator<( FrameNumber tick ) const
        {
            return Tick < tick;
        }
    };

    struct RotationKeyFrame
    {
        FrameNumber Tick;
        glm::quat   Rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );

        /// NO TANGENTS, and the reason is the maths: a cubic through quaternions leaves the unit sphere,
        /// and the curve that does not is `squad`, which is a different construction. `Constant` and
        /// `Linear` are both meaningful for a rotation, and holding a pose is what `Constant` is for.
        KeyInterp Interp = KeyInterp::Linear;

        bool operator<( const RotationKeyFrame& other ) const
        {
            return Tick < other.Tick;
        }
        bool operator<( FrameNumber tick ) const
        {
            return Tick < tick;
        }
    };

    struct ScaleKeyFrame
    {
        FrameNumber Tick;
        glm::vec3   Scale = glm::vec3( 1.0f );

        KeyInterp   Interp        = KeyInterp::Linear;
        TangentMode Mode          = TangentMode::Auto;
        glm::vec3   ArriveTangent = glm::vec3( 0.0f );
        glm::vec3   LeaveTangent  = glm::vec3( 0.0f );
        glm::vec3   ArriveWeight  = glm::vec3( 0.0f );
        glm::vec3   LeaveWeight   = glm::vec3( 0.0f );

        bool operator<( const ScaleKeyFrame& other ) const
        {
            return Tick < other.Tick;
        }
        bool operator<( FrameNumber tick ) const
        {
            return Tick < tick;
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
        /// THE TICK RATE IS AN ARGUMENT, and it has to be: a tangent is value-per-second, so turning one
        /// into a position inside a segment needs to know how long that segment is in seconds. The track
        /// does not own the rate — the clip does — so it is passed rather than duplicated here.
        [[nodiscard]] BoneTransform Sample( FrameTime at, FrameRate tickRate ) const
        {
            BoneTransform out;
            out.Translation = GetInterpolatedPosition( at, tickRate );
            out.Rotation    = GetInterpolatedRotation( at );
            out.Scale       = GetInterpolatedScale( at, tickRate );
            return out;
        }

        /// True when the track carries anything at all. A track with three empty channels is a name with no
        /// animation behind it, and the pose it would produce is the bind pose — which the caller already
        /// has, and which is why every sampler checked this before reading.
        [[nodiscard]] bool HasKeys() const
        {
            return !PositionKeys.empty() || !RotationKeys.empty() || !ScaleKeys.empty();
        }

        [[nodiscard]] glm::vec3 GetInterpolatedPosition( FrameTime at, FrameRate tickRate ) const
        {
            if ( PositionKeys.empty() )
            {
                return glm::vec3( 0.0f );
            }
            if ( PositionKeys.size() == 1 )
            {
                return PositionKeys[0].Position;
            }

            // The bracketing pair is found by INTEGER comparison on the tick; only the fraction between
            // them is a float, and it is bounded by one key interval rather than by the clip's length.
            const auto it = std::lower_bound( PositionKeys.begin(), PositionKeys.end(), at.Frame );

            if ( it == PositionKeys.begin() )
            {
                return PositionKeys.front().Position;
            }
            if ( it == PositionKeys.end() )
            {
                return PositionKeys.back().Position;
            }

            const auto prev = it - 1;
            const auto next = it;

            const auto span = static_cast<double>( next->Tick.Value - prev->Tick.Value );
            if ( span <= 0.0 )
            {
                // Two keys on the same tick. On floats this was a divide by zero producing an infinity or
                // a NaN that flowed into the pose; on the tick grid it is a state the file can hold and
                // the answer is the later key, which is what a sampler at that tick means.
                return next->Position;
            }
            const auto factor = static_cast<float>( ( at.AsTicks() - prev->Tick.Value ) / span );

            // ONE CALL PER COMPONENT, and no temporary channel built to make them: a tangent is a slope
            // and a slope is a scalar, which is also why UE stores a transform control as nine scalar
            // channels rather than three vector ones.
            const double spanSeconds =
                 span * static_cast<double>( tickRate.Denominator ) / static_cast<double>( tickRate.Numerator );
            glm::vec3 out;
            for ( int c = 0; c < 3; ++c )
            {
                out[c] = EvaluateSegment( prev->Position[c], prev->LeaveTangent[c], next->Position[c],
                                          next->ArriveTangent[c], next->Interp, spanSeconds, factor );
            }
            return out;
        }

        [[nodiscard]] glm::quat GetInterpolatedRotation( FrameTime at ) const
        {
            if ( RotationKeys.empty() )
            {
                return glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
            }
            if ( RotationKeys.size() == 1 )
            {
                return RotationKeys[0].Rotation;
            }

            // The bracketing pair is found by INTEGER comparison on the tick; only the fraction between
            // them is a float, and it is bounded by one key interval rather than by the clip's length.
            const auto it = std::lower_bound( RotationKeys.begin(), RotationKeys.end(), at.Frame );

            if ( it == RotationKeys.begin() )
            {
                return RotationKeys.front().Rotation;
            }
            if ( it == RotationKeys.end() )
            {
                return RotationKeys.back().Rotation;
            }

            const auto prev = it - 1;
            const auto next = it;

            const auto span = static_cast<double>( next->Tick.Value - prev->Tick.Value );
            if ( span <= 0.0 )
            {
                // Two keys on the same tick. On floats this was a divide by zero producing an infinity or
                // a NaN that flowed into the pose; on the tick grid it is a state the file can hold and
                // the answer is the later key, which is what a sampler at that tick means.
                return next->Rotation;
            }
            const auto factor = static_cast<float>( ( at.AsTicks() - prev->Tick.Value ) / span );

            // CONSTANT OR SLERP, and there is no third branch to add later without adding `squad` with
            // it. A rotation key states its shape like every other key; `Cubic` is refused where a clip
            // is BUILT (AnimationClipBuild), so it cannot reach here and be quietly treated as linear.
            if ( next->Interp == KeyInterp::Constant )
            {
                return prev->Rotation;
            }
            return glm::slerp( prev->Rotation, next->Rotation, factor );
        }

        [[nodiscard]] glm::vec3 GetInterpolatedScale( FrameTime at, FrameRate tickRate ) const
        {
            if ( ScaleKeys.empty() )
            {
                return glm::vec3( 1.0f );
            }
            if ( ScaleKeys.size() == 1 )
            {
                return ScaleKeys[0].Scale;
            }

            // The bracketing pair is found by INTEGER comparison on the tick; only the fraction between
            // them is a float, and it is bounded by one key interval rather than by the clip's length.
            const auto it = std::lower_bound( ScaleKeys.begin(), ScaleKeys.end(), at.Frame );

            if ( it == ScaleKeys.begin() )
            {
                return ScaleKeys.front().Scale;
            }
            if ( it == ScaleKeys.end() )
            {
                return ScaleKeys.back().Scale;
            }

            const auto prev = it - 1;
            const auto next = it;

            const auto span = static_cast<double>( next->Tick.Value - prev->Tick.Value );
            if ( span <= 0.0 )
            {
                // Two keys on the same tick. On floats this was a divide by zero producing an infinity or
                // a NaN that flowed into the pose; on the tick grid it is a state the file can hold and
                // the answer is the later key, which is what a sampler at that tick means.
                return next->Scale;
            }
            const auto factor = static_cast<float>( ( at.AsTicks() - prev->Tick.Value ) / span );

            // ONE CALL PER COMPONENT, and no temporary channel built to make them: a tangent is a slope
            // and a slope is a scalar, which is also why UE stores a transform control as nine scalar
            // channels rather than three vector ones.
            const double spanSeconds =
                 span * static_cast<double>( tickRate.Denominator ) / static_cast<double>( tickRate.Numerator );
            glm::vec3 out;
            for ( int c = 0; c < 3; ++c )
            {
                out[c] = EvaluateSegment( prev->Scale[c], prev->LeaveTangent[c], next->Scale[c],
                                          next->ArriveTangent[c], next->Interp, spanSeconds, factor );
            }
            return out;
        }
    };

    // Animation notify / event: a named marker at a time (same unit as Duration / key times). Fires once when
    // playback crosses it; the Animator queues crossed notifies and the ECS dispatches them to scripts.
    struct AnimationNotify
    {
        std::string Name;
        FrameNumber Tick;
    };

    class AnimationClip
    {
    public:
        std::string AnimationName;

        /// Length of the clip, in ticks on `TickRate`'s grid.
        FrameNumber DurationTicks;

        /// The resolution the keys are counted at, and the grid an artist edits on. TWO numbers, because
        /// they answer two questions — see TimeModel.hpp. `TicksPerSecond`, a float that every shipped
        /// clip set to 1.0 so that "tick" meant "second", is what they replace.
        FrameRate TickRate    = PROJECT_TICK_RATE;
        FrameRate DisplayRate = DEFAULT_DISPLAY_RATE;
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

        std::vector<AnimationNotify> Notifies; // sorted-by-tick markers fired during playback

        /// The clip's length in seconds, for the callers whose question really is about seconds — a
        /// crossfade duration, a UI readout, the normalized fraction the AnimGraph gates exit time on.
        /// Derived rather than stored: a second copy of the length is a second answer to it.
        [[nodiscard]] double DurationSeconds() const
        {
            return FrameTimeToSeconds( FrameTime{ DurationTicks, 0.0F }, TickRate );
        }
    };
} // namespace Desert::Animation
