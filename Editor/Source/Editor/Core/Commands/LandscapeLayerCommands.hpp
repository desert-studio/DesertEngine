#pragma once

#include <Engine/ECS/Components.hpp>
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
} // namespace Desert::Editor::Commands
