#pragma once

#include <entt/entt.hpp>

#include <atomic>
#include <cstdint>

namespace Desert::ECS
{
    // THE HIERARCHY REVISION (UE: the attachment change a USceneComponent::AttachToComponent broadcasts). A stamp
    // the owning Scene bumps on every change of the parent/child graph: Scene::Attach / Scene::Detach (they edit
    // RelationshipComponent in place) and the RelationshipComponent construct / update / destroy signals (an
    // entity created, loaded or destroyed). A reader that derives something from the hierarchy (the selection
    // outline's subtree, SelectionOutlineMask) keeps it until the stamp moves, instead of walking the graph every
    // frame. Atomic: the collectors read it on a worker while the game thread attaches.
    struct HierarchyRevision
    {
        std::atomic<uint64_t> Value{ 0 };

        void Bump() noexcept
        {
            Value.fetch_add( 1, std::memory_order_acq_rel );
        }
    };

    // The registry context's link to its Scene's revision (registry.ctx holds a pointer, as RenderStateDeltaLink
    // does: the Scene owns the stamp and outlives its registry's systems).
    struct HierarchyRevisionLink
    {
        const HierarchyRevision* Revision = nullptr;
    };

    // The hierarchy stamp of @p registry, or nothing when no Scene feeds one (a bare registry): a reader that gets
    // nothing trusts no cache of the hierarchy and derives it again.
    [[nodiscard]] inline bool ReadHierarchyRevision( const entt::registry& registry, uint64_t& out ) noexcept
    {
        const auto* link = registry.try_ctx<HierarchyRevisionLink>();
        if ( link == nullptr || link->Revision == nullptr )
            return false;
        out = link->Revision->Value.load( std::memory_order_acquire );
        return true;
    }
} // namespace Desert::ECS
