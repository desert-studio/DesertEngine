#pragma once

// THE WORLD'S WEATHER (WTH-STATE, owner 10-18). The mechanism is the engine's; the rules (schedules, random
// cycles, admin commands) are the game's Luau, which calls SetWeather.
//
//   WeatherZone components (ECS/WeatherZoneComponent.hpp) — authored: which preset each zone rests at, where it
//   applies, its priority and blend width. The Unbound zone is the world's base weather.
//   WeatherState (this) — runtime, SERVER-AUTHORITATIVE: per zone, the weather it shows now and the transition it
//   is in (from a snapshot of the values, to a target preset, over N seconds). WTH-NET replicates the zone's
//   (target preset, start time, duration) and every client interpolates the same curve.
//   WeatherAt(point) — the one query, as ECS::WindAt: every zone's current values blended at a point by priority
//   and distance (the PostProcessVolume rule of Graphic/ViewSettings.cpp).
//   Apply — the weather at the observer (the active camera) written into the consumers' OWN values: the named
//   world parameters (Core::WorldParameterInstance), the cloud layer's material "Coverage"
//   (VolumetricCloudData::DynamicParams) with its "DensityScale" and the "CloudTypeWeight<k>" of every type slot
//   (WTH-OVERCAST: the preset names the TYPES, Apply finds which slot of the layer's material holds each), the
//   height fog's FogDensity; and each zone's WindSpeed into the
//   WindSource on its own entity. Nothing reads weather from here except those writes.

#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/Serialization/WeatherPreset.hpp>

#include <Common/Core/ResultStr.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::Core
{
    class WorldParameterInstance;

    /// The values one weather states, in the form they blend: named world parameters plus the sky and the wind.
    /// A value no side states is absent, and an absent value is not written.
    struct WeatherValues
    {
        std::vector<Assets::Serialization::WeatherParameterValue> Parameters;
        std::optional<float>                                      CloudCoverage;
        // The preset's CloudTypes resolved to handles (WTH-OVERCAST); empty = the layer's weights are left alone.
        std::vector<std::pair<Assets::AssetHandle, float>>        CloudTypes;
        std::optional<float>                                      CloudDensity;
        std::optional<float>                                      FogDensity;
        std::optional<float>                                      WindSpeed;

        [[nodiscard]] static WeatherValues FromPreset( const Assets::Serialization::WeatherPresetData& preset );

        /// a -> b by t in [0,1]. A value only one side states holds that side's value (there is nothing to move
        /// from or to), so presets meant to blend state the same names — the eleven defaults do.
        [[nodiscard]] static WeatherValues Lerp( const WeatherValues& a, const WeatherValues& b, float t );

        [[nodiscard]] const Assets::Serialization::WeatherParameterValue* Find( const std::string& name ) const;
    };

    /// How far `point` is inside a zone, 0..1: the PostProcessVolume weight (Graphic/ViewSettings.cpp
    /// PostProcessVolumeWeight) — 1 inside the box or unbound, falling linearly to 0 across BlendWidth.
    [[nodiscard]] float WeatherZoneWeight( bool unbound, const glm::vec3& zonePosition, const glm::vec3& extent,
                                           float blendWidth, const glm::vec3& point );

    struct WeatherZoneState
    {
        Assets::AssetHandle Preset;  // the preset the zone is at, or moving to
        WeatherValues       Current; // what it shows this frame
        WeatherValues       From;    // the snapshot a transition started from
        float               Elapsed  = 0.0f;
        float               Duration = 0.0f; // 0 = not in a transition
    };

    class WeatherState
    {
    public:
        /// Advance every zone by `seconds` of gameplay time: a zone seen for the first time starts AT its
        /// authored preset; a zone whose entity is gone is dropped; a transition moves Current along a
        /// smoothstep of Elapsed / Duration (monotone, flat at both ends).
        void Tick( entt::registry& registry, float seconds );

        /// Start moving `zone` to `preset` over `seconds` (0 = at once) from what it shows now. Refused for an
        /// entity with no WeatherZone, a negative or non-finite time, or a preset that cannot be read.
        Common::BoolResultStr Set( entt::registry& registry, entt::entity zone, const Assets::AssetHandle& preset,
                                   float seconds );

        /// The weather at a world point: every zone's Current by ascending priority, each laid over what lies
        /// beneath by its weight. Values no zone states stay absent.
        [[nodiscard]] WeatherValues At( entt::registry& registry, const glm::vec3& point ) const;

        /// Write the weather at `observer` into its consumers (see the file comment). Names a preset states that
        /// the world's collection lacks are reported once each.
        void Apply( entt::registry& registry, WorldParameterInstance& parameters, const glm::vec3& observer );

        [[nodiscard]] const WeatherZoneState* Zone( entt::entity zone ) const;

        void Clear()
        {
            m_Zones.clear();
        }

    private:
        std::unordered_map<entt::entity, WeatherZoneState> m_Zones;
        std::unordered_set<std::string>                    m_Reported;
    };
} // namespace Desert::Core
