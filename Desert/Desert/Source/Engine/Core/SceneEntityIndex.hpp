#pragma once

// THE SCENE'S LIST OF ENTITIES, AND THE ONE WAY AN ENTITY LEAVES IT.
//
// Why this is its own unit and not three members of Scene: WorldPartition streaming unloads a cell by
// destroying every entity in it, and until WP6 `Scene::DestroyEntity` cost O(N) in the size of the WORLD
// — a linear search of the entity vector, a `vector::erase` from the middle, and then a pass over the
// whole UUID map to shift every stored index past the hole. Unloading a cell of k entities from a world of
// N therefore cost O(k * N). The destroy of a parent was worse still: every child it took down first
// erased ITSELF from the Children vector of the parent that was about to die anyway, at the front, so a
// folder of k children cost O(k^2) before a single entity had left the registry.
//
// Both are fixed here, and here rather than in Scene.cpp because Scene drags the renderer in with it,
// which would leave the algorithm that streaming leans on hardest reachable by no suite at all
// (Desert/Tests/Engine/EntityDestroy compiles this unit directly and times it).
//
// ORDER. Removal is swap-with-last: the entity that was last takes the vacated slot. That is the whole
// O(1), and the price is that `All()` is no longer creation order after a destroy. Every reader was
// checked (WP6 report): the outliner never read this list (it walks `registry.view<UUIDComponent>`, which
// EnTT already swap-and-pops), the loader rebuilds hierarchy from explicit Parent slots rather than from
// position, and picking breaks exact-distance ties by the first candidate, which no scene authors on
// purpose. The one visible consequence is that a .desce saved after a delete lists the formerly-last
// record where the deleted one was — a moved block in a text diff, not a change in the loaded scene.

#include <Common/Core/UUID.hpp>
#include <Engine/ECS/Entity.hpp>

#include <entt/entt.hpp>

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace Desert::Core
{
    class SceneEntityIndex final
    {
    public:
        // Appends and answers a reference into the list. The reference is invalidated by the next Add,
        // exactly as the `emplace_back` it replaces was — callers already copy the Entity (it is a handle
        // and a registry pointer) rather than hold on to it.
        ECS::Entity& Add( const Common::UUID& uuid, entt::entity handle, entt::registry& registry );

        // O(1): swap-with-last. Answers false for a handle this index never held, so a caller cannot
        // mistake "nothing was there" for "removed".
        bool Remove( entt::entity handle );

        // The entity registered under @p uuid most recently, or null.
        [[nodiscard]] const ECS::Entity* Find( const Common::UUID& uuid ) const;

        [[nodiscard]] const std::vector<ECS::Entity>& All() const
        {
            return m_Entities;
        }

        [[nodiscard]] std::size_t Size() const
        {
            return m_Entities.size();
        }

        void Clear();

    private:
        // Parallel to m_Entities: the UUID each slot was added under, so a swap can re-point the moved
        // entity's lookup without asking the registry for its UUIDComponent.
        std::vector<ECS::Entity>  m_Entities;
        std::vector<Common::UUID> m_Ids;

        // Keyed by HANDLE, not by UUID. A handle is unique among live entities by construction; a UUID is
        // unique only if every caller of CreateEntityWithUUID was careful, and the index it replaced — a
        // UUID -> slot map — lost track of the older of two entities sharing one, leaving a slot nothing
        // could ever remove.
        std::unordered_map<entt::entity, std::size_t> m_SlotOf;
        std::unordered_map<Common::UUID, entt::entity> m_ByUuid;
    };

    // Destroys @p root and its whole subtree: registry, index and the parent's Children list. The cost is
    // O(subtree + siblings of root) — the root unlinks itself from its parent ONCE, and nothing below it
    // does, because every node below it has a parent that is about to be destroyed as well.
    //
    // Runtime resources a component holds outside the registry (Jolt bodies, script environments, …) are
    // released by the owning system's `on_destroy<Component>` listener, which `registry.destroy` fires —
    // see Desert/Tests/Engine/EntityDestroy for the register of which component is released by whom.
    void DestroyEntityTree( entt::registry& registry, SceneEntityIndex& index, entt::entity root );
} // namespace Desert::Core
