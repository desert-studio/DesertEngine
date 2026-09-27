#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/World/Landscape/LandscapeGenerator.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::Commands
{
    /**
     * @brief Records an edit of the landscape root's target layers as one undo entry: @p after is already in the
     * component, @p before is what undo puts back. Addressed by the root's UUID, so the entry survives the entt
     * pool moving; the root is NOT re-created, unlike MutateEntityUndoable, whose subtree snapshot would re-create
     * every tile of the landscape for a one-float edit.
     */
    void RecordLandscapeLayersEdit( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                    const Common::UUID& landscape, std::vector<Assets::AssetHandle> before,
                                    std::vector<Assets::AssetHandle> after, std::string label );

    /// UE's "Create Layer Info" under Target Layers: writes `Landscape/Layers/Layer N.delayerinfo` (the first
    /// free N, a distinct swatch), registers it, and appends it to the scene's first landscape, undoably.
    /// Returns the new layer's name; refuses a scene without a loaded landscape root.
    Common::ResultStr<std::string> AddLandscapeLayer( const std::shared_ptr<::Desert::Core::Scene>& scene );

    /// Points target layer @p slot at the layer info @p handle, undoably; refuses a handle already listed.
    Common::BoolResultStr AssignLandscapeLayer( const std::shared_ptr<::Desert::Core::Scene>& scene, size_t slot,
                                                const Assets::AssetHandle& handle );

    /// Removes target layer @p slot from the list, undoably. The tiles keep that layer's weights.
    Common::BoolResultStr RemoveLandscapeLayer( const std::shared_ptr<::Desert::Core::Scene>& scene, size_t slot );

    /**
     * @brief UE's New Landscape "Create", in the background: the heights are generated on the JobSystem
     * (World/Landscape/LandscapeGenerator.hpp's LandscapeGenerateJob) while the editor keeps drawing, and
     * FinishCreateLandscape applies them on the main thread. Refuses while a run is in flight (naming how far it
     * is), with no scene, or with settings the generator refuses.
     */
    Common::BoolResultStr StartCreateLandscape( const std::shared_ptr<::Desert::Core::Scene>&      scene,
                                                const World::Landscape::LandscapeGenerateSettings& settings );
    /// A run is in flight (started, not yet handed over).
    bool IsCreatingLandscape();
    /// The run's progress, 0..1; 0 when idle.
    float CreateLandscapeFraction();
    /// Asks the run to stop; FinishCreateLandscape then hands over World::Landscape::kLandscapeGenerateCancelled.
    void CancelCreateLandscape();
    /**
     * @brief Main thread, once a frame. When the run has finished, adds a root entity and one tile entity per tile
     * to the scene the run was started for, as ONE undo step, and returns the root's UUID; or the run's refusal
     * (a cancel among them). nullopt while idle or still running. The tiles' heights live in memory until the
     * scene is saved (their files are written beside the scene then), so the undo entry keeps the generated
     * tiles and redo re-creates the same UUIDs WITH them — a snapshot restore would come back without terrain,
     * since a tile block without a file has none.
     */
    std::optional<Common::ResultStr<Common::UUID>> FinishCreateLandscape();
} // namespace Desert::Editor::Commands
