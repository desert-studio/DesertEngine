#pragma once

// BUILD HLODs OF A SAVED WORLD — UE's Build > Build HLODs and the HLODs builder of its builder commandlet
// (UWorldPartitionHLODsBuilder), as ONE service: the World Partition panel's button and the windowless builder
// run (Editor/Builders/WorldPartitionBuilders.cpp) both start it, and nothing else builds a world's HLODs.
//
// It reads the world SAVED at the scene's file (scene + entity files, exactly as WorldCook reads them), so the
// checksums the build stores are the ones a cook of the same files computes; the open scene is not read. Two
// phases: Start checks the world and begins the landscape tiles' BaseColor bake on the GPU (LandscapeHLODBake),
// which runs over the next editor frames - a hidden window renders them as a shown one does; Tick finishes it -
// every cell's HLOD built by MakeHLODBuilders with each landscape HLOD bound to its baked material, written to
// `<Scene>_HLOD/` cell by cell (WorldCells::BuiltHLODWriter), read back and checked stale-free. A world without
// landscape HLODs is built inside Start.

#include "LandscapeHLODBake.hpp"

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    class HLODBuild
    {
    public:
        /// Begins a Build HLODs of the world saved at @p scenePath (@p sceneName names it in the log). Refused, by
        /// name, for a world the build refuses (its files unreadable, a member the builders cannot build).
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<HLODBuild>>
        Start( const std::filesystem::path& scenePath, std::string sceneName, Assets::AssetManager* assets );

        ~HLODBuild();
        HLODBuild( const HLODBuild& )            = delete;
        HLODBuild& operator=( const HLODBuild& ) = delete;

        /// One editor frame of the build: nothing while the landscape bake runs, then the build's outcome - the
        /// world's HLOD files written and read back stale-free, or why not.
        [[nodiscard]] std::optional<Common::BoolResultStr> Tick();

    private:
        HLODBuild( std::filesystem::path scenePath, std::string sceneName );

        // The second phase: build from the saved files (each landscape HLOD bound to its @p baked material),
        // write, read back.
        [[nodiscard]] Common::BoolResultStr Finish( const std::vector<LandscapeHLODBaked>& baked ) const;

        std::filesystem::path                m_ScenePath;
        std::string                          m_SceneName;
        std::unique_ptr<LandscapeHLODBake>   m_Bake;    // the landscape BaseColor bake; null once it is over
        std::optional<Common::BoolResultStr> m_Outcome; // set when the build is over (Start or the bake's end)
    };

    /// UE's DeleteHLODs builder (UWorldPartitionHLODsBuilder -DeleteHLODs): the HLOD files built for the world
    /// saved at @p scenePath (`<Scene>_HLOD/`, WorldCells::BuiltHLODDirectory) are removed, and the world is then
    /// one a cook refuses until its HLODs are built again. Answers how many files went; a world with none built
    /// answers 0. Refused, by name, when the directory cannot be removed.
    [[nodiscard]] Common::ResultStr<std::size_t> DeleteBuiltHLODs( const std::filesystem::path& scenePath );
} // namespace Desert::Editor
