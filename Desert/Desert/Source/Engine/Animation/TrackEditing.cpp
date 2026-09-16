#include "TrackEditing.hpp"

#include <algorithm>

#include <glm/gtx/matrix_decompose.hpp>

namespace Desert::Animation
{
    namespace
    {
        /// Lift one component of a vec3 channel into the scalar form the tangent maths works in.
        template <class KeyType, class Member>
        std::vector<ScalarKey> LiftComponent( const std::vector<KeyType>& keys, Member member, int component )
        {
            std::vector<ScalarKey> scalars;
            scalars.reserve( keys.size() );
            for ( const KeyType& key : keys )
            {
                ScalarKey scalar;
                scalar.Tick          = key.Tick;
                scalar.Value         = ( key.*member )[component];
                scalar.ArriveTangent = key.ArriveTangent[component];
                scalar.LeaveTangent  = key.LeaveTangent[component];
                scalar.Interp        = key.Interp;
                scalar.Mode          = key.Mode;
                scalars.push_back( scalar );
            }
            return scalars;
        }

        template <class KeyType>
        void RefreshVectorChannel( std::vector<KeyType>& keys, FrameRate tickRate, auto member )
        {
            if ( keys.size() < 2 )
            {
                // One key is a constant and has no neighbours to take a slope from; zero keys is nothing.
                for ( KeyType& key : keys )
                {
                    key.ArriveTangent = glm::vec3( 0.0f );
                    key.LeaveTangent  = glm::vec3( 0.0f );
                }
                return;
            }

            for ( int component = 0; component < 3; ++component )
            {
                std::vector<ScalarKey> scalars = LiftComponent( keys, member, component );
                AutoSetTangents( scalars, tickRate );
                for ( std::size_t i = 0; i < keys.size(); ++i )
                {
                    keys[i].ArriveTangent[component] = scalars[i].ArriveTangent;
                    keys[i].LeaveTangent[component]  = scalars[i].LeaveTangent;
                }
            }
        }
    } // namespace

    std::vector<ScalarKey> LiftChannel( const BoneTrack& track, TrackChannel channel, int component )
    {
        if ( component < 0 || component > 2 )
        {
            return {};
        }
        switch ( channel )
        {
            case TrackChannel::Position:
                return LiftComponent( track.PositionKeys, &PositionKeyFrame::Position, component );
            case TrackChannel::Scale:
                return LiftComponent( track.ScaleKeys, &ScaleKeyFrame::Scale, component );
            case TrackChannel::Rotation:
                // Not a gap — see the header. A quaternion's components are not curves.
                return {};
        }
        return {};
    }

    bool ApplyChannel( BoneTrack& track, TrackChannel channel, int component,
                       const std::vector<ScalarKey>& scalars )
    {
        if ( component < 0 || component > 2 )
        {
            return false;
        }

        const auto write = [&]( auto& keys, auto member )
        {
            if ( keys.size() != scalars.size() )
            {
                return false;
            }
            for ( std::size_t i = 0; i < keys.size(); ++i )
            {
                if ( keys[i].Tick != scalars[i].Tick )
                {
                    return false; // a retime is a different operation — see the header
                }
            }
            for ( std::size_t i = 0; i < keys.size(); ++i )
            {
                ( keys[i].*member )[component]   = scalars[i].Value;
                keys[i].ArriveTangent[component] = scalars[i].ArriveTangent;
                keys[i].LeaveTangent[component]  = scalars[i].LeaveTangent;
                keys[i].Interp                   = scalars[i].Interp;
                keys[i].Mode                     = scalars[i].Mode;
            }
            return true;
        };

        switch ( channel )
        {
            case TrackChannel::Position:
                return write( track.PositionKeys, &PositionKeyFrame::Position );
            case TrackChannel::Scale:
                return write( track.ScaleKeys, &ScaleKeyFrame::Scale );
            case TrackChannel::Rotation:
                return false;
        }
        return false;
    }

    void RefreshTangents( BoneTrack& track, FrameRate tickRate )
    {
        RefreshVectorChannel( track.PositionKeys, tickRate, &PositionKeyFrame::Position );
        RefreshVectorChannel( track.ScaleKeys, tickRate, &ScaleKeyFrame::Scale );
    }

    bool InsertKeyFromCurve( BoneTrack& track, TrackChannel channel, FrameNumber tick, FrameRate tickRate )
    {
        const FrameTime at{ tick, 0.0F };

        const auto occupied = []( const auto& keys, FrameNumber where )
        {
            // An EQUALITY, because A5 put every key on an integer tick. The 1 ms epsilon this replaces had
            // both of an epsilon's failures at once: too small and the animator gets a second key a
            // thousandth of a second from the first, too large and a deliberate pair merges into one.
            return std::any_of( keys.begin(), keys.end(),
                                [where]( const auto& key ) { return key.Tick == where; } );
        };

        switch ( channel )
        {
            case TrackChannel::Position:
            {
                if ( track.PositionKeys.empty() || occupied( track.PositionKeys, tick ) )
                {
                    return false;
                }
                PositionKeyFrame key;
                key.Tick     = tick;
                key.Position = track.GetInterpolatedPosition( at, tickRate );
                key.Interp   = KeyInterp::Cubic;
                key.Mode     = TangentMode::User;
                for ( int component = 0; component < 3; ++component )
                {
                    const std::vector<ScalarKey> scalars =
                         LiftComponent( track.PositionKeys, &PositionKeyFrame::Position, component );
                    const float slope            = SlopeAt( scalars, tick, tickRate );
                    key.ArriveTangent[component] = slope;
                    key.LeaveTangent[component]  = slope;
                }
                track.PositionKeys.push_back( key );
                std::sort( track.PositionKeys.begin(), track.PositionKeys.end() );
                return true;
            }

            case TrackChannel::Rotation:
            {
                if ( track.RotationKeys.empty() || occupied( track.RotationKeys, tick ) )
                {
                    return false;
                }
                RotationKeyFrame key;
                key.Tick     = tick;
                key.Rotation = track.GetInterpolatedRotation( at );
                // No tangents to seed: a rotation key has none, and `Linear` is what the slerp between its
                // neighbours already was.
                key.Interp = KeyInterp::Linear;
                track.RotationKeys.push_back( key );
                std::sort( track.RotationKeys.begin(), track.RotationKeys.end() );
                return true;
            }

            case TrackChannel::Scale:
            {
                if ( track.ScaleKeys.empty() || occupied( track.ScaleKeys, tick ) )
                {
                    return false;
                }
                ScaleKeyFrame key;
                key.Tick   = tick;
                key.Scale  = track.GetInterpolatedScale( at, tickRate );
                key.Interp = KeyInterp::Cubic;
                key.Mode   = TangentMode::User;
                for ( int component = 0; component < 3; ++component )
                {
                    const std::vector<ScalarKey> scalars =
                         LiftComponent( track.ScaleKeys, &ScaleKeyFrame::Scale, component );
                    const float slope            = SlopeAt( scalars, tick, tickRate );
                    key.ArriveTangent[component] = slope;
                    key.LeaveTangent[component]  = slope;
                }
                track.ScaleKeys.push_back( key );
                std::sort( track.ScaleKeys.begin(), track.ScaleKeys.end() );
                return true;
            }
        }
        return false;
    }

    bool InsertFirstKeyFromPose( BoneTrack& track, TrackChannel channel, FrameNumber tick,
                                 const glm::mat4& localPose )
    {
        glm::vec3 scale;
        glm::quat rotation;
        glm::vec3 translation;
        glm::vec3 skew;
        glm::vec4 perspective;
        // A TRS round trip is not the identity on a matrix carrying shear (`Skeleton` says so about
        // OffsetMatrix), and that is fine HERE: a keyframe channel can only express T, R and S, so the
        // shear is not being lost by this decompose — it was never expressible in a key to begin with.
        if ( !glm::decompose( localPose, scale, rotation, translation, skew, perspective ) )
        {
            return false;
        }

        switch ( channel )
        {
            case TrackChannel::Position:
            {
                if ( !track.PositionKeys.empty() )
                {
                    return false;
                }
                PositionKeyFrame key;
                key.Tick     = tick;
                key.Position = translation;
                key.Interp   = KeyInterp::Cubic;
                key.Mode     = TangentMode::Auto;
                track.PositionKeys.push_back( key );
                return true;
            }

            case TrackChannel::Rotation:
            {
                if ( !track.RotationKeys.empty() )
                {
                    return false;
                }
                RotationKeyFrame key;
                key.Tick     = tick;
                key.Rotation = rotation;
                key.Interp   = KeyInterp::Linear;
                track.RotationKeys.push_back( key );
                return true;
            }

            case TrackChannel::Scale:
            {
                if ( !track.ScaleKeys.empty() )
                {
                    return false;
                }
                ScaleKeyFrame key;
                key.Tick   = tick;
                key.Scale  = scale;
                key.Interp = KeyInterp::Cubic;
                key.Mode   = TangentMode::Auto;
                track.ScaleKeys.push_back( key );
                return true;
            }
        }
        return false;
    }
} // namespace Desert::Animation
