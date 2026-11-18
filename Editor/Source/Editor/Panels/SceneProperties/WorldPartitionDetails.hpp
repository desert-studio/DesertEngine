#pragma once

// DETAILS > WORLD PARTITION - UE's actor Details "World Partition" category (AActor::bIsSpatiallyLoaded,
// AActor::RuntimeGrid, AActor::HLODLayer, AActor::DataLayerAssets), drawn by the Details panel above the component
// list for every entity of a partitioned world. It edits the components that ARE those settings - the AlwaysLoaded
// marker, RuntimeGrid, HLODLayer and DataLayers (ECS/Components.hpp) - so there is one source of truth per value:
// the section is a view over them, never a copy. Every pick is one undo step (Commands::MutateEntityUndoable).
//
// It also answers where the entity lands: the cell of its composite (Rules::CellStem, the cell's file stem), or
// why it is always loaded - the planner's own answer (Rules::PlanWorldPartition over the scene's own serialised
// form, the loader's path), remade when the history moves, the selection changes or the world's partition changes.

#include <Engine/Core/Serialize/SceneFormat.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace Desert::ECS
{
    class Entity;
}

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Assets
{
    class AssetManager;
}

namespace Desert::Editor
{
    class WorldPartitionDetails
    {
    public:
        /// Draws the section for @p entity of @p scene; nothing in a world with no WorldPartition block.
        void Draw( ::Desert::ECS::Entity& entity, ::Desert::Core::Scene& scene, const std::shared_ptr<::Desert::Assets::AssetManager>& assets );

    private:
        // Where the entity's composite lands, as one line: "G0_L0_3_-2 - grid Main, level 0, cell 2500 cm", or the
        // always-loaded reason, or why the world could not be planned.
        [[nodiscard]] std::string PlanCellLine( const ::Desert::ECS::Entity& entity, ::Desert::Core::Scene& scene,
                                                const std::shared_ptr<::Desert::Assets::AssetManager>& assets ) const;

        // The cell line and what it was made from.
        std::string                                   m_CellLine;
        std::uint64_t                                 m_CellEntity   = 0;
        std::uint64_t                                 m_CellRevision = 0;
        const ::Desert::Core::Scene*                            m_CellScene    = nullptr;
        std::optional<::Desert::Core::WorldPartitionSerialized> m_CellPartition;
        bool                                          m_CellAlwaysLoaded = false;
        bool                                          m_CellValid        = false;
    };
} // namespace Desert::Editor
