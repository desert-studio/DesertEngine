#pragma once

#include <Engine/Assets/Common.hpp>

#include <entt/entt.hpp>

#include <functional>
#include <mutex>
#include <vector>

namespace Desert::ECS
{
    /**
     * @brief One world, one wind, one ocean: a world's water bodies name at most one FFT wave set.
     *
     * The first body (by entity id) naming an FFT set is the world's ocean; a body naming another FFT set is
     * refused - an error naming both, once per change of the refusals - and is no water: neither drawn nor floated
     * on, never another set's waves. Any number of Gerstner sets is allowed.
     *
     * Lives in the registry's context as a shared_ptr, created by the module's Prepares hook; its two readers (the
     * zone collector and the physics' gather) run inside parallel groups, so it serialises them.
     */
    class WaterOceanRule final
    {
    public:
        /// Whether a wave set is an FFT ocean (Runtime::WaterWavesService::IsFFTSet).
        using IsFFTSet = std::function<bool( const Assets::AssetHandle& )>;

        /// Writes the bodies refused as the registry stands into @p out (reused storage).
        void Refused( entt::registry& registry, const IsFFTSet& isFFT, std::vector<entt::entity>& out );

    private:
        std::mutex                m_Mutex;
        entt::entity              m_Ocean = entt::null;
        std::vector<entt::entity> m_Said; // the refusals already reported, against m_Ocean
        std::vector<entt::entity> m_Found;
    };
} // namespace Desert::ECS
