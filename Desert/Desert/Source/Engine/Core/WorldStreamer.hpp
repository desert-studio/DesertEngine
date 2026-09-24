#pragma once

// A PARTITIONED WORLD IN PLAY: ITS RECORDS STAY IN MEMORY, AND ONLY THE NEIGHBOURHOOD OF THE CAMERA IS IN THE ECS.
//
// The decisions are pure and tested (Serialize/WorldPartitionResidencyExecutor.hpp, suite WorldPartitionStreamer);
// this is the Scene side of them — the ResidencyWorld whose Activate is SceneSerializer::InstantiateRecords on
// one unit's records — asked of a WorldCellSource — and whose Destroy is Scene::DestroyEntity.
//
// PLAY ONLY (analysis A8). In Edit the whole world is in the ECS, because saving writes the ECS and a world
// saved with half its cells missing would lose them. The editor takes its Play snapshot BEFORE this starts and
// Stop rebuilds the scene from that snapshot, so what streaming destroyed during Play comes back whole.
//
// THE RECORDS ARE THE SNAPSHOT, PARSED ONCE. They are what the scene was at the moment Play began — the same
// bytes Stop restores — so a cell streamed back in during Play is the cell as it was authored, not as Play left
// it: a record is not written back when its cell leaves. That is the streaming contract of UE's runtime cells
// too (a cell's actors are re-loaded from the package, not from their last runtime state).

#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldCellSource.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Core
{
    class Scene;

    class WorldStreamer final : public Rules::ResidencyWorld
    {
    public:
        // Starts streaming @p scene, whose entities are the whole of @p snapshotJson right now. Returns nullptr —
        // and touches nothing — when the snapshot states no WorldPartition block. The streaming source is the
        // scene's active camera.
        [[nodiscard]] static Common::ResultStr<std::unique_ptr<WorldStreamer>>
        Begin( Scene& scene, Assets::AssetManager& assets, const std::string& snapshotJson );

        // One frame: step the residency from the active camera's position and apply what it decides.
        [[nodiscard]] Common::BoolResultStr Tick( double nowSeconds );

        // WHAT THE LAST Tick DID, for an instrument that attributes a frame's cost (the editor's --flight).
        // Reset at the start of every Tick, so it never carries an earlier frame's activations.
        struct TickReport
        {
            Rules::ResidencyTick Tick;
            double               ActivationMs = 0.0; // summed over this tick's InstantiateRecords calls
            std::string          ActivatedUnits;     // Rules::DescribeResidencyUnit of each, space-separated
            std::size_t          LiveRecords = 0;    // records held as entities after the tick
        };

        [[nodiscard]] const TickReport& LastTick() const
        {
            return m_LastTick;
        }

        // Whether @p scene is the one this streams: the editor updates several scenes a frame.
        [[nodiscard]] bool Streams( const Scene& scene ) const
        {
            return &scene == m_Scene;
        }

        // Says what streaming cost, once: the activation measurement MsPerRecord comes from.
        ~WorldStreamer() override;

        WorldStreamer( const WorldStreamer& )            = delete;
        WorldStreamer& operator=( const WorldStreamer& ) = delete;

        [[nodiscard]] Common::BoolResultStr Activate( std::size_t                  unit,
                                                      std::span<const std::size_t> records ) override;
        void                                Destroy( std::span<const std::size_t> records ) override;

    private:
        WorldStreamer( Scene& scene, Assets::AssetManager& assets, SceneSerialized records );

        [[nodiscard]] Common::ResultStr<Rules::StreamingSource> Source() const;

        Scene*                                  m_Scene;
        Assets::AssetManager*                   m_Assets;
        SceneSerialized                         m_Records;
        std::optional<Rules::ResidencyExecutor> m_Executor;
        // What a unit's records are read from. The snapshot in memory today; WP9 swaps in the cooked cell
        // files without this class changing (Serialize/WorldCellSource.hpp).
        std::unique_ptr<Rules::WorldCellSource> m_Source;

        // What activation actually cost, measured around InstantiateRecords.
        std::size_t m_Activations      = 0;
        std::size_t m_RecordsActivated = 0;
        double      m_ActivationMs     = 0.0;
        double      m_WorstUnitMs      = 0.0;
        std::size_t m_MostResident     = 0;

        TickReport m_LastTick;
    };
} // namespace Desert::Core
