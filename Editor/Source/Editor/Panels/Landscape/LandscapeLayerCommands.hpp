#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/World/Landscape/LandscapeGenerator.hpp>
#include <Engine/World/Landscape/LandscapeSplines.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <filesystem>
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
    /**
     * @brief UE's Import (Manage mode) into the scene's first landscape: replaces the heights of the WHOLE
     * landscape with the file's (16-bit .png, .r16 / .raw) as ONE undo step. The file must be the landscape's size
     * — (tiles · quads + 1) samples a side — or it is refused naming both sizes; nothing is written on a refusal.
     */
    Common::BoolResultStr ImportLandscapeHeightmap( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                    const std::filesystem::path&                  path );

    /**
     * @brief UE's New Landscape → Import from File: a new landscape whose tile grid is the file's size, with
     * @p settings' location, tile size, spacing and Z scale, added as ONE undo step (the same entry Create
     * pushes). Refuses while a Create is running, and a file whose sides are not a multiple of the tile's quads
     * plus one (naming the nearest valid sizes). Returns the new root's UUID.
     */
    Common::ResultStr<Common::UUID>
    ImportLandscapeHeightmapAsNew( const std::shared_ptr<::Desert::Core::Scene>&      scene,
                                   const std::filesystem::path&                       path,
                                   const World::Landscape::LandscapeGenerateSettings& settings );

    /// UE's Export: the scene's first landscape's heights to @p path (format by extension), the whole landscape
    /// or, with @p selectedTiles, the bounding rectangle of the selected tiles of it (refused when none is
    /// selected).
    Common::BoolResultStr ExportLandscapeHeightmap( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                    const std::filesystem::path& path, bool selectedTiles );

    // ── Edit Layers (UE: Landscape Mode → Edit Layers) ──────────────────────────────────────────────────────
    //
    // Each call edits the scene's first landscape's stack (LandscapeComponent::EditLayers) as ONE undo entry.
    // A change the merge reads — visibility, an alpha, removing a layer — re-merges every tile of the landscape
    // (World::Landscape::MergeLandscapeEditLayers), in the call and in its undo and redo; so it is refused while
    // a tile of the landscape is unloaded, whose stored samples would keep the old merge. @p layer names a layer
    // by its Guid; an unknown Guid is refused naming it.

    /// UE's "Create Layer": a new empty layer "Layer N" directly above the editing layer
    /// (LandscapeSculptState::EditingLayer; the bottom one when none is picked); it becomes the editing layer.
    /// Returns its Guid. An empty layer changes no sample, so nothing is merged.
    Common::ResultStr<Common::UUID> AddLandscapeEditLayer( const std::shared_ptr<::Desert::Core::Scene>& scene );

    /// UE's "Delete Layer": the layer leaves the stack and every tile, and the landscape is re-merged; undo puts
    /// its data back on the same tiles. The last layer is refused — a landscape has at least one. When it was
    /// the editing layer, the editing layer goes back to the bottom one.
    Common::BoolResultStr RemoveLandscapeEditLayer( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                    const Common::UUID&                           layer );

    /// UE's "Rename"; refuses an empty name. Changes no sample.
    Common::BoolResultStr RenameLandscapeEditLayer( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                    const Common::UUID& layer, std::string name );

    /// UE's eye: an invisible layer contributes nothing to the merge.
    Common::BoolResultStr SetLandscapeEditLayerVisible( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                        const Common::UUID& layer, bool visible );

    /// UE's lock: brushes refuse a locked layer (World::Landscape::CheckLandscapeEditLayerTarget). Changes no
    /// sample.
    Common::BoolResultStr SetLandscapeEditLayerLocked( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                       const Common::UUID& layer, bool locked );

    /// UE's Heightmap / Weightmap alpha (-1..1 and 0..1, refused outside naming the value).
    Common::BoolResultStr SetLandscapeEditLayerAlpha( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                      const Common::UUID& layer, float heightAlpha,
                                                      float weightAlpha );

    /**
     * @brief UE's Splines tool Ctrl+click: adds to landscape @p landscape a control point at @p location (world cm) with @p point's
     * width and falloffs, joined by a segment with @p segment's Raise/Lower to control point @p joinTo when
     * given, and re-derives the Splines edit layer — creating it on top of the stack the first time, as UE
     * makes its splines layer. ONE undo entry (UE: one transaction per click); its undo restores the splines,
     * the stack and the layer's data, so the ground returns byte for byte. Returns the new point's index.
     */
    Common::ResultStr<uint32_t> AddLandscapeSplinePoint( ::Desert::Core::Scene& scene, const Common::UUID& landscape,
                                                         const glm::vec3& location, std::optional<uint32_t> joinTo,
                                                         const World::Landscape::LandscapeSplineControlPoint& point,
                                                         const World::Landscape::LandscapeSplineSegment& segment );

    /// UE: a control point dragged by the transform gizmo. Control point @p point of landscape @p landscape goes
    /// to @p location (world cm), its segments' curves follow, and the Splines layer is re-derived, so the old
    /// band leaves the ground. ONE undo entry for the whole drag (the caller commits on release).
    Common::BoolResultStr MoveLandscapeSplinePoint( ::Desert::Core::Scene& scene, const Common::UUID& landscape,
                                                    uint32_t point, const glm::vec3& location );

    /**
     * @brief UE: Delete with a control point selected. The point and its segments leave the splines and the
     * Splines layer is re-derived; ONE undo entry. Later points' indices drop by one. A point joining exactly
     * two segments is bridged by one segment between its neighbours (the first segment's Raise/Lower) so
     * deleting a middle point keeps the spline — UE drops both segments and leaves two pieces; bridging is the
     * owner's L9b spec ("a 2-point road").
     */
    Common::BoolResultStr RemoveLandscapeSplinePoint( ::Desert::Core::Scene& scene, const Common::UUID& landscape,
                                                      uint32_t point );

    /**
     * @brief UE: a segment's LayerName set in the Details panel. Every segment of landscape @p landscape's
     * splines paints @p layerName (empty: paints nothing) along its band, and the Splines layer is re-derived;
     * ONE undo entry. Refuses a name that is not one of the landscape's paint layers, naming it.
     */
    Common::BoolResultStr SetLandscapeSplineLayer( ::Desert::Core::Scene& scene, const Common::UUID& landscape,
                                                   const std::string& layerName );

    /**
     * @brief UE: a segment's SplineMeshes set in the Details panel. Every segment of landscape @p landscape's
     * splines lays @p meshes along itself (empty: none); ONE undo entry. The bent meshes are derived from the
     * splines by the renderer's side (ECS::LandscapeSplineMeshDraw), so a later point edit re-bends them.
     * Refuses a landscape with no segment and an entry ValidateLandscapeSplineMeshEntry refuses.
     */
    Common::BoolResultStr SetLandscapeSplineMeshes( ::Desert::Core::Scene& scene, const Common::UUID& landscape,
                                                    const std::vector<World::Landscape::LandscapeSplineMeshEntry>& meshes );

    /// The paint layer after the one the first segment paints, in the landscape's layer order, and "" after the
    /// last (and from ""  the first): what the palette's "Spline paint layer: next" sets.
    Common::ResultStr<std::string> NextLandscapeSplineLayer( entt::registry&     registry,
                                                             const Common::UUID& landscape );

    /// UE: the splines layer re-renders when a layer below it changed. After a brush wrote layer
    /// @p editedLayer, re-derives the Splines layer on every loaded tile when it sits ABOVE @p editedLayer;
    /// otherwise (no Splines layer, an edit above it) changes nothing. Not an undo entry: the brush's own
    /// entry calls it again on undo and redo, and the layer is a pure function of the ground below.
    Common::BoolResultStr ReapplyLandscapeSplinesBelow( entt::registry& registry, const Common::UUID& landscape,
                                                        const Common::UUID& editedLayer );
} // namespace Desert::Editor::Commands
