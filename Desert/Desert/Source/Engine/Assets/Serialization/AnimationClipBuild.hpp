#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>

namespace Desert::Assets::Serialization
{
    /**
     * @brief The one reader of a `.anim` body (ANIM v5): the TMLN block through `Timeline::ReadSequence`.
     *
     * Refuses by name a block that is not a TMLN document, one that fails `Validate`, and one whose host is
     * not `AnimationClip` (a UI clip or a level sequence saved under `.anim` is a file no clip consumer can
     * play).
     */
    [[nodiscard]] Common::ResultStr<Animation::AnimationClip>
    BuildClipFromAssetData( const AnimationAssetData& data );

    /**
     * @brief The one writer of a `.anim` body: the clip's sequence as its TMLN block (`WriteSequence`).
     *
     * The header is left to the caller (SaveClipToFile keeps the file's GUID; the migrator keeps the gen-3
     * file's). Refuses what `WriteSequence` refuses, rather than writing an error text as the body.
     */
    [[nodiscard]] Common::ResultStr<AnimationAssetData>
    BuildAssetDataFromClip( const Animation::AnimationClip& clip );
} // namespace Desert::Assets::Serialization
