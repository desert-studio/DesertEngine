#pragma once

#include <entt/entt.hpp>
#include <Engine/ECS/LockComponent.hpp>
#include <Engine/ECS/RelationshipComponent.hpp>

namespace Desert::ECS
{
    // ── THE AUTHORING LOCK, AS A PURE FUNCTION OF A REGISTRY ─────────────────────────────────────────
    //
    // The question "may I move this?" is asked from three unrelated places — the viewport's picker, the
    // gizmo, and the outliner's row — and every one of them would otherwise have spelled it itself.
    // Three spellings of one predicate is how a lock ends up blocking two of the three things it claims
    // to block, which is worse than no lock: the outliner would still draw the closed padlock.
    //
    // So it is one inline predicate over the registry, with no Scene, no camera and no device in the
    // way, which also makes the whole rule reachable by a unit test.

    // Presence of LockComponent IS the locked state (see the note on the component).
    inline bool IsLocked( const entt::registry& registry, entt::entity entity )
    {
        return entity != entt::null && registry.has<LockComponent>( entity );
    }

    // Locks or unlocks the entity AND its entire subtree.
    //
    // WHY RECURSIVE, when UE's actor lock is per-actor: this engine's hierarchy is real and its
    // neighbouring authoring flag is recursive already (Scene::SetVisibleRecursive). Locking a prop
    // whose meshes are children, and then still being able to grab one of those meshes, is the lock
    // failing at the one job anyone would ask it to do. Stamping the subtree rather than walking
    // ancestors on every query keeps the predicate above O(1) — the picker runs it per click and the
    // outliner per row per frame.
    inline void SetLockedRecursive( entt::registry& registry, entt::entity entity, bool locked )
    {
        if ( entity == entt::null )
            return;

        if ( locked )
        {
            if ( !registry.has<LockComponent>( entity ) )
                registry.emplace<LockComponent>( entity );
        }
        else if ( registry.has<LockComponent>( entity ) )
        {
            registry.remove<LockComponent>( entity );
        }

        if ( registry.has<RelationshipComponent>( entity ) )
        {
            // Copied, not referenced: emplace/remove above can reallocate the pool the relationship
            // component lives beside, and a reference into it would dangle mid-recursion.
            const auto children = registry.get<RelationshipComponent>( entity ).Children;
            for ( const auto child : children )
                SetLockedRecursive( registry, child, locked );
        }
    }
} // namespace Desert::ECS
