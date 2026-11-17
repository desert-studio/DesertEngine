#pragma once

// THE EDITOR'S DATA LAYER STATE AND VERBS (WP-DATA-LAYER-OUTLINER) - UE UDataLayerEditorSubsystem
// (WorldPartition/DataLayer/DataLayerEditorSubsystem.h), the pattern and not the letter.
//
// What the world states about its layers - name, parent, type, initial runtime state, colour, initially loaded and
// initially visible - is the world's: WorldPartitionSerialized::DataLayers, edited through EditWorldPartition (one
// undo step, refused by Rules::CheckDataLayers). What one user does with them in one session is here, per world,
// and is never written:
//   - VISIBLE (UE UDataLayerInstance::bIsVisible, transient; it starts at the world's InitiallyVisible). A hidden
//     layer's members stay loaded and in the Outliner and are not drawn: ECS::HiddenByDataLayersComponent (UE
//     AActor::bHiddenEdLayer). A member is drawn while ANY of its layers is effectively visible (UE
//     UpdateActorVisibility), and a layer is effectively visible while it and every ancestor is
//     (IsEffectiveVisible). Loading is a different switch: the World Partition panel's Loaded
//     (EditorRegions::DataLayerOverrides).
//   - CURRENT (UE AddToActorEditorContext / RemoveFromActorEditorContext): the layers every entity the user
//   creates
//     joins - added, placed, pasted or duplicated - through Commands::SetCreatedEntitiesContext, in the creation's
//     own undo step.
// Membership (UE AddActorsToDataLayers / RemoveActorsFromDataLayers) is the entities' own DataLayers component,
// edited as ONE undo step and a whole composite at a time: a composite belongs to its anchor's layers
// (Rules::PlanWorldPartition names a member that states other layers), so a verb on any entity of a hierarchy
// applies to its root and every descendant.
//
// The per-world state is keyed by the world (a weak reference), so it dies with the scene it describes, as UE's
// transient per-world editor state does.

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor::DataLayerEditor
{
    // ── visibility (UE IsVisible / SetDataLayerVisibility) ─────────────────────────────────────────────────
    /// This session's own Visible of @p layer (the world's InitiallyVisible until toggled). A name the world does
    /// not state hides nothing: true.
    [[nodiscard]] bool IsVisible( const ::Desert::Core::Scene& scene, const std::string& layer );
    /// UE IsEffectiveVisible for every layer of the world, index = WorldPartitionSerialized::DataLayers.
    [[nodiscard]] std::vector<bool> EffectiveVisibility( const ::Desert::Core::Scene& scene );
    /// Shows or hides @p layer for this session; refused by name for a layer the world does not state. The
    /// members' drawing follows on the next UpdateEntityVisibility. Answers whether anything changed.
    [[nodiscard]] Common::BoolResultStr SetVisible( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                    const std::string& layer, bool visible );
    /// Stamps or lifts ECS::HiddenByDataLayersComponent on every entity whose state differs from what its layers
    /// say now - all of them lifted while the scene plays (the game draws by runtime state only). An entity that
    /// becomes hidden leaves the selection (UE deselects an actor its layers hide). Returns how many entities
    /// changed.
    std::size_t UpdateEntityVisibility( ::Desert::Core::Scene& scene );

    // ── the current layers (UE's actor editor context) ────────────────────────────────────────────────────
    [[nodiscard]] bool IsCurrent( const ::Desert::Core::Scene& scene, const std::string& layer );
    /// The current layers the world still states, in the world's order.
    [[nodiscard]] std::vector<std::string> CurrentLayers( const ::Desert::Core::Scene& scene );
    /// UE Make Current / Remove from current; refused by name for a layer the world does not state.
    [[nodiscard]] Common::BoolResultStr SetCurrent( const std::shared_ptr<::Desert::Core::Scene>& scene,
                                                    const std::string& layer, bool current );
    /// What Commands::SetCreatedEntitiesContext runs on the roots of a creation: a root that hangs off nothing
    /// joins the current layers with its whole subtree; a root created under a parent joins its composite's layers
    /// - the anchor's - so the composite stays one set of layers.
    void ApplyCurrentLayers( ::Desert::Core::Scene& scene, const std::vector<Common::UUID>& roots );

    // ── membership (UE AddActorsToDataLayers / RemoveActorsFromDataLayers / SelectActorsInDataLayer) ─────────
    /// The composites of @p entities (each one's root and every descendant) join @p layer, as ONE undo step.
    /// Answers how many entities changed; refused by name for an unknown layer or no entity.
    [[nodiscard]] Common::ResultStr<std::size_t> AddEntities( ::Desert::Core::Scene&           scene,
                                                              const std::vector<Common::UUID>& entities,
                                                              const std::string&               layer );
    [[nodiscard]] Common::ResultStr<std::size_t> RemoveEntities( ::Desert::Core::Scene&           scene,
                                                                 const std::vector<Common::UUID>& entities,
                                                                 const std::string&               layer );
    /// The loaded entities that state @p layer itself (not its children's), in registry order: the Outliner rows.
    [[nodiscard]] std::vector<Common::UUID> DirectMembers( const ::Desert::Core::Scene& scene,
                                                           const std::string&           layer );
    /// UE Select Actors: the selection becomes every loaded member of @p layer and of every layer nested under it
    /// (a member of a child is a member of its parent). Answers how many were selected.
    [[nodiscard]] Common::ResultStr<std::size_t> SelectMembers( ::Desert::Core::Scene& scene,
                                                                const std::string&     layer );
} // namespace Desert::Editor::DataLayerEditor
