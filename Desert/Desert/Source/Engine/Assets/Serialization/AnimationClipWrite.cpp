#include "AnimationClipWrite.hpp"

#include <Common/Core/Core.hpp> // BOOLSUCCESS
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets::Serialization
{
    AnimationAssetData BuildAssetDataFromClip( const Animation::AnimationClip& clip )
    {
        AnimationAssetData data;
        // STAMPED, not inherited. A clip that came from a v0 file cannot reach here — the loader refuses
        // it — so everything written is at the current generation by construction, and saying so in the
        // file is what lets the next build tell the two apart.
        data.Version           = kAnimationVersion;
        data.Name              = clip.AnimationName;
        data.TickRate          = FrameRateData{ clip.TickRate.Numerator, clip.TickRate.Denominator };
        data.DisplayRate       = FrameRateData{ clip.DisplayRate.Numerator, clip.DisplayRate.Denominator };
        data.DurationTicks     = clip.DurationTicks.Value;
        data.SkeletonSignature = clip.SkeletonSignature;

        data.Channels.reserve( clip.Tracks.size() );
        for ( const auto& track : clip.Tracks )
        {
            ChannelData channel;
            channel.BoneName = track.BoneName;

            channel.Positions.reserve( track.PositionKeys.size() );
            for ( const auto& k : track.PositionKeys )
                channel.Positions.push_back( KeyPosition{ k.Tick.Value, k.Position } );

            channel.Rotations.reserve( track.RotationKeys.size() );
            for ( const auto& k : track.RotationKeys )
                channel.Rotations.push_back( KeyRotation{ k.Tick.Value, k.Rotation } );

            channel.Scales.reserve( track.ScaleKeys.size() );
            for ( const auto& k : track.ScaleKeys )
                channel.Scales.push_back( KeyScale{ k.Tick.Value, k.Scale } );

            data.Channels.push_back( std::move( channel ) );
        }

        data.Notifies.reserve( clip.Notifies.size() );
        for ( const auto& notify : clip.Notifies )
            data.Notifies.push_back( NotifyData{ notify.Name, notify.Tick.Value } );

        return data;
    }

    Common::BoolResultStr SaveClipToFile( const std::filesystem::path& path, const Animation::AnimationClip& clip )
    {
        const std::string json = rfl::json::write( BuildAssetDataFromClip( clip ) );

        // The verdict is the primitive's, and it is read before this function returns. See the header
        // for the shape this replaces.
        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, json ); !written )
            return Common::MakeFormattedError<bool>( "clip '{}' ({} bytes) was not saved to '{}': {}",
                                                     clip.AnimationName, json.size(), path.string(),
                                                     written.GetError() );

        return BOOLSUCCESS;
    }
} // namespace Desert::Assets::Serialization
