#include "AnimationClipBuild.hpp"

#include <Engine/Animation/Timeline/Sequence.hpp>

#include <Common/Json/Json.hpp>

#include <format>
#include <string>
#include <utility>

namespace Desert::Assets::Serialization
{
    Common::ResultStr<Animation::AnimationClip> BuildClipFromAssetData( const AnimationAssetData& data )
    {
        const std::string block = Common::Json::Write( data.Sequence );
        auto sequence = Animation::Timeline::ReadSequence( std::span<const uint8_t>(
             reinterpret_cast<const uint8_t*>( block.data() ), block.size() ) );
        if ( !sequence )
        {
            return Common::MakeFormattedError<Animation::AnimationClip>( "clip '{}': {}", data.Name,
                                                                         sequence.GetError() );
        }
        if ( sequence.GetValue().Host != Animation::Timeline::SequenceHost::AnimationClip )
        {
            return Common::MakeFormattedError<Animation::AnimationClip>(
                 "clip '{}': its timeline block is a {} sequence, not an AnimationClip one", data.Name,
                 Animation::Timeline::ToString( sequence.GetValue().Host ) );
        }
        Animation::AnimationClip clip;
        clip.AnimationName     = data.Name;
        if ( data.Skeleton )
        {
            auto skeleton = Common::Content::AssetGuidFromText( data.Skeleton->Guid );
            if ( !skeleton )
                return Common::MakeFormattedError<Animation::AnimationClip>(
                     "clip '{}' names skeleton '{}' by a GUID that does not parse: {}", data.Name,
                     data.Skeleton->Path, skeleton.GetError() );
            clip.Skeleton = skeleton.GetValue();
        }
        clip.Sequence = sequence.ExtractValue();
        return Common::MakeSuccess( std::move( clip ) );
    }

    Common::ResultStr<AnimationAssetData> BuildAssetDataFromClip( const Animation::AnimationClip& clip )
    {
        if ( clip.Sequence.Host != Animation::Timeline::SequenceHost::AnimationClip )
        {
            return Common::MakeFormattedError<AnimationAssetData>(
                 "clip '{}' holds a {} sequence, not an AnimationClip one", clip.AnimationName,
                 Animation::Timeline::ToString( clip.Sequence.Host ) );
        }
        const auto block = Animation::Timeline::WriteSequence( clip.Sequence );
        if ( !block )
        {
            return Common::MakeFormattedError<AnimationAssetData>( "clip '{}': {}", clip.AnimationName,
                                                                   block.GetError() );
        }
        const std::vector<uint8_t>& bytes = block.GetValue();
        auto value = Common::Json::Read<Common::Json::Value>(
             std::string_view( reinterpret_cast<const char*>( bytes.data() ), bytes.size() ) );
        if ( !value )
        {
            return Common::MakeFormattedError<AnimationAssetData>( "clip '{}': the TMLN block is not JSON: {}",
                                                                   clip.AnimationName, value.GetError() );
        }
        AnimationAssetData data;
        // `Skeleton` is not stated here: its AssetGuidRef path is the content registry's (ReferenceTo), which
        // the writer (SaveClipToFile) asks; this function stays pure for Tools/SceneMigrator.
        data.Name     = clip.AnimationName;
        data.Sequence = value.ExtractValue();
        return Common::MakeSuccess( std::move( data ) );
    }
} // namespace Desert::Assets::Serialization
