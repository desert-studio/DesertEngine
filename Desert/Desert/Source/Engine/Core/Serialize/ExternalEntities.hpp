#pragma once

// A PARTITIONED WORLD KEEPS ONE FILE PER ENTITY (SCNE v35, WP16).
//
// The pattern is UE's One File Per Actor, and not its letter: a level states its header, and every actor lives
// in its own package under `__ExternalActors__/<level>/<hash>/<guid>.uasset`, so two people editing two actors
// of one map never touch the same file. Here the scene file of a partitioned world (one that states a
// `WorldPartition` block - presence is the switch, SceneFormat.hpp) keeps everything a scene states EXCEPT its
// entity records; in their place it lists the records' ids, in scene order, as `ExternalEntities`. Each record
// is the file
//
//     <scene dir>/__ExternalEntities__/<scene file stem>/<bucket>/<id>.deent
//
// where `bucket` is the id's low byte in two hex digits (UE's hash folder: it keeps one directory from holding
// a whole world's worth of files). The path is a function of the id alone, so it never moves while the entity
// exists, and an edit to one entity rewrites exactly that entity's file.
//
// WHY THE HEADER LISTS THE IDS, WHEN UE'S LEVEL DOES NOT. UE finds its actors by scanning the folder, so a file
// that is missing is an actor that silently is not there. Here a missing file is a REFUSAL naming the path and
// the id (the list says it must exist), and a file nobody lists is a refusal too (a leftover of a delete that
// did not finish, or of a merge that kept the file and dropped the entity). The price is that adding or
// removing an entity edits the header's list; editing one never does.
//
// WHY AN ENTITY FILE STATES NO VERSION. It is a piece of ONE scene document, not a document of its own: the
// scene is split on save and joined on load, byte for byte (Assemble after Split is the identity on the
// canonical text), so the scene header's SCNE generation is the generation of every piece. A schema step is
// run by Tools/SceneMigrator on the joined scene and written back split, the same way it runs on any scene.
//
// WHERE IT SITS. Below the typed tree and above the text: the loader joins the pieces into the one document the
// rest of the load has always parsed (ReadSceneFileText), and the saver splits the one document it has always
// composed (WriteSceneFile) - so the foreign-key carry, the version gate and every in-memory snapshot (Play,
// the World Partition panel) are the same code for a partitioned world as for any other.

#include <Common/Content/ExternalEntitiesFolder.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>
#include <Common/Json/Carry.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Core::ExternalEntities
{
    inline constexpr std::string_view kFolder     = Common::Content::kExternalEntitiesFolder;
    inline constexpr std::string_view kExtension  = ".deent";
    inline constexpr std::string_view kListMember = "ExternalEntities";
    inline constexpr std::string_view kRecords    = "Entities";

    // `<scene dir>/__ExternalEntities__/<scene file stem>` - every piece of this scene is below it.
    [[nodiscard]] std::filesystem::path DirectoryOf( const std::filesystem::path& scenePath );

    // The one path the record `id` of the scene at `scenePath` is stored at.
    [[nodiscard]] std::filesystem::path FileOf( const std::filesystem::path& scenePath, Common::UUID id );

    // A scene document cut into its header and its records. PURE.
    struct SplitScene
    {
        Common::Json::TextDocument                                       Header;
        std::vector<std::pair<Common::UUID, Common::Json::TextDocument>> Records; // scene order
    };

    // Refuses, naming `source` and the record's index or id, a record with no id and two records with one id:
    // either would give two entities one file.
    [[nodiscard]] Common::ResultStr<SplitScene> Split( const Common::Json::TextDocument& scene,
                                                       std::string_view                  source );

    // The scene document back from its header and its records' texts, `readRecord(id)` giving the text of one.
    // PURE apart from what `readRecord` does. A record whose text cannot be read or is not the record it is
    // filed as is a refusal naming the id and what `readRecord` said.
    using RecordReader = std::function<Common::ResultStr<std::string>( Common::UUID )>;
    [[nodiscard]] Common::ResultStr<Common::Json::TextDocument>
    Assemble( const Common::Json::TextDocument& header, std::string_view source, const RecordReader& readRecord );

    // What a file-level split write did.
    struct WriteOutcome
    {
        std::size_t Written   = 0; // files whose bytes changed (the header counts)
        std::size_t Unchanged = 0; // files whose bytes were already these
        std::size_t Removed   = 0; // record files of entities the scene no longer has
    };

    // THE ONE WRITER OF A SCENE FILE (WP16b): the editor's save and autosave, the device-lost save and
    // Tools/WorldGen all write through it, so the layout on disk is decided in exactly one place.
    //   - a partitioned world (the document states `WorldPartition`): `scenePath` gets the header plus one
    //     canonical file per record, rewriting only the files whose text differs and deleting every `.deent`
    //     below DirectoryOf(scenePath) that no record claims;
    //   - any other scene: `scenePath` gets the whole document as canonical text, and every `.deent` a
    //     partitioned past of this scene left below DirectoryOf(scenePath) is deleted (counted as Removed) -
    //     the loader does not look there for such a scene, so the files would be dead weight that the next
    //     partition of this scene would refuse as unlisted.
    // A failure names the file; files written before it stay written (each write is atomic, so no file is
    // ever half-written).
    [[nodiscard]] Common::ResultStr<WriteOutcome> WriteSceneFile( const std::filesystem::path&      scenePath,
                                                                  const Common::Json::TextDocument& scene );

    // WriteSceneFile of a scene held as TEXT (the autosave's and the device-lost save's SerializeToJson output,
    // WorldGen's typed writer): parsed, then written the same way. Text that is not JSON is refused naming
    // `scenePath`.
    [[nodiscard]] Common::ResultStr<WriteOutcome> WriteSceneText( const std::filesystem::path& scenePath,
                                                                  std::string_view             json );

    // The text of the scene at `path`, as ONE document with its entities inline - what ParseLoadableScene reads:
    //   - a scene that states no partition: the file's bytes, untouched;
    //   - a partitioned header: the header joined with every listed record's file. A listed file that is
    //     missing is a refusal naming the path and the id; a `.deent` below the scene's folder that the list
    //     does not name is a refusal naming the path;
    //   - a partitioned world that states its records INLINE: refused - that is the layout before v35, and the
    //     fix is Tools/SceneMigrator, which the message names.
    [[nodiscard]] Common::ResultStr<std::string> ReadSceneFileText( const std::filesystem::path& path );

    // True when `document` is a partitioned header (states ExternalEntities).
    [[nodiscard]] bool IsHeader( const Common::Json::TextDocument& document );
} // namespace Desert::Core::ExternalEntities
