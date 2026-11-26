#pragma once

// The scene's water bodies as the physics step sees them (UE: UWaterSubsystem's FSolverSafeWaterBodyData,
// rebuilt from the components): every WaterBodyComponent with its entity's position and the waves the world's
// record (WaterWaveSources) gives it - the very lane the renderer draws it with. A body the record refuses is
// left out; a lane still pending is a body whose surface is not ready. A river is its bake in the world's
// WaterRiverBakes (never baked here): no bake, no river. A river that ends in an ocean (Water::RiverMouth) carries
// that ocean's lane; every fall's plunge (Water::FallPlunge) is handed to the body it lands in.
//
// RETAINED (UE: FWaterBodyManager registers a body on add/remove, the per-frame path reads the list): which
// entities are water bodies is kept between gathers and walked again only when a WaterBodyComponent was
// constructed, patched or destroyed, or gained or lost its TransformComponent (WaterBodyRevision in the registry's
// context, raised by their signals), or the registry is another one. The body's VALUES are read from its
// components every gather - its transform and its fields are edited in place (gizmo, inspector) without a signal -
// into storage the gather keeps, so a steady frame allocates nothing.

#include <Water/Public/WaterBodyQuery.hpp>

#include <Common/Core/Entt.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Desert::ECS
{
    class WaterWaveSources; // Water/Public/WaterWaveSources.hpp

    class WaterBodyGather final
    {
    public:
        /// Every water body of the registry, in entity order. The span is the gather's own storage: valid until the
        /// next Gather or Reset.
        [[nodiscard]] std::span<const Water::WaterBodyState> Gather( entt::registry&         registry,
                                                                     const WaterWaveSources& sources );

        /// Forgets the retained bodies.
        void Reset();

    private:
        const entt::registry*              m_BuiltFor      = nullptr;
        uint64_t                           m_BuiltRevision = 0; // WaterBodyRevision m_Members was built at
        std::vector<entt::entity>          m_Members;           // the registry's water bodies, in view order
        std::vector<Water::WaterBodyState> m_Bodies;            // this gather's bodies, storage reused
    };

    /// Raised by every WaterBodyComponent construct / patch / destroy (WaterBodyGather's membership key).
    struct WaterBodyRevision
    {
        uint64_t Value = 1;
    };

    /// The registry's membership key; the first ask connects the signals that raise it.
    [[nodiscard]] uint64_t WaterBodyRevisionOf( entt::registry& registry );
} // namespace Desert::ECS
