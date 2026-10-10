#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Common::Content
{
    // ── THE PROJECT'S TRASH: A DELETE THAT CAN BE TAKEN BACK (ASSET-TRASH) ─────────────────────────
    //
    // UE deletes an asset through ObjectTools::DeleteObjects and gets it back from source control
    // (revert). This project's content is not always under source control and Saved/ is machine-local,
    // so the editor's delete MOVES instead of removing: everything the asset is on disk goes into one
    // slot under <Project>/Saved/Trash/, together with the registry rows it held, and a restore puts the
    // very same bytes back at the very same paths and the very same rows back into the registry. The
    // GUID is in the file's header, so the restored asset is the same asset, and every reference that
    // names it (by GUID or by path) resolves again with nothing rewritten.
    //
    // WHAT GOES INTO A SLOT, each file under Files/<n>/<its name>:
    //   * the path itself - one file, or a folder with everything under it;
    //   * its import record (`<source>.deimport`, ImportRecordPathFor) when there is one - the source's
    //     identity, so a restored source is not re-imported as a new asset;
    //   * a partitioned scene's entity folder (ExternalEntitiesDirectoryOf) - the scene without it would
    //     load with every entity missing.
    // Beside them: Trash.json (where each file came from, the GUID) and Rows.json (the registry rows
    // removed, AssetRegistry::Serialize), so a slot is restorable after the editor restarts.
    //
    // REFUSED, naming the path, before anything moves: a path that is not on disk (a file that exists only
    // in a mounted pak cannot be moved). A restore is REFUSED, before anything moves, when any original path
    // is taken again: it never overwrites what was made after the delete.
    struct TrashedFile
    {
        std::filesystem::path Original; // where it lived
        std::filesystem::path Stored;   // where it is in the slot
    };

    struct AssetTrashRecord
    {
        std::filesystem::path                  From; // the path the delete was asked for
        std::filesystem::path                  Slot; // <trash root>/<id>
        std::optional<AssetGuid>               Guid; // the header GUID of a deleted file; none for a folder
        std::vector<TrashedFile>               Files;
        std::vector<Utils::AssetRegistryEntry> Rows; // the registry rows the delete removed
    };

    // <Project>/Saved/Trash - beside Saved/Autosaves, never under a content root (a scan would list it).
    [[nodiscard]] std::filesystem::path ProjectTrashRoot();

    [[nodiscard]] ResultStr<AssetTrashRecord> MoveToTrash( Utils::AssetRegistry&        registry,
                                                           const std::filesystem::path& path,
                                                           const std::filesystem::path& trashRoot );

    // Puts every file back and re-inserts every row; the slot is removed afterwards.
    [[nodiscard]] BoolResultStr RestoreFromTrash( Utils::AssetRegistry& registry, const AssetTrashRecord& record );

    // A slot read back from its Trash.json and Rows.json.
    [[nodiscard]] ResultStr<AssetTrashRecord> ReadTrashSlot( const std::filesystem::path& slot );

    // Every readable slot under @p trashRoot, newest first. A slot that cannot be read is skipped with a
    // warning naming it (it is someone's data: it is left on disk, not removed).
    [[nodiscard]] std::vector<AssetTrashRecord> ListTrash( const std::filesystem::path& trashRoot );
} // namespace Common::Content
