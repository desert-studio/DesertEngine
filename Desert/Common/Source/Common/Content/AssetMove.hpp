#pragma once

#include <Common/Content/AssetRedirector.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <filesystem>
#include <optional>
#include <string>
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
} // namespace Common::Content
