#pragma once

#include <Common/Core/DevInstruments.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/GameplayDebugger/GameplayDebuggerCategory.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace Desert::Debug
{
    /// What World Partition streams around the player (UE UWorldPartitionSubsystem::Draw and its switches
    /// wp.Runtime.ToggleDrawRuntimeHash2D / 3D, ToggleDrawStreamingSources, ToggleDrawRuntimeCellsDetails,
    /// ShowRuntimeSpatialHashGridLevel), as a gameplay debugger category, so a packaged game and the editor's Play
    /// show it by the same code:
    ///   2D  - a map of the runtime hash in the view's top-right corner: every cell around the first streaming
    ///         source as a square, the sources' loading circles and sectors, the sources as dots with their facing.
    ///   3D  - every cell that is not Unloaded as a box over its square, as high as its content (the cell's
    ///         Height, UE's content bounds in Z).
    ///   Sources - each source's shapes per grid: a sphere of its loading range, a sector's wedge on the ground.
    ///   Details - the cells under each source: name, state, records, read and activation time, data layers.
    /// Cells are coloured by streaming state (UE ULevelStreaming's status colours), by their grid's DebugColor or by
    /// their first runtime data layer's DebugColor. Everything is read from the world's WorldStreamer (GameplayDebugger
    /// ::GetWorldStreamer); a world that is not streamed says so.
    class GameplayDebuggerCategoryWorldPartition final : public IGameplayDebuggerCategory
    {
    public:
        enum class ColorMode : std::uint8_t
        {
            State,
            Grid,
            DataLayer
        };

        /// The switches (UE's wp.Runtime console toggles): Shift+M, Shift+B, Shift+S, Shift+D, Shift+G, Shift+C
        /// while the category is on, or a host's command (the editor's palette).
        struct View
        {
            bool               Hash2D      = false;
            bool               Hash3D      = true;
            bool               Sources     = true;
            bool               CellDetails = true;
            std::optional<int> GridLevel; // nullopt = every level (UE ShowRuntimeSpatialHashGridLevel)
            ColorMode          Coloring = ColorMode::State;
        };

        [[nodiscard]] std::string_view GetName() const override
        {
            return "World Partition";
        }
        [[nodiscard]] bool IsEnabledByDefault() const override
        {
            return false;
        }

        void ReadInput( const Input::LocalPlayerInput& input ) override;
        void CollectData( const Core::Scene& scene, const ECS::Entity* target,
                          GameplayDebuggerCategoryData& out ) override;

        [[nodiscard]] View& GetView()
        {
            return m_View;
        }
        /// All levels -> 0 -> 1 -> ... -> the world's last level -> all levels.
        void NextGridLevel();
        void NextColorMode();

    private:
        View m_View;
        int  m_LevelCount = 0; // the streamed world's, from the last collection
    };
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS
