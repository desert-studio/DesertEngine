#pragma once

#include <Engine/ECS/Components.hpp>

#include <entt/entt.hpp>

namespace Desert::ECS
{
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
    inline bool IsHidden( const entt::registry& registry, entt::entity entity )
    {
        if ( entity == entt::null || !registry.valid( entity ) )
            return false;

        return registry.has<VisibilityComponent>( entity ) && !registry.get<VisibilityComponent>( entity ).Visible;
    }
} // namespace Desert::ECS
