#pragma once

// THE RIVERS' BAKES OF ONE WORLD (UE: the river's velocity in the WaterInfo bake, once per body): the ONLY caller
// of Water::BakeRiver. Refreshed serially in the Water module's Prepares hook, right after WaterWaveSources and
// before any system; every reader of the frame (WaterZoneECSSystem, WaterBodyGather, WaterFallEffects) reads it
// const. It lives in the registry's ctx as std::shared_ptr<WaterRiverBakes> (made by a SceneStates maker, as
// WaterWaveSources is): a second world in the process has its own.
#include <Water/Public/WaterRiver.hpp>

#include <Common/Core/Entt.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::ECS
{
    class WaterWaveSources; // Water/Public/WaterWaveSources.hpp

    class WaterRiverBakes final
    {
    public:
        struct River
        {
            uint64_t                                     Key = 0u; // Water::RiverBakeKey it was baked for
            std::shared_ptr<const Water::RiverFlowField> Flow;     // never null; Size 0 = cannot be drawn
        };

        /// Re-bakes every visible, unrefused river whose RiverBakeKey changed (its spline, width, flow, location,
        /// the scene's gravity, the oceans it may end in) and forgets the rivers that are gone. Bumps Revision()
        /// when any bake changed. A registry with no Core::SceneSettingsLink (or a scene without gravity) bakes
        /// nothing and says so once (RefusalReason): a river without a gravity has no falls to draw.
        void Refresh( entt::registry& registry, const WaterWaveSources& sources );

        /// The bake of @p river, or nullptr: not a river of this world, hidden, or refused by the wave record.
        [[nodiscard]] const River* Find( entt::entity river ) const noexcept;

        /// Raised by every changed bake: a reader keyed on it rebuilds its rows then and at no other time.
        [[nodiscard]] uint64_t Revision() const noexcept
        {
            return m_Revision;
        }

        /// Why nothing is baked ("" = baked); logged once per change by Refresh.
        [[nodiscard]] const std::string& RefusalReason() const noexcept
        {
            return m_Refusal;
        }

        /// The world's entities are gone (Scene::Clear): every bake is dropped.
        void Reset() noexcept;

    private:
        std::unordered_map<entt::entity, River> m_Rivers;
        std::vector<Water::RiverReceiver>       m_Receivers; // storage reused per Refresh
        uint64_t                                m_Revision = 0u;
        std::string                             m_Refusal;
    };
} // namespace Desert::ECS
