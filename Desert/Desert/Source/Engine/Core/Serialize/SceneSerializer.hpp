#pragma once

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/SceneLoadPhases.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <span>
#include <string_view>

namespace Desert::Core
{
    class SceneSerializer
    {
    public:
        explicit SceneSerializer( const Scene* scene, const Assets::AssetManager* assetManager );

        std::string SerializeToJson() const;

        /// Loads a scene from the JSON text of a .desce file into the scene this serializer was made for.
        ///
        /// FAILS, rather than repairs, on a file that is not at the current generation of the format
        /// (Core::kSceneVersion / Core::kUnitVersion). The error names the file, what it is, what is
        /// required and the exact SceneMigrator command that converts it — and NOTHING is created for it:
        /// not an entity, not a setting, not the scene name. The scene is left exactly as it was.
        ///
        /// @param source what to call this file in that error. A PATH when there is one; the play-mode
        ///        snapshot has no file, so it says so. It is never used to open anything - this function
        ///        does not touch the disk, and passing the path is only how the message can name it.
        [[nodiscard]] Common::BoolResultStr DeserializeFromJson( const std::string& json,
                                                                 std::string_view   source ) const;

        /// Writes the scene to @p path and SAYS WHETHER THE BYTES LANDED.
        ///
        /// This used to return void, and so did everything above it — Scene::Serialize and the editor's
        /// Ctrl+S — which made a failed save indistinguishable from a successful one all the way up to
        /// the user: the "unsaved changes" star went out and a green "Saved 'X'" toast appeared for a
        /// scene that was still only in memory. The error names the destination and the step that
        /// failed; on failure the file on disk is byte-identical to what it was (the write primitive is
        /// write-then-rename), so the right thing for a caller to do is keep the scene dirty and say so.
        ///
        /// IT ALSO USED TO CHOOSE THE DESTINATION ITSELF, from the scene's name — `Scene/` plus the name
        /// with its spaces turned into underscores. That is why a scene opened from
        /// `Scene/U52_LockProbe.desce` and named "U52 Lock Probe" saved into `Scene/U52_Lock_Probe.desce`
        /// while the file the user had open was never written: a second copy appeared beside the first,
        /// the editor said "Saved", and the next open of "the same" level showed the work missing. The
        /// derivation is gone rather than corrected — a display name and a file identity are two things,
        /// and only whoever opened the file knows the second one. There is no `TargetPath()` any more for
        /// the same reason: nothing may compute this path except the caller that owns it.
        [[nodiscard]] Common::BoolResultStr SaveToFile( const Common::Filepath& path ) const;

        /// Makes entities of @p records in the scene: the identity stitch, then the same three passes a whole
        /// file goes through (create, fill and attach, instantiate prefabs). DeserializeFromJson calls it with
        /// every record of the file; the world streamer calls it with the records of one cell, so a cell comes
        /// back exactly as a load would have made it. Parents are resolved among @p records only — a unit of
        /// the partition holds whole composites, so a parent is never in another call.
        ///
        /// @param phases where the passes are timed, or null. A whole-file load passes its timeline; the
        ///        streamer passes null, because a cell is a few hundred records made every few frames and a
        ///        line per pass per cell would bury the log it is meant to explain.
        [[nodiscard]] Common::BoolResultStr InstantiateRecords( std::span<const Assets::EntityData> records,
                                                                std::string_view                    sceneName,
                                                                SceneLoadPhases*                    phases ) const;

    private:
        Scene*                m_Scene;
        Assets::AssetManager* m_AssetManager;
    };

    /// A mesh asset's box, from the cooked registry's Bounds column: read without loading the mesh, which is
    /// the point — a partition that had to load every mesh to place it would be the eager boot this engine
    /// removed. What the loader and the world streamer both plan a partition with.
    [[nodiscard]] Rules::AssetBoundsSource RegistryMeshBounds();

} // namespace Desert::Core