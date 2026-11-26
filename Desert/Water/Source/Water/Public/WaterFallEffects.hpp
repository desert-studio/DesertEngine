#pragma once

#include <Common/Core/UUID.hpp>
#include <Engine/Core/ModuleHooks.hpp>
#include <Water/Public/WaterRiver.hpp>

#include <entt/entt.hpp>

#include <cstdint>
#include <map>
#include <tuple>

namespace Desert::Water
{
    /// The landing speed (cm/s) at which a fall's foot plays its effects at their full spawn rate (a drop of about
    /// 11 m).
    inline constexpr float kFallFullIntensitySpeed = 1500.0f;

    /**
     * @brief The effects a scene's river falls play (UE Water spawns a body's Niagara systems by code the same way):
     *        for every fall of every visible river one transient entity per asset the river names
     *        (WaterBodyData::FallSpray / FallMist / FallSound). The falls are the world's bakes
     *        (ECS::WaterRiverBakes::Find); nothing is baked here.
     *
     * The spray and the mist stand at the plunge's foot (Water::FallPlunge), X across the landing line, with
     * (Width, 0, Radius) as User.Extent and the landing speed as their spawn share. The sound is an
     * AudioSourceComponent as loud as the water lands fast, moved every tick to the point of the sheet nearest the
     * active camera (the listener), so a wide fall sounds as wide as it is. An entity follows the bake and goes
     * when its fall, river or asset does. A root of the scene, never the river's child: a child would be the
     * river's subtree in its documents.
     */
    class WaterFallEffects final : public Core::ISceneModuleState
    {
    public:
        void Tick( Core::Scene& scene, entt::registry& registry, double seconds ) override;
        void Reset() override;
        void Clear() override;

    private:
        struct Effect
        {
            entt::entity Entity = entt::null;
            Common::UUID Id;
            uint64_t     SeenTick = 0u;
        };
        // The river's entity, the fall's index on it, the effect's kind.
        using EffectKey = std::tuple<entt::entity, uint32_t, uint32_t>;

        std::map<EffectKey, Effect> m_Effects;
        uint64_t                    m_Tick = 0u;
    };
} // namespace Desert::Water
