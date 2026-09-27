#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/World/Landscape/LandscapeGenerator.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::Commands
{
    /// Field-by-field equality of two target-layer lists (the panel's "did this frame change anything").
    bool SameLandscapeLayers( const std::vector<ECS::LandscapeLayerInfo>& a,
                              const std::vector<ECS::LandscapeLayerInfo>& b );

    /**
     * @brief Records an edit of the landscape root's target layers as one undo entry: @p after is already in the
     * component, @p before is what undo puts back. Addressed by the root's UUID, so the entry survives the entt
     * pool moving; the root is NOT re-created, unlike MutateEntityUndoable, whose subtree snapshot would re-create
     * every tile of the landscape for a one-float edit.
     */
    void RecordLandscapeLayersEdit( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                    const Common::UUID& landscape, std::vector<ECS::LandscapeLayerInfo> before,
                                    std::vector<ECS::LandscapeLayerInfo> after, std::string label );

    /// UE's "+" under Target Layers: appends "Layer N" (the first free N) to the scene's first landscape,
    /// undoably. Returns the new layer's name; refuses a scene without a loaded landscape root.
    Common::ResultStr<std::string> AddLandscapeLayer( const std::shared_ptr<::Desert::Core::Scene>& scene );

    /**
     * @brief UE's New Landscape "Create": generates the heights (World/Landscape/LandscapeGenerator.hpp), then adds a
     * root entity and one tile entity per tile, as ONE undo step. The tiles' heights live in memory until the scene
     * is saved (their files are written beside the scene then), so the undo entry keeps the generated tiles and
     * redo re-creates the same UUIDs WITH them — a snapshot restore would come back without terrain, since a tile
     * block without a file has none. Returns the root's UUID, or the generator's refusal.
     */
    Common::ResultStr<Common::UUID> CreateLandscape( const std::shared_ptr<::Desert::Core::Scene>&     scene,
                                                     const World::Landscape::LandscapeGenerateSettings& settings );
} // namespace Desert::Editor::Commands
