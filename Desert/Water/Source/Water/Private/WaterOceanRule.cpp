#include <Water/Public/WaterOceanRule.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/ECS/TagComponent.hpp>
#include <Water/Public/WaterBodyComponent.hpp>

#include <algorithm>
#include <string>

namespace Desert::ECS
{
    namespace
    {
        std::string NameOf( const entt::registry& registry, const entt::entity entity )
        {
            const auto* const tag = registry.try_get<TagComponent>( entity );
            return tag != nullptr ? tag->Tag : std::to_string( static_cast<uint32_t>( entity ) );
        }
    } // namespace

    void WaterOceanRule::Refused( entt::registry& registry, const IsFFTSet& isFFT, std::vector<entt::entity>& out )
    {
        const std::lock_guard<std::mutex> lock( m_Mutex );
        const auto                        view = registry.view<WaterBodyComponent>();

        entt::entity ocean = entt::null;
        m_Found.clear();
        for ( const entt::entity entity : view )
        {
            const Assets::AssetHandle waves = view.get<WaterBodyComponent>( entity ).Data.WaterWaves;
            if ( static_cast<uint64_t>( waves ) == 0 || !isFFT( waves ) )
                continue;
            m_Found.push_back( entity );
            if ( ocean == entt::null || static_cast<uint32_t>( entity ) < static_cast<uint32_t>( ocean ) )
                ocean = entity;
        }
        if ( ocean != entt::null )
        {
            const Assets::AssetHandle owned = view.get<WaterBodyComponent>( ocean ).Data.WaterWaves;
            std::erase_if( m_Found, [&]( const entt::entity entity )
                           { return view.get<WaterBodyComponent>( entity ).Data.WaterWaves == owned; } );
            std::sort( m_Found.begin(), m_Found.end() );
        }
        if ( m_Ocean != ocean || m_Said != m_Found )
        {
            for ( const entt::entity entity : m_Found )
                LOG_ERROR( "[Water] '{}' is no water body: it names another FFT wave set than '{}', and a world has "
                           "one ocean - give both the same WaterWaves, or this one a Gerstner set",
                           NameOf( registry, entity ), NameOf( registry, ocean ) );
            m_Ocean = ocean;
            m_Said  = m_Found;
        }
        out = m_Found;
    }
} // namespace Desert::ECS
