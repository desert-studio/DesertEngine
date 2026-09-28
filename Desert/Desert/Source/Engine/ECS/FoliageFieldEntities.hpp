#pragma once

#include <Engine/ECS/Components.hpp>

#include <entt/entt.hpp>

namespace Desert::ECS
{
    // ── A FOLIAGE FIELD'S PREFAB INSTANCES ARE DERIVED STATE, NOT AUTHORED ENTITIES (FO-9) ───────────────
    //
    // A field (FoliageComponent) realizes one prefab instance per placed transform as its CHILDREN, every
    // frame, from the transforms it stores (RealizePrefabFoliage); the scene saver skips them for the same
    // reason. UE hides the actors its foliage spawns from the Scene Outliner (FFoliageActor::Spawn sets
    // bHideFromSceneOutliner) and a viewport click on foliage selects the foliage actor that owns it.
    // These two predicates are that rule, over a registry, so the outliner and the picker cannot disagree.

    // The outliner lists the field and none of its descendants.
    inline bool HidesChildrenFromOutliner( const entt::registry& registry, entt::entity entity )
    {
        return entity != entt::null && registry.has<FoliageComponent>( entity );
    }

    // The field @p entity is a realized part of (the nearest FoliageComponent ANCESTOR), or null when it
    // is not inside a field. The field itself is not its own owner.
    inline entt::entity OwningFoliageField( const entt::registry& registry, entt::entity entity )
    {
        entt::entity current = entity;
        while ( current != entt::null && registry.has<RelationshipComponent>( current ) )
        {
            current = registry.get<RelationshipComponent>( current ).Parent;
            if ( current != entt::null && registry.has<FoliageComponent>( current ) )
                return current;
        }
        return entt::null;
    }
} // namespace Desert::ECS
