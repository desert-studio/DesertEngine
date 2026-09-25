#include "AnimationClipWrite.hpp"
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>
#include <Common/Content/CanonicalText.hpp>

#include <Common/Core/Core.hpp> // BOOLSUCCESS
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <rflcpp/rfl/json.hpp>

namespace Desert::Assets::Serialization
{
    AnimationAssetData BuildAssetDataFromClip( const Animation::AnimationClip& clip )
    {
        AnimationAssetData data;
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
                channel.Positions.push_back(
                     KeyPosition{ k.Tick.Value, k.Position,
                                  KeyShape{ static_cast<int>( k.Interp ), static_cast<int>( k.Mode ), 0.0f, 0.0f },
                                  k.ArriveTangent, k.LeaveTangent } );

            channel.Rotations.reserve( track.RotationKeys.size() );
            for ( const auto& k : track.RotationKeys )
                channel.Rotations.push_back( KeyRotation{
                     k.Tick.Value, k.Rotation, KeyShape{ static_cast<int>( k.Interp ), 0, 0.0f, 0.0f } } );

            channel.Scales.reserve( track.ScaleKeys.size() );
            for ( const auto& k : track.ScaleKeys )
                channel.Scales.push_back(
                     KeyScale{ k.Tick.Value, k.Scale,
                               KeyShape{ static_cast<int>( k.Interp ), static_cast<int>( k.Mode ), 0.0f, 0.0f },
                               k.ArriveTangent, k.LeaveTangent } );

            data.Channels.push_back( std::move( channel ) );
        }

        data.Sections.reserve( clip.Sections.size() );
        for ( const auto& section : clip.Sections )
        {
            SectionData out;
            out.Name      = section.Name;
            out.StartTick = section.Start.Value;
            out.EndTick   = section.End.Value;
            out.Blend     = static_cast<int32_t>( section.Blend );
            out.Tracks    = section.Tracks;
            out.Weight.reserve( section.Weight.size() );
            for ( const auto& k : section.Weight )
                out.Weight.push_back( SectionWeightKey{
                     k.Tick.Value, k.Value,
                     KeyShape{ static_cast<int>( k.Interp ), static_cast<int>( k.Mode ), 0.0f, 0.0f },
                     k.ArriveTangent, k.LeaveTangent } );
            data.Sections.push_back( std::move( out ) );
        }

        data.Notifies.reserve( clip.Notifies.size() );
        for ( const auto& notify : clip.Notifies )
            data.Notifies.push_back( NotifyData{ notify.Name, notify.Tick.Value } );

        // An in-memory clip that never got a section is written with the one it behaves as, so no
        // generation-3 file can be silent about what its values mean. One producer for all three writers.
        EnsureStatedSections( data );

        return data;
    }

    Common::BoolResultStr SaveClipToFile( const std::filesystem::path& path, const Animation::AnimationClip& clip )
    {
        // A SAVE KEEPS THE CLIP'S IDENTITY: the GUID the file being replaced states, minted only for a new
        // file (ANIM 4, T7e). Sequencer tracks and anim graphs name the clip, and a fresh GUID would orphan
        // them. The header is stamped here, at the writer, so the version it states is this build's.
        AnimationAssetData data = BuildAssetDataFromClip( clip );
        data.Header =
             HeaderKeepingFileGuid( path, Common::Content::ContentKind::Animation, AnimationTextSubsystems() );
        const auto canonicalJson = Common::Content::CanonicalJsonTextOfWriterOutput( WriteAnimationJson( data ) );
        if ( !canonicalJson )
            return Common::MakeError<bool>( canonicalJson.GetError() );
        const std::string& json = canonicalJson.GetValue();

        // The verdict is the primitive's, and it is read before this function returns. See the header
        // for the shape this replaces.
        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, json ); !written )
            return Common::MakeFormattedError<bool>( "clip '{}' ({} bytes) was not saved to '{}': {}",
                                                     clip.AnimationName, json.size(), path.string(),
                                                     written.GetError() );

        return BOOLSUCCESS;
    }
} // namespace Desert::Assets::Serialization
