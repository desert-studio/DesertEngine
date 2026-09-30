#include "AnimationClipWrite.hpp"
#include <Engine/Assets/ContentRegistry.hpp>
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
        // The GUID resolves; the path is the registry's key for it, for the reader (AssetGuidRef).
        if ( !clip.Skeleton.IsNull() )
            data.Skeleton = ContentRegistry::ReferenceTo( clip.Skeleton );

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
            data.Notifies.push_back(
                 NotifyData{ notify.Name, notify.Tick.Value, notify.Track, notify.DurationTicks.Value } );

        data.Curves.reserve( clip.Curves.size() );
        for ( const auto& curve : clip.Curves )
        {
            CurveData out;
            out.Name = curve.Name;
            out.Keys.reserve( curve.Keys.size() );
            for ( const auto& k : curve.Keys )
                out.Keys.push_back( SectionWeightKey{
                     k.Tick.Value, k.Value,
                     KeyShape{ static_cast<int>( k.Interp ), static_cast<int>( k.Mode ), 0.0f, 0.0f },
                     k.ArriveTangent, k.LeaveTangent } );
            data.Curves.push_back( std::move( out ) );
        }

        // An in-memory clip that never got a section is written with the one it behaves as, so no
        // generation-3 file can be silent about what its values mean. One producer for all three writers.
        EnsureStatedSections( data );

        return data;
    }

    namespace
    {
        // THE SOURCE THE FILE BEING REPLACED NAMES (THM-FIXJ), kept as its GUID is: a save of an imported clip
        // must not cut it from the source its Reimport re-imports. nullopt for a new file or a hand-authored
        // clip; an error when the file there is not a clip this build reads - a save must not guess over it.
        Common::ResultStr<std::optional<ImportSourceInfo>>
        ImportOfFileBeingReplaced( const std::filesystem::path& path )
        {
            const auto text = Common::Utils::FileSystem::ReadFileContentIfExists( path );
            if ( !text )
                return Common::MakeError<std::optional<ImportSourceInfo>>( text.GetError() );
            const auto& contents = text.GetValue();
            if ( !contents.has_value() )
                return Common::MakeSuccess( std::optional<ImportSourceInfo>{} );
            const auto clip = ReadAnimationJson( *contents );
            if ( !clip )
                return Common::MakeError<std::optional<ImportSourceInfo>>(
                     std::format( "'{}' is replaced by a save, but {}", path.string(), clip.GetError() ) );
            return Common::MakeSuccess( clip.GetValue().Import );
        }
    } // namespace

    Common::BoolResultStr SaveClipToFile( const std::filesystem::path& path, const Animation::AnimationClip& clip )
    {
        // A SAVE KEEPS THE CLIP'S IDENTITY: the GUID the file being replaced states, minted only for a new
        // file (ANIM 4, T7e). Sequencer tracks and anim graphs name the clip, and a fresh GUID would orphan
        // them. The header is stamped here, at the writer, so the version it states is this build's.
        Common::Content::AssetGuid identity = ReadTextHeaderGuid( path );
        if ( identity.IsNull() )
            identity = Common::Content::AssetGuid::Generate();
        return SaveClipToFile( path, clip, identity );
    }

    Common::BoolResultStr SaveClipToFile( const std::filesystem::path& path, const Animation::AnimationClip& clip,
                                          const Common::Content::AssetGuid& identity )
    {
        if ( identity.IsNull() )
            return Common::MakeFormattedError<bool>( "clip '{}' was not saved to '{}': no identity was stated",
                                                     clip.AnimationName, path.string() );
        AnimationAssetData data = BuildAssetDataFromClip( clip );
        data.Header = Common::Content::MakeTextHeader( Common::Content::ContentKind::Animation, identity,
                                                       AnimationTextSubsystems() );
        const auto import = ImportOfFileBeingReplaced( path );
        if ( !import )
            return Common::MakeError<bool>( import.GetError() );
        data.Import              = import.GetValue();
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
