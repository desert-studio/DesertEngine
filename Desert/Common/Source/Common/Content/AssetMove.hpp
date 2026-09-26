#pragma once

#include <Common/Content/AssetRedirector.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Common::Content
{
    // RENAME / MOVE OF ONE ASSET, LEAVING A REDIRECTOR AT ITS OLD PATH (AF10c; UE's AssetRenameManager, which
    // leaves an ObjectRedirector behind so every package that still names the old path keeps loading).
    //
    // The file moves by rename (atomic within one volume; across volumes: copy through the atomic writer, then
    // remove the source), then a redirector naming the file's header GUID is written at the old path, then the
    // registry drops the old row and gains two: the moved asset at its new key (its cook-time edges and box
    // carried over, because the file did not change) and the redirector at the old key. Any step that fails
    // undoes the steps before it, so a refusal leaves the disk and the registry as they were.
    //
    // REFUSED, naming the path: a destination that already exists, a destination outside every content root,
    // a change of extension (the kind is the place and the extension; a rename must not change what a file
    // is), a source with no registry row, a source that is itself a redirector, and a source whose header
    // states no GUID (a redirector can only name its target by GUID - skeletons, animations, raw cloud
    // volumes and shaders cannot be moved this way until their formats carry one).
    struct AssetMoveRecord
    {
        std::filesystem::path     From;
        std::filesystem::path     To;
        AssetRedirector           Redirector; // the one written at From
        Utils::AssetRegistryEntry MovedRow;   // the row as it was before the move, restored by undo
    };

    // Every row whose dependency edges name `row` (by its GUID fold, its declared identity or its path
    // handle), as keys in key order. Redirector rows are not listed. The confirm dialog shows these before the
    // move: they are what keeps resolving through the redirector.
    [[nodiscard]] std::vector<std::string> ReferrersOf( const Utils::AssetRegistry&      registry,
                                                        const Utils::AssetRegistryEntry& row );

    // `redirectorSelf` is the redirector's own GUID: generated when absent, passed back by a redo so the
    // redone move writes the very same bytes.
    [[nodiscard]] ResultStr<AssetMoveRecord>
    MoveAssetLeavingRedirector( Utils::AssetRegistry& registry, const std::filesystem::path& from,
                                const std::filesystem::path&    to,
                                const std::optional<AssetGuid>& redirectorSelf = std::nullopt );

    // The inverse: removes the redirector (refused when the file at From is no longer the one the move
    // wrote), moves the asset back, restores the registry's original row.
    [[nodiscard]] BoolResultStr UndoAssetMove( Utils::AssetRegistry& registry, const AssetMoveRecord& record );
    // RENAME / MOVE OF A FOLDER (AF10c): every file under it the registry has a row for moves through
    // `MoveAssetLeavingRedirector` - a redirector stays at each old path, so a scene outside the folder that
    // names one of them keeps loading - and every other file (a source image, a note) moves as a plain file.
    // ALL OR NOTHING: a refusal on any file (a taken name in a destination that already exists, a GUID-less
    // asset, a redirector left by an earlier move) takes back every file moved before it, in reverse, and the
    // disk and the registry are as they were. The files are listed by `FileSystem::ListFilesRecursive`, the
    // engine's one content walk.
    //
    // A folder holding no registry row is renamed whole (its empty subfolders go with it): nothing names its
    // files through the registry, so there is nothing to leave a redirector for. A folder that does hold rows
    // keeps existing at its old path (the redirectors live there); subfolders its plain files leave empty are
    // removed. REFUSED, naming the path: a source that is not a folder, a destination inside the source, and a
    // file under the source that exists only in a mounted pak (it cannot be moved on disk).
    struct AssetFolderMoveRecord
    {
        std::filesystem::path                                                From;
        std::filesystem::path                                                To;
        bool                                                                 WholeDirectory = false;
        std::vector<AssetMoveRecord>                                         Assets;     // in move order
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> PlainFiles; // (from, to), in order
        std::vector<std::filesystem::path> CreatedDirectories; // destination folders the move made, in order
    };

    // `redirectorSelves` maps an asset's old path to the redirector GUID a previous run of this move wrote
    // there: a redo passes them back so it writes the very same redirector bytes.
    [[nodiscard]] ResultStr<AssetFolderMoveRecord>
    MoveFolderLeavingRedirectors( Utils::AssetRegistry& registry, const std::filesystem::path& from,
                                  const std::filesystem::path&                      to,
                                  const std::map<std::filesystem::path, AssetGuid>& redirectorSelves = {} );

    // The inverse, in reverse order: plain files back, every asset move undone, the made folders removed.
    [[nodiscard]] BoolResultStr UndoFolderMove( Utils::AssetRegistry&        registry,
                                                const AssetFolderMoveRecord& record );
} // namespace Common::Content
