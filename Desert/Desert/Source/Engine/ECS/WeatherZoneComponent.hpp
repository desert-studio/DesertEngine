#pragma once

// A WEATHER ZONE (WTH-STATE, owner 10-18): the volume pattern of UE's PostProcessVolume / AudioVolume applied to
// weather. The world's BASE weather is the Unbound zone (lowest priority, as a level's unbound post-process volume
// is its base grade); bounded zones override it inside their box and fade out across BlendRadius. Which zone wins
// where they overlap is Priority. The ONE query is Core::WeatherAt (Core/WeatherState.hpp), as ECS::WindAt is
// for wind.
//
// The zone names the preset it RESTS at; the weather it SHOWS is the world's WeatherState, which moves from one
// preset to another over a transition (SetWeather). That state is runtime and server-authoritative (WTH-NET
// replicates it); this component is the authored start.
//
// Wind: weather keeps no wind. A WindSource on the same entity is the zone's wind, and the weather writes its
// Speed (a preset's WindSpeed) — one truth for every wind consumer (foliage, cloud drift, cloth).

#include <Engine/Assets/Common.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

#include <glm/glm.hpp>

namespace Desert::ECS
{
    struct WeatherZoneData
    {
        REFLECT()

        PROPERTY( DisplayName( "Preset" ), Category( "Weather Zone" ), Summary, Asset<WeatherPresetAsset>,
                  Tooltip( "The weather this zone rests at (a .dweatherpreset). Scripts move it to another one "
                           "with Weather.set." ) )
        Assets::AssetHandle Preset;

        PROPERTY( DisplayName( "Infinite Extent (Unbound)" ), Category( "Weather Zone" ),
                  Tooltip( "Applies everywhere: the world's base weather. Bounded zones of a higher priority "
                           "override it inside their box." ) )
        bool Unbound = false;

        PROPERTY( DisplayName( "Extent" ), Category( "Weather Zone" ), Length,
                  Tooltip( "Half-size of the box, around the entity's position, inside which the zone's weather "
                           "applies in full." ) )
        glm::vec3 Extent = glm::vec3( 50000.0f );

        PROPERTY( DisplayName( "Blend Width" ), Category( "Weather Zone" ), Length, Range( 0.0f, 1000000.0f ),
                  Tooltip( "Distance outside the box across which the zone's weather fades into what lies "
                           "beneath it." ) )
        float BlendWidth = 10000.0f;

        PROPERTY( DisplayName( "Priority" ), Category( "Weather Zone" ),
                  Tooltip( "Higher priority is applied later and therefore wins where zones overlap." ) )
        float Priority = 0.0f;
    };

    struct WeatherZoneComponent
    {
        COMPONENT( Key( "WeatherZone" ), Block( Data ), Run( SkyAndAtmosphere ) )
        WeatherZoneData Data;
    };
} // namespace Desert::ECS
