#pragma once

// SWITCHING A WORLD ON: UE's "Convert Level to World Partition", as a decision rather than a commandlet.
//
// UE's analogue is UWorldPartitionConvertCommandlet (Engine/Source/Editor/UnrealEd/Private/Commandlets/
// WorldPartitionConvertCommandlet.cpp), which is not ported here and could not be: nearly all of it is
// the part of the job this engine does not have — moving every actor of a ULevel out into external
// actor packages, rebuilding the HLOD layers, and re-saving the map through UPackage. Our `.desce` is
// one document whose records are already addressable, so the whole conversion is the one thing UE's
// commandlet does at its start: give the world a partition with the default grid. (The UE 5.8 tree is
// not on this machine, so this file is written against our own format rather than transcribed.)
//
// PURE, AND THAT IS THE POINT. In: the scene's name and the partition it has (none, if it is not
// partitioned). Out: the partition it should have, or a refusal that NAMES the scene. No Scene, no
// panel, no undo stack — so Desert/Tests/Engine/WorldPartition can state the whole rule without an
// editor, and the editor command below it is only the plumbing that stores the answer.

#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <Common/Core/ResultStr.hpp>

#include <optional>
#include <string_view>

namespace Desert::Core::Rules
{
    // The one label the button and the command palette both show, so the two cannot drift apart.
    inline constexpr std::string_view kConvertToWorldPartitionLabel = "Convert scene to World Partition";

    /// The partition an UNPARTITIONED world gets: exactly one grid, at the format's own defaults
    /// (WorldPartitionGridSerialized — 128 m cells, 256 m loading range, in centimetres).
    ///
    /// EXACTLY ONE GRID, and not a copy of the numbers. The defaults live in the struct, which is the
    /// single place that states what a fresh grid is; restating 12800 here would be the second source
    /// of truth that makes a default change silently stop applying to new worlds.
    [[nodiscard]] inline WorldPartitionSerialized DefaultWorldPartition()
    {
        WorldPartitionSerialized partition;
        partition.Grids.emplace_back();
        return partition;
    }

    /// Answers what @p sceneName's partition becomes when the user asks to convert it.
    ///
    /// REFUSES A WORLD THAT IS ALREADY PARTITIONED, BY NAME. Doing it again would silently replace a
    /// grid the author had tuned with the default one — the substitution the contract forbids — and
    /// answering "done" while changing nothing is the silent no-op it forbids next to it. So the caller
    /// gets a sentence it can show, naming the scene and the grids it already has.
    [[nodiscard]] inline Common::ResultStr<WorldPartitionSerialized>
    ConvertToWorldPartition( std::string_view sceneName, const std::optional<WorldPartitionSerialized>& current )
    {
        if ( current.has_value() )
            return Common::MakeFormattedError<WorldPartitionSerialized>(
                 "'{}' is already partitioned: it states {} grid(s), the first with {:.0f} m cells. "
                 "Converting again would replace them with the default grid, so nothing was changed.",
                 sceneName, current->Grids.size(),
                 current->Grids.empty() ? 0.0 : static_cast<double>( current->Grids.front().CellSize ) / 100.0 );

        return Common::MakeSuccess( DefaultWorldPartition() );
    }
} // namespace Desert::Core::Rules
