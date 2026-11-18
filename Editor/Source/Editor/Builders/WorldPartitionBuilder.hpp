#pragma once

// THE WORLD PARTITION BUILDERS WITHOUT A WINDOW — UE 5.5's UWorldPartitionBuilderCommandlet and UWorldPartition-
// Builder, as the editor's own run mode: `DesertEditor --run WorldPartitionBuilder --builder <Name> --scene <file>`
// (UE's `-run=WorldPartitionBuilderCommandlet -Builder=<Class> <Map>`; `--name=value` is the same flag).
//
// WHY THE EDITOR AND NOT WorldCook. A builder is editor work: the HLODs builder bakes its landscape tiles on the
// GPU, Convert and FoliageRepartition run the editor's own operations, and all of them save through the editor's
// one save (SceneFiles::SaveOpenScene). The editor already has the renderer, the asset manager and that save; a
// builder run is the editor started with its main window never shown and no splash (EditorStartup reveals only
// over a splash), which runs ONE builder on the scene the command line names once its content has settled, and
// exits with the builder's status: 0 built, 1 refused or failed (the log says why), 2 a command line refused
// before anything ran. WorldCook stays the cook.
//
// THE PATTERN IS UE's. A builder states how the world is loaded for it (ELoadingMode): Custom - it reads what it
// needs itself (HLODs read the SAVED world); EntireWorld - every entity of every data layer is loaded first;
// IterativeCells2D - the world's ground is cut into squares of IterativeCellSize and each is loaded with the
// previous one unloaded (EditorRegions::LoadRegions), so a world larger than memory is built a region at a time.
// The run calls PreRun once, Run once per cell (Continue on the next frames while a step answers Pending - a GPU
// bake spans frames), and PostRun with the verdict. The builders and their names are one table
// (WorldPartitionBuilders.cpp); a new builder is a row there.

#include <Engine/Core/Serialize/EditorRegions.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>

#include <Common/Core/ResultStr.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    class SceneFiles;
    class SceneWorkspace;
    struct BuilderRunOptions;

    /// UE UWorldPartitionBuilder::ELoadingMode, the modes this engine has.
    enum class BuilderLoadingMode
    {
        Custom,           // the builder reads what it needs; the run loads nothing
        EntireWorld,      // the whole world, every data layer, before Run
        IterativeCells2D, // one square of the ground at a time, the previous one unloaded
    };

    /// What a step of a builder answers: finished, or more work on the next frames (Continue).
    enum class BuilderStep
    {
        Done,
        Pending,
    };

    /// What a builder works on (UE: the UWorld and the package helper the commandlet hands RunInternal).
    struct BuilderContext
    {
        std::shared_ptr<::Desert::Core::Scene>         Scene;     // the open scene: the world, or the region of it loaded
        Assets::AssetManager*                   Assets = nullptr;
        std::filesystem::path                   ScenePath; // the world's file, as the editor opened it
        const BuilderRunOptions*                Options = nullptr;
        std::function<Common::BoolResultStr()>  Save;      // the editor's one save of the open scene
    };

    /// One region of a run: a square of the ground (IterativeCells2D), or the whole world (Whole).
    struct BuilderCell
    {
        ::Desert::Core::Rules::CellBounds Bounds; // X/Z, cm; unused when Whole
        std::size_t             Index = 0;
        std::size_t             Count = 1;
        bool                    Whole = true;
    };

    /// UE UWorldPartitionBuilder: PreRun, RunInternal per cell, PostRun.
    class IWorldPartitionBuilder
    {
    public:
        virtual ~IWorldPartitionBuilder() = default;

        /// Once, after the world is open and before any cell is loaded. A refusal ends the run.
        [[nodiscard]] virtual Common::BoolResultStr PreRun( BuilderContext& context );
        /// Once per cell, with the cell loaded (EntireWorld and Custom: once, Whole).
        [[nodiscard]] virtual Common::ResultStr<BuilderStep> Run( BuilderContext& context, const BuilderCell& cell ) = 0;
        /// Every frame after a Run (or a Continue) answered Pending, until it answers Done. A builder whose steps
        /// never answer Pending is never asked; one that does and has no continuation is refused by name.
        [[nodiscard]] virtual Common::ResultStr<BuilderStep> Continue( BuilderContext& context );
        /// Once, with the run's verdict, whatever it was.
        [[nodiscard]] virtual Common::BoolResultStr PostRun( BuilderContext& context, bool succeeded );
    };

    /// One row of the builder table (WorldPartitionBuilders.cpp).
    struct WorldPartitionBuilderRow
    {
        std::string_view   Name;             // `--builder <Name>`
        std::string_view   Summary;          // what it does, for the list a wrong name is answered with
        BuilderLoadingMode Mode;             // its own loading mode
        bool               CellsAllowed;     // `--builder-cell-size` may run it IterativeCells2D
        bool               OnlyDirtyAllowed; // `--only-dirty` means something to it
        std::unique_ptr<IWorldPartitionBuilder> ( *Make )();
    };

    /// The builder table, in the order the list is written.
    [[nodiscard]] std::span<const WorldPartitionBuilderRow> WorldPartitionBuilderRows();
    /// The row named @p name, or null.
    [[nodiscard]] const WorldPartitionBuilderRow* FindWorldPartitionBuilder( std::string_view name );
    /// What the process checks before anything starts: the builder is in the table, and `--builder-cell-size` /
    /// `--only-dirty` mean something to it. Refused with the table's list.
    [[nodiscard]] Common::BoolResultStr ValidateBuilderRequest( const BuilderRunOptions& options );

    /// PURE. The squares of side @p cellSize (cm) an IterativeCells2D run visits over @p world: aligned to
    /// multiples of the side (UE's iterative cells are), row by row in X then Z, every square that meets the world.
    [[nodiscard]] std::vector<::Desert::Core::Rules::CellBounds> IterativeCells2D( const ::Desert::Core::Rules::CellBounds& world,
                                                                         float                           cellSize );

    /// THE RUN (UE UWorldPartitionBuilderCommandlet::Main). A member of EditorLayer; idle unless the command line
    /// asked for a builder run.
    class WorldPartitionBuilderRun
    {
    public:
        WorldPartitionBuilderRun( SceneWorkspace& workspace, SceneFiles& files,
                                  std::shared_ptr<Assets::AssetManager>& assets );
        ~WorldPartitionBuilderRun();

        /// One editor frame, called once the startup stages are done. @p sceneSettled: the scene the command line
        /// named is open and its content has settled - the run starts on the first such frame. Answers the exit
        /// status on the frame the run ends, nothing before (and nothing ever outside a builder run).
        [[nodiscard]] std::optional<int> Tick( bool sceneSettled );

    private:
        [[nodiscard]] Common::BoolResultStr Begin();
        // One step: a cell's Run or a pending Continue. True when the last cell is done.
        [[nodiscard]] Common::ResultStr<bool> Step();
        [[nodiscard]] int Finish( Common::BoolResultStr outcome );

        SceneWorkspace&                        m_Workspace;
        SceneFiles&                            m_Files;
        std::shared_ptr<Assets::AssetManager>& m_Assets;

        const WorldPartitionBuilderRow*          m_Row = nullptr;
        std::unique_ptr<IWorldPartitionBuilder>  m_Builder;
        BuilderContext                           m_Context;
        ::Desert::Core::EditorRegions::DataLayerOverrides m_Layers; // every data layer of the world loaded
        std::vector<BuilderCell>                 m_Cells;
        std::size_t                              m_Next     = 0;
        bool                                     m_Pending  = false;
        bool                                     m_Started  = false;
        bool                                     m_Finished = false;
        std::chrono::steady_clock::time_point    m_StartedAt;
    };
} // namespace Desert::Editor
