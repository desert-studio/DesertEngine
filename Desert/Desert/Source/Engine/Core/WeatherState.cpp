#include <Engine/Core/WeatherState.hpp>

#include <Engine/Core/WorldParameterInstance.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/ExponentialHeightFogComponent.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/ECS/WeatherZoneComponent.hpp>
#include <Engine/ECS/WindSourceComponent.hpp>
#include <Engine/Graphic/Materials/MaterialOverrides.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Weather/WeatherPresetService.hpp>

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

        void SetDynamicParam( std::vector<std::pair<std::string, glm::vec4>>& params, const std::string& name,
                              const float value )
        {
            const auto      it = std::find_if( params.begin(), params.end(),
                                               [&name]( const auto& kv ) { return kv.first == name; } );
            const glm::vec4 v( value, 0.0f, 0.0f, 0.0f );
            if ( it == params.end() )
                params.emplace_back( name, v );
            else
                it->second = v;
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
        values.CloudDensity  = preset.CloudDensity;
        for ( const auto& type : preset.CloudTypes )
        {
            const auto handle =
                 Runtime::ResourceRegistry::GetWeatherPresetService()->CloudTypeHandleForPath( type.Type );
            if ( !handle )
            {
                LOG_ERROR( "[Weather] a preset's cloud type '{}' is not shown: {}", type.Type, handle.GetError() );
                continue;
            }
            values.CloudTypes.emplace_back( handle.GetValue(), type.Weight );
        }
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
        out.CloudDensity  = LerpOptional( a.CloudDensity, b.CloudDensity, t );
        // Per type, as D-14 has it: a side that states a set states every type it leaves out at 0, so Clear's
        // cumulus fades out while Rain's stratus fades in. A side that states no set holds the other's.
        if ( a.CloudTypes.empty() || b.CloudTypes.empty() )
            out.CloudTypes = a.CloudTypes.empty() ? b.CloudTypes : a.CloudTypes;
        else
        {
            const auto weightIn = []( const WeatherValues& v, const Assets::AssetHandle& type )
            {
                for ( const auto& [handle, weight] : v.CloudTypes )
                    if ( handle == type )
                        return weight;
                return 0.0f;
            };
            out.CloudTypes = a.CloudTypes;
            for ( const auto& [handle, weight] : b.CloudTypes )
                if ( std::none_of( a.CloudTypes.begin(), a.CloudTypes.end(),
                                   [&handle]( const auto& kv ) { return kv.first == handle; } ) )
                    out.CloudTypes.emplace_back( handle, 0.0f );
            for ( auto& [handle, weight] : out.CloudTypes )
                weight += ( weightIn( b, handle ) - weight ) * t;
        }
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

        for ( const entt::entity entity : registry.view<ECS::VolumetricCloudComponent>() )
        {
            auto& data = registry.get<ECS::VolumetricCloudComponent>( entity ).Data;
            if ( here.CloudCoverage )
                SetDynamicParam( data.DynamicParams, "Coverage", *here.CloudCoverage );
            if ( here.CloudDensity )
                SetDynamicParam( data.DynamicParams, "DensityScale", *here.CloudDensity );
            if ( here.CloudTypes.empty() )
                continue;
            // WHICH SLOT HOLDS EACH TYPE is the layer material's answer (CloudType1..4 of its chain), so a preset
            // names types and works on any sky that has them; a slot whose type the weather does not state is at
            // 0.
            Graphic::MaterialOverrides overrides;
            if ( !Runtime::ResourceRegistry::GetMaterialService()->ResolveOverrides( data.Material, overrides ) )
                continue;
            std::vector<bool> placed( here.CloudTypes.size(), false );
            for ( uint32_t slot = 0; slot < ECS::kCloudTypeSlots; ++slot )
            {
                const std::string slotName = "CloudType" + std::to_string( slot + 1u );
                const auto        held     = std::find_if( overrides.Textures.begin(), overrides.Textures.end(),
                                                           [&slotName]( const auto& kv ) { return kv.first == slotName; } );
                if ( held == overrides.Textures.end() || held->second == 0u )
                    continue;
                float weight = 0.0f;
                for ( size_t i = 0; i < here.CloudTypes.size(); ++i )
                    if ( static_cast<uint64_t>( here.CloudTypes[i].first ) == held->second )
                    {
                        weight    = here.CloudTypes[i].second;
                        placed[i] = true;
                    }
                SetDynamicParam( data.DynamicParams, "CloudTypeWeight" + std::to_string( slot + 1u ), weight );
            }
            for ( size_t i = 0; i < here.CloudTypes.size(); ++i )
                if ( !placed[i] && here.CloudTypes[i].second > 0.0f &&
                     m_Reported
                          .insert( "cloudtype:" +
                                   std::to_string( static_cast<uint64_t>( here.CloudTypes[i].first ) ) )
                          .second )
                    LOG_ERROR(
                         "[Weather] the weather shows cloud type {}, which no type slot of the cloud layer's "
                         "material holds; that share of the sky is not drawn",
                         static_cast<uint64_t>( here.CloudTypes[i].first ) );
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
