#pragma once

// The scene's water bodies as the physics step sees them (UE: UWaterSubsystem's FSolverSafeWaterBodyData,
// rebuilt from the components): every WaterBodyComponent with its entity's position and its wave set's waves.
//
// The lookup is a parameter, as DestructibleLifetime's: the engine passes WaterWavesService and a suite hands
// it waves built in memory. A wave set that cannot be read refuses its body by name, once per entity per Play;
// a set still being read leaves the body out of THIS gather only, and the next step's gather takes it in.
//
// RETAINED (UE: FWaterBodyManager registers a body on add/remove, the per-frame path reads the list): which
// entities are water bodies is kept between gathers and walked again only when a WaterBodyComponent was
// constructed, patched or destroyed (WaterBodyRevision in the registry's context, raised by its signals) or the
// registry is another one. The body's VALUES are read from its components every gather - its transform and its
// fields are edited in place (gizmo, inspector) without a signal - into storage the gather keeps, so a steady
// frame allocates nothing.

#include <Engine/Assets/Common.hpp>
#include <Water/Public/WaterBodyQuery.hpp>

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>

#include <functional>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::ECS
{
    class WaterBodyGather final
    {
    public:
        using WavesLookup =
             std::function<Common::ResultStr<std::shared_ptr<const std::vector<Water::GerstnerWave>>>(
                  const Assets::AssetHandle& )>;
        /// The newest CPU mirror of the FFT ocean a wave set names (Runtime::WaterWavesService::Surfaces), null
        /// for a Gerstner set or before the first read-back (the body then answers with its Gerstner waves).
        using SurfaceLookup =
             std::function<std::shared_ptr<const Water::FFTSurfaceSnapshot>( const Assets::AssetHandle& )>;

        /// Every water body of the registry that is ready, in entity order. The span is the gather's own
        /// storage: valid until the next Gather or Reset.
        [[nodiscard]] std::span<const Water::WaterBodyState> Gather( entt::registry&    registry,
                                                                     const WavesLookup& lookup,
                                                                     const SurfaceLookup& surfaces = {} );

        /// Forgets the refusals and the retained bodies: a new Play reports them again.
        void Reset();

    private:
        const entt::registry*              m_BuiltFor      = nullptr;
        uint64_t                           m_BuiltRevision = 0; // WaterBodyRevision m_Members was built at
        std::vector<entt::entity>          m_Members;           // the registry's water bodies, in view order
        std::vector<Water::WaterBodyState> m_Bodies;            // this gather's ready bodies, storage reused
        std::unordered_set<entt::entity>   m_Refused;
        // Each river's bake and the RiverBakeKey it was baked for: re-baked only when its spline, width, flow or
        // location changed (a transform move raises no WaterBodyRevision, so the key - not the revision - decides).
        struct RiverBake
        {
            uint64_t                                     Key = 0;
            std::shared_ptr<const Water::RiverFlowField> Flow;
        };
        std::unordered_map<entt::entity, RiverBake> m_Rivers;
    };

    /// Raised by every WaterBodyComponent construct / patch / destroy (WaterBodyGather's membership key).
    struct WaterBodyRevision
    {
        uint64_t Value = 1;
    };
} // namespace Desert::ECS
