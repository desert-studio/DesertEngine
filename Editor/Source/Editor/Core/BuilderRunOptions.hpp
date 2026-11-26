#pragma once

// A BUILDER RUN OF THE EDITOR, AS THE COMMAND LINE ASKED FOR IT (UE: `UnrealEditor-Cmd <map>
// -run=WorldPartitionBuilderCommandlet -Builder=<Class> ...`). The values only: CommandLine.hpp parses them,
// Sandbox publishes them before the application exists, and the run itself is Editor/Builders/
// WorldPartitionBuilder.hpp. Pure data, so the parse that fills it stays testable without a GPU.

#include <string>
#include <string_view>

namespace Desert::Editor
{
    struct BuilderRunOptions
    {
        /// The one commandlet this editor runs (`--run WorldPartitionBuilder`). A NAME and not a switch, as in
        /// UE, so the next commandlet is a new name rather than a second flag family.
        static constexpr std::string_view kWorldPartitionBuilder = "WorldPartitionBuilder";

        /// `--run <commandlet>`. Empty — the default — is the editor as a person uses it.
        std::string Run;
        /// `--builder <Name>`: the row of the builder table (Editor/Builders/WorldPartitionBuilders.cpp).
        std::string Builder;
        /// `--builder-cell-size <cm>`: run the builder region by region, each a square of this side on the
        /// ground, the previous one unloaded before the next loads (UE ELoadingMode::IterativeCells2D with
        /// IterativeCellSize). 0 — the default — is the builder's own loading mode.
        float IterativeCellSize = 0.0f;
        /// `--only-dirty` (ResaveEntities): write only the entities whose files the load found out of date,
        /// instead of every entity of the world.
        bool OnlyDirty = false;

        [[nodiscard]] bool Active() const
        {
            return !Run.empty();
        }

        static BuilderRunOptions& Get();
    };
} // namespace Desert::Editor
