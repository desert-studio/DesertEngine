#pragma once

// The scene's water bodies as the physics step sees them (UE: UWaterSubsystem's FSolverSafeWaterBodyData,
// rebuilt from the components): every WaterBodyComponent with its entity's position and its wave set's waves.
//
// The lookup is a parameter, as DestructibleLifetime's: the engine passes WaterWavesService and a suite hands
// it waves built in memory. A wave set that cannot be read refuses its body by name, once per entity per Play;
// a set still being read leaves the body out of THIS gather only, and the next step's gather takes it in.

#include <Engine/Assets/Common.hpp>
#include <Engine/Water/WaterBodyQuery.hpp>

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>

#include <functional>
#include <memory>
#include <string>
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

        /// Every water body of the registry that is ready, in entity order.
        [[nodiscard]] std::vector<Water::WaterBodyState> Gather( entt::registry&    registry,
                                                                 const WavesLookup& lookup );

        /// Forgets the refusals: a new Play reports them again.
        void Reset();

    private:
        std::unordered_set<entt::entity> m_Refused;
    };
} // namespace Desert::ECS
