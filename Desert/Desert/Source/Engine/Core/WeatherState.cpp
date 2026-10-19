#include <Engine/Core/WeatherState.hpp>

#include <Engine/Core/WorldParameterInstance.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/ExponentialHeightFogComponent.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/ECS/WeatherZoneComponent.hpp>
#include <Engine/ECS/WindSourceComponent.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Core
{
    namespace
    {
        std::optional<float> LerpOptional( const std::optional<float>& a, const std::optional<float>& b,
                                           const float t )
        {
            if ( a && b )
                return *a + ( *b - *a ) * t;
            return a ? a : b;
        }

        glm::vec3 PositionOf( entt::registry& registry, const entt::entity entity )
        {
            return glm::vec3( ECS::Entity( entity, registry ).GetWorldTransform()[3] );
        }

        Common::ResultStr<const Assets::Serialization::WeatherPresetData*>
        ReadPreset( const Assets::AssetHandle& preset )
        {
            using Result = const Assets::Serialization::WeatherPresetData*;
            if ( preset == Assets::AssetHandle{} )
                return Common::MakeError<Result>( std::string( "the zone names no preset" ) );
            return Runtime::ResourceRegistry::GetWeatherPresetService()->Get( preset );
        }
    } // namespace

    WeatherValues WeatherValues::FromPreset( const Assets::Serialization::WeatherPresetData& preset )
    {
        WeatherValues values;
        values.Parameters    = preset.Parameters;
        values.CloudCoverage = preset.CloudCoverage;
        values.FogDensity    = preset.FogDensity;
        values.WindSpeed     = preset.WindSpeed;
        return values;
    }

    const Assets::Serialization::WeatherParameterValue* WeatherValues::Find( const std::string& name ) const
    {
        for ( const auto& p : Parameters )
            if ( p.Name == name )
                return &p;
        return nullptr;
    }

    WeatherValues WeatherValues::Lerp( const WeatherValues& a, const WeatherValues& b, const float t )
    {
        WeatherValues out;
        out.Parameters = a.Parameters;
        for ( auto& p : out.Parameters )
            if ( const auto* to = b.Find( p.Name ) )
                for ( size_t c = 0; c < p.Value.size(); ++c )
                    p.Value[c] += ( to->Value[c] - p.Value[c] ) * t;
        for ( const auto& p : b.Parameters )
            if ( a.Find( p.Name ) == nullptr )
                out.Parameters.push_back( p );
        out.CloudCoverage = LerpOptional( a.CloudCoverage, b.CloudCoverage, t );
        out.FogDensity    = LerpOptional( a.FogDensity, b.FogDensity, t );
        out.WindSpeed     = LerpOptional( a.WindSpeed, b.WindSpeed, t );
        return out;
    }

    float WeatherZoneWeight( const bool unbound, const glm::vec3& zonePosition, const glm::vec3& extent,
                             const float blendWidth, const glm::vec3& point )
    {
        if ( unbound )
            return 1.0f;
        const glm::vec3 outside  = glm::max( glm::abs( point - zonePosition ) - extent, 0.0f );
        const float     distance = glm::length( outside );
        if ( distance <= 0.0f )
            return 1.0f;
        if ( blendWidth <= 0.0f || distance >= blendWidth )
            return 0.0f;
        return 1.0f - distance / blendWidth;
    }

    void WeatherState::Tick( entt::registry& registry, const float seconds )
    {
        for ( auto it = m_Zones.begin(); it != m_Zones.end(); )
        {
            if ( !registry.valid( it->first ) || !registry.has<ECS::WeatherZoneComponent>( it->first ) )
                it = m_Zones.erase( it );
            else
                ++it;
        }
        for ( const entt::entity entity : registry.view<ECS::WeatherZoneComponent>() )
        {
            auto [it, fresh]       = m_Zones.try_emplace( entity );
            WeatherZoneState& zone = it->second;
            if ( fresh )
            {
                zone.Preset = registry.get<ECS::WeatherZoneComponent>( entity ).Data.Preset;
                if ( const auto preset = ReadPreset( zone.Preset ); preset )
                    zone.Current = WeatherValues::FromPreset( *preset.GetValue() );
                // A preset that cannot be read was reported by the service once; the zone then states nothing.
                continue;
            }
            if ( zone.Duration <= 0.0f )
                continue;
            const auto target = ReadPreset( zone.Preset );
            if ( !target )
            {
                zone.Duration = 0.0f;
                continue;
            }
            zone.Elapsed       = std::min( zone.Elapsed + std::max( seconds, 0.0f ), zone.Duration );
            const float linear = zone.Elapsed / zone.Duration;
            const float eased  = linear * linear * ( 3.0f - 2.0f * linear );
            zone.Current =
                 WeatherValues::Lerp( zone.From, WeatherValues::FromPreset( *target.GetValue() ), eased );
            if ( zone.Elapsed >= zone.Duration )
                zone.Duration = 0.0f;
        }
    }

    Common::BoolResultStr WeatherState::Set( entt::registry& registry, const entt::entity zone,
                                             const Assets::AssetHandle& preset, const float seconds )
    {
        if ( !registry.valid( zone ) || !registry.has<ECS::WeatherZoneComponent>( zone ) )
            return Common::MakeFormattedError<bool>( "entity {} is not a weather zone",
                                                     static_cast<uint32_t>( zone ) );
        if ( !std::isfinite( seconds ) || seconds < 0.0f )
            return Common::MakeFormattedError<bool>( "the transition time {} is not a time >= 0", seconds );
        const auto target = ReadPreset( preset );
        if ( !target )
            return Common::MakeFormattedError<bool>( "preset {}: {}", static_cast<uint64_t>( preset ),
                                                     target.GetError() );
        // A zone set in the frame it appeared starts from its authored preset, not from nothing.
        Tick( registry, 0.0f );
        WeatherZoneState& state = m_Zones[zone];
        state.Preset            = preset;
        state.From              = state.Current;
        state.Elapsed           = 0.0f;
        state.Duration          = seconds;
        if ( seconds <= 0.0f )
            state.Current = WeatherValues::FromPreset( *target.GetValue() );
        return BOOLSUCCESS;
    }

    WeatherValues WeatherState::At( entt::registry& registry, const glm::vec3& point ) const
    {
        struct Layer
        {
            float                   Priority;
            entt::entity            Entity;
            float                   Weight;
            const WeatherZoneState* State;
        };
        std::vector<Layer> layers;
        for ( const auto& [entity, state] : m_Zones )
        {
            if ( !registry.valid( entity ) || !registry.has<ECS::WeatherZoneComponent>( entity ) )
                continue;
            const ECS::WeatherZoneData& data = registry.get<ECS::WeatherZoneComponent>( entity ).Data;
            const float weight = WeatherZoneWeight( data.Unbound, PositionOf( registry, entity ), data.Extent,
                                                    data.BlendWidth, point );
            if ( weight > 0.0f )
                layers.push_back( { data.Priority, entity, weight, &state } );
        }
        // Lowest priority first, so the highest is laid last and wins; ties by entity for the same answer on
        // every machine.
        std::sort( layers.begin(), layers.end(), []( const Layer& a, const Layer& b )
                   { return a.Priority != b.Priority ? a.Priority < b.Priority : a.Entity < b.Entity; } );
        WeatherValues out;
        for ( const Layer& layer : layers )
            out = WeatherValues::Lerp( out, layer.State->Current, layer.Weight );
        return out;
    }

    void WeatherState::Apply( entt::registry& registry, WorldParameterInstance& parameters,
                              const glm::vec3& observer )
    {
        if ( m_Zones.empty() )
            return;
        const WeatherValues here = At( registry, observer );

        for ( const auto& p : here.Parameters )
        {
            const auto values = parameters.Values();
            const auto found  = std::find_if( values.begin(), values.end(),
                                              [&p]( const WorldParameterValue& v ) { return v.Name == p.Name; } );
            if ( found == values.end() )
            {
                if ( m_Reported.insert( p.Name ).second )
                    LOG_ERROR( "[Weather] a preset states world parameter '{}', which the scene's collection does "
                               "not declare; the value goes nowhere",
                               p.Name );
                continue;
            }
            const auto written = found->Type == Assets::Serialization::WorldParameterType::Scalar
                                      ? parameters.SetScalar( p.Name, p.Value[0] )
                                      : parameters.SetVector(
                                             p.Name, glm::vec4( p.Value[0], p.Value[1], p.Value[2], p.Value[3] ) );
            if ( !written && m_Reported.insert( p.Name ).second )
                LOG_ERROR( "[Weather] world parameter '{}': {}", p.Name, written.GetError() );
        }

        if ( here.CloudCoverage )
            for ( const entt::entity entity : registry.view<ECS::VolumetricCloudComponent>() )
            {
                auto&           params = registry.get<ECS::VolumetricCloudComponent>( entity ).Data.DynamicParams;
                const auto      it     = std::find_if( params.begin(), params.end(),
                                                       []( const auto& kv ) { return kv.first == "Coverage"; } );
                const glm::vec4 value( *here.CloudCoverage, 0.0f, 0.0f, 0.0f );
                if ( it == params.end() )
                    params.emplace_back( "Coverage", value );
                else
                    it->second = value;
            }

        if ( here.FogDensity )
            for ( const entt::entity entity : registry.view<ECS::ExponentialHeightFogComponent>() )
                registry.get<ECS::ExponentialHeightFogComponent>( entity ).Data.FogDensity = *here.FogDensity;

        // The ZONE'S wind, not the blend at the observer: the WindSource on the zone's entity is that zone's air,
        // and ECS::WindAt sums the sources where each consumer stands.
        for ( const auto& [entity, state] : m_Zones )
            if ( state.Current.WindSpeed && registry.valid( entity ) &&
                 registry.has<ECS::WindSourceComponent>( entity ) )
                registry.get<ECS::WindSourceComponent>( entity ).Data.Speed = *state.Current.WindSpeed;
    }

    const WeatherZoneState* WeatherState::Zone( const entt::entity zone ) const
    {
        const auto it = m_Zones.find( zone );
        return it == m_Zones.end() ? nullptr : &it->second;
    }
} // namespace Desert::Core
