#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/World/Landscape/LandscapeEditLayers.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <entt/entt.hpp>

#include <span>
#include <vector>

// The scene-free half of the Edit Layers commands (LandscapeLayerCommands.hpp): what one edit does to a stack
// and to the tiles of one landscape in a registry. The undo entry and the editor's editing layer are the
// commands'; this half needs no Scene, no undo stack and no renderer, so a suite checks it on a bare registry.
namespace Desert::Editor::Commands
{
    /// One removed layer's data on one tile: what the removal's undo puts back.
    struct LandscapeRemovedTileLayer
    {
        int32_t                                      TileX = 0;
        int32_t                                      TileZ = 0;
        World::Landscape::LandscapeEditLayerTileData Data;
    };

    /// Every loaded tile entity of @p landscape. With @p refuseUnloaded an unloaded tile is refused naming it —
    /// its stored samples would keep the old merge; without it (an edit that changes no sample) it is skipped.
    Common::ResultStr<std::vector<entt::entity>>
    LandscapeLoadedTiles( entt::registry& registry, const Common::UUID& landscape, bool refuseUnloaded );

    /// @p layer's index in @p stack; refuses a Guid the stack does not name.
    Common::ResultStr<size_t> LandscapeEditLayerIndex( const World::Landscape::LandscapeEditLayerStack& stack,
                                                       const Common::UUID&                              layer );

    /// @p stack with a new empty layer @p guid, named "Layer N" (the first N from the stack's size + 1 that no
    /// layer uses), directly above @p editing (the bottom layer when @p editing is null or not in the stack).
    World::Landscape::LandscapeEditLayerStack
    LandscapeStackWithLayerAdded( const World::Landscape::LandscapeEditLayerStack& stack,
                                  const Common::UUID& editing, const Common::UUID& guid );

    /// @p stack without @p layer. Refuses an unknown layer and the last one: a landscape has at least one.
    Common::ResultStr<World::Landscape::LandscapeEditLayerStack>
    LandscapeStackWithLayerRemoved( const World::Landscape::LandscapeEditLayerStack& stack,
                                    const Common::UUID&                              layer );

    /// What @p layer holds on every loaded tile of @p landscape (refusing an unloaded tile).
    Common::ResultStr<std::vector<LandscapeRemovedTileLayer>>
    LandscapeEditLayerTileDataOf( entt::registry& registry, const Common::UUID& landscape,
                                  const Common::UUID& layer );

    /**
     * @brief Puts @p stack on @p landscape's root, drops from every loaded tile the data of a layer @p stack
     * does not name, puts @p restore back on its tiles and, with @p merge, re-merges every tile whole with
     * @p rules (World::Landscape::MergeLandscapeEditLayers). With @p merge an unloaded tile is refused before
     * anything is written. Refuses an invalid stack, naming it.
     */
    Common::BoolResultStr
    ApplyLandscapeEditLayerStack( entt::registry& registry, const Common::UUID& landscape,
                                  const World::Landscape::LandscapeEditLayerStack&      stack,
                                  std::span<const World::Landscape::LandscapeLayerRule> rules,
                                  std::span<const LandscapeRemovedTileLayer> restore, bool merge );
} // namespace Desert::Editor::Commands
