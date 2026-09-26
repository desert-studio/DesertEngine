#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>

#include <filesystem>

namespace Desert::Assets::Serialization
{
    /**
     * @brief The runtime clip -> the `.anim` file's channel list. The exact mirror of
     *        BuildClipFromAssetData, and it lives beside it for that reason.
     *
     * WHY IT EXISTS AS A FUNCTION AT ALL (Д35). This conversion used to be twenty lines inside
     * SequencerPanel::SaveClipToDisk, which is a member of an ImGui panel that drags the renderer, the
     * ECS and the asset manager in with it — so the WRITE half of the `.anim` format was the one half
     * no suite could compile, while the READ half had had its own suite since AnimationClipFormat. A
     * format whose two directions are not testable together is a format whose round trip is an
     * assumption.
     */
    [[nodiscard]] AnimationAssetData BuildAssetDataFromClip( const Animation::AnimationClip& clip );

    /**
     * @brief Serialise @p clip and write it to @p path, reporting whether the BYTES ARRIVED.
     *
     * THE DEFECT THIS REPLACES, because it is the point of the function. The panel did:
     *
     *     std::ofstream out( path, std::ios::binary );
     *     if ( !out ) { LOG_WARN(...); return {}; }
     *     out << WriteAnimationJson( data );
     *     return path.string();                 // <-- the success value
     *
     * There was no check after the insertion at all. `operator<<` fills the filebuf; the bytes reach
     * the OS at the flush, and without an explicit close() the flush is `~ofstream`, running after the
     * path has already been handed back as proof of a save. A person pressed Save in the Sequencer,
     * was told the clip was saved, and was given the path of a file that could be empty.
     *
     * It goes through Common::Utils::FileSystem::WriteContentToFileAtomic, which closes before it
     * decides and writes through a temporary — so a failed save also cannot destroy the clip that was
     * on disk before it.
     */
    [[nodiscard]] Common::BoolResultStr SaveClipToFile( const std::filesystem::path&    path,
                                                        const Animation::AnimationClip& clip );
} // namespace Desert::Assets::Serialization
