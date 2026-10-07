#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>

#include <filesystem>

namespace Desert::Assets::Serialization
{

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

    /**
     * @brief The same write with the file's identity STATED by the caller instead of kept from the file being
     *        replaced: engine content generated with fixed GUIDs (Geometry::ProceduralCharacterFactory) states
     *        the same GUID on every regeneration. A null @p identity is refused.
     */
    [[nodiscard]] Common::BoolResultStr SaveClipToFile( const std::filesystem::path&      path,
                                                        const Animation::AnimationClip&   clip,
                                                        const Common::Content::AssetGuid& identity );
} // namespace Desert::Assets::Serialization
