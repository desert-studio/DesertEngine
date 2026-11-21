#pragma once

#include <Engine/ECS/Components.hpp>

#include <entt/entt.hpp>

namespace Desert::ECS
{
    // AN ENTITY OF A WORLD PARTITION CELL THAT IS NOT WHOLE YET (WP-ACTIVATE-SLICED). A cell is made a slice at a
    // time over several frames and drawn only once every record of it is an entity and its collision is ready —
    // UE's ULevel::bIsVisible, false between the first and the last step of UWorld::AddToWorld. The world
    // streamer adds this to every entity a slice makes and removes it when the cell is revealed. Runtime only:
    // no ComponentRegistry key, so it is never saved, and the Play snapshot Stop rebuilds from never holds it.
    // Kept apart from VisibilityComponent because that is AUTHORED: hiding through it would overwrite what the
    // author set, and the reveal could not tell which entities to show again.
    struct StreamingPendingComponent
    {
    };

    // AN ENTITY STREAMED OUT, DESTROYED AT THE NEXT VIEW FENCE (WP-OPT-UNLOAD; UE: a streamed-out level's
    // components are unregistered and its primitives leave the FScene through the render command queue - the
    // game thread never waits for the rendering thread to stream out). Scene::DestroyEntityAtFence stamps it on
    // the whole subtree so this update's recording no longer draws the entity, and destroys the tree once the
    // views that may still replay its draws have finished. Runtime only, never saved.
    struct StreamingOutComponent
    {
    };

    // ── "DO NOT DRAW ME", AS ONE PURE FUNCTION OF A REGISTRY ─────────────────────────────────────────
    //
    // The outliner's eye and the Details "Visible" tick both write one bool, VisibilityComponent::Visible,
    // onto ANY entity — a mesh, a sun, a Sky Atmosphere, a fog volume. Reading it back was open-coded in
    // exactly three collectors (mesh, terrain, text) and nowhere else, so unticking Visible on the sun or
    // on the sky changed nothing at all: the collectors that build the sky, the fog, the clouds and the
    // lights never asked. That is the defect this predicate exists to make impossible to repeat.
    //
    // WHY A PREDICATE AND NOT THREE `has<>`/`get<>` PAIRS AT EACH SITE: the sites are spread over eight
    // files owned by different passes, and the only reason the first three agreed on the spelling is that
    // they were copied. A copied spelling is how a flag ends up honoured by two of the three things it
    // claims to control. It also makes the rule reachable from a unit test without a Scene or a device,
    // the same split Engine/ECS/EntityLock.hpp uses for the authoring lock.
    //
    // O(1), NO ANCESTOR WALK: Scene::SetVisibleRecursive stamps the whole subtree, so a hidden parent has
    // already written `false` onto every descendant. Walking parents here would be a second, disagreeing
    // definition of the same state — and the collectors call this once per entity per frame.
    //
    // ABSENCE MEANS VISIBLE: most entities never get the component, and treating "no component" as hidden
    // would black out every scene ever authored.
    //
    // A cell still being made is hidden as a whole (StreamingPendingComponent above), whatever its entities'
    // own Visible says; it is one more bit on the entity, so this stays O(1).
    inline bool IsHidden( const entt::registry& registry, entt::entity entity )
    {
        if ( entity == entt::null || !registry.valid( entity ) )
            return false;
        if ( registry.has<StreamingPendingComponent>( entity ) || registry.has<StreamingOutComponent>( entity ) )
            return true;

        // Its data layers are hidden in the editor (UE bHiddenEdLayer): stamped on the entity itself, so no walk.
        if ( registry.has<HiddenByDataLayersComponent>( entity ) )
            return true;
        return registry.has<VisibilityComponent>( entity ) && !registry.get<VisibilityComponent>( entity ).IsVisible();
    }
} // namespace Desert::ECS
