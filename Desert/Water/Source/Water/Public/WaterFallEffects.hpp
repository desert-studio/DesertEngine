#pragma once

#include <Common/Core/UUID.hpp>
#include <Engine/Core/ModuleHooks.hpp>
#include <Water/Public/WaterRiver.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>

namespace Desert::Water
{
    /// The landing speed (cm/s) at which a fall's foot plays its effects at their full spawn rate (a drop of about
    /// 11 m).
    inline constexpr float kFallFullIntensitySpeed = 1500.0f;

    /**
     * @brief The effects a scene's river falls play at their foot (UE Water spawns a body's Niagara systems by code
     *        the same way): for every fall of every visible river one transient entity per .dfx the river names
     *        (WaterBodyData::FallFoam / FallSpray / FallMist).
     *
     * An entity stands at the fall's foot, X along the sheet, with the sheet's width as User.Extent and the landing
     * speed as its spawn share. It follows the bake, and goes when its fall, river or asset does. A root of the
     * scene, never the river's child: a child would be the river's subtree in its documents.
     */
    class WaterFallEffects final : public Core::ISceneModuleState
    {
    public:
        void Tick( Core::Scene& scene, entt::registry& registry, double seconds ) override;
        void Reset() override;
        void Clear() override;

    private:
        struct River
        {
            uint64_t                              Key = 0u;
            std::shared_ptr<const RiverFlowField> Flow;
        };
        struct Effect
        {
            entt::entity Entity = entt::null;
            Common::UUID Id;
            uint64_t     SeenTick = 0u;
        };
        // The river's entity, the fall's index on it, the effect's kind.
        using EffectKey = std::tuple<entt::entity, uint32_t, uint32_t>;

        std::unordered_map<entt::entity, River> m_Rivers;
        std::map<EffectKey, Effect>             m_Effects;
        uint64_t                                m_Tick = 0u;
    };
} // namespace Desert::Water
