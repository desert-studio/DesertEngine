#pragma once

// AI-STATETREE CONTRACT (header only, owner review pending) - docs/contracts/AI-STATETREE.md.
//
// PERCEPTION VOCABULARY (UE AIModule/Classes/Perception: AIPerceptionTypes.h FAIStimulus, AISenseConfig_Sight.h,
// AISenseConfig_Hearing.h, AISenseConfig_Damage.h). Units: centimetres, seconds, degrees.

#include <CoreReflection/ReflectionMacros.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace Desert::AI::Perception
{
    enum class Sense : std::uint8_t
    {
        Sight,
        Hearing,
        Damage,
        Count
    };

    /// Who a sense reports (UE FAISenseAffiliationFilter). Teams come from AIPerceptionStimuliSourceData::Team
    /// against AIPerceptionData::Team (UE IGenericTeamAgentInterface, here two integers until a team system
    /// exists): same team = Friendly, Team 0 = Neutral, else Enemy.
    struct AffiliationFilter
    {
        REFLECT()

        PROPERTY( DisplayName( "Enemies" ), Category( "Affiliation" ) )
        bool Enemies = true;
        PROPERTY( DisplayName( "Neutrals" ), Category( "Affiliation" ) )
        bool Neutrals = false;
        PROPERTY( DisplayName( "Friendlies" ), Category( "Affiliation" ) )
        bool Friendlies = false;
    };

    /// UE UAISenseConfig_Sight (AISenseConfig_Sight.h:18).
    struct SightConfig
    {
        REFLECT()

        PROPERTY( DisplayName( "Enabled" ), Category( "Sight" ) )
        bool Enabled = true;

        PROPERTY( DisplayName( "Sight Radius" ), Category( "Sight" ), Length, Range( 0.0f, 1.0e6f ),
                  Tooltip( "UE SightRadius: a target inside this distance and the cone, with a clear line, is "
                           "seen." ) )
        float SightRadius = 3000.0f;

        PROPERTY( DisplayName( "Lose Sight Radius" ), Category( "Sight" ), Length, Range( 0.0f, 1.0e6f ),
                  Tooltip( "UE LoseSightRadius: a SEEN target stays seen until it passes this distance (>= "
                           "Sight Radius; the gap is the hysteresis)." ) )
        float LoseSightRadius = 3500.0f;

        PROPERTY( DisplayName( "Peripheral Vision Half Angle" ), Category( "Sight" ), Range( 0.0f, 180.0f ),
                  Units( "deg" ), Tooltip( "UE PeripheralVisionAngleDegrees: half-angle of the view cone." ) )
        float PeripheralHalfAngleDegrees = 70.0f;

        PROPERTY( DisplayName( "Auto Success Range" ), Category( "Sight" ), Length, Range( 0.0f, 1.0e6f ),
                  Tooltip( "UE AutoSuccessRangeFromLastSeenLocation: a target that was seen and is still within "
                           "this distance of where it was last seen stays seen without a trace. 0 = off." ) )
        float AutoSuccessRange = 0.0f;

        PROPERTY( DisplayName( "Max Age" ), Category( "Sight" ), Range( 0.0f, 3600.0f ), Units( "s" ),
                  Tooltip( "UE MaxAge: seconds a lost stimulus is remembered. 0 = forever." ) )
        float MaxAgeSeconds = 5.0f;

        PROPERTY( DisplayName( "Eye Height" ), Category( "Sight" ), Length,
                  Tooltip( "Height of the eye above the entity origin (UE GetActorEyesViewPoint)." ) )
        float EyeHeight = 160.0f;

        PROPERTY( DisplayName( "Detect" ), Category( "Sight" ) )
        AffiliationFilter Detect;
    };

    /// UE UAISenseConfig_Hearing (AISenseConfig_Hearing.h).
    struct HearingConfig
    {
        REFLECT()

        PROPERTY( DisplayName( "Enabled" ), Category( "Hearing" ) )
        bool Enabled = true;

        PROPERTY( DisplayName( "Hearing Range" ), Category( "Hearing" ), Length, Range( 0.0f, 1.0e6f ),
                  Tooltip( "UE HearingRange: a noise of loudness 1 is heard within this distance; the range "
                           "scales with the noise's loudness (UE: Range * Loudness)." ) )
        float HearingRange = 3000.0f;

        PROPERTY( DisplayName( "Max Age" ), Category( "Hearing" ), Range( 0.0f, 3600.0f ), Units( "s" ) )
        float MaxAgeSeconds = 3.0f;

        PROPERTY( DisplayName( "Detect" ), Category( "Hearing" ) )
        AffiliationFilter Detect;
    };

    /// UE UAISenseConfig_Damage: reported by the damage path, no range.
    struct DamageConfig
    {
        REFLECT()

        PROPERTY( DisplayName( "Enabled" ), Category( "Damage" ) )
        bool Enabled = true;

        PROPERTY( DisplayName( "Max Age" ), Category( "Damage" ), Range( 0.0f, 3600.0f ), Units( "s" ) )
        float MaxAgeSeconds = 5.0f;
    };

    /// UE FAIStimulus: what one sense knows about one source.
    struct Stimulus
    {
        Sense        Type   = Sense::Sight;
        entt::entity Source = entt::null;      // the actor perceived (noise: its instigator)
        glm::vec3    StimulusLocation{ 0.0f }; // where it was perceived (last seen / noise position)
        glm::vec3    ReceiverLocation{ 0.0f };
        float        Strength   = 0.0f; // sight 1; hearing: loudness; damage: amount
        float        AgeSeconds = 0.0f;
        bool         Sensed     = false; // false = lost, remembered until MaxAge (UE WasSuccessfullySensed)
        std::string  Tag;                // noise / damage tag ("Gunshot", "Horn"); empty for sight
    };

    /// A noise reported by gameplay (UE UAISense_Hearing::ReportNoiseEvent, AISense_Hearing.h:64). From game
    /// code, never derived from the audio engine: a sound the player hears is not necessarily one the NPC
    /// should react to, and a silent footstep can still be a noise.
    struct NoiseEvent
    {
        glm::vec3    Location{ 0.0f };
        float        Loudness   = 1.0f; // multiplies the listener's HearingRange
        entt::entity Instigator = entt::null;
        float        MaxRange   = 0.0f; // UE MaxRange: hard cap on distance regardless of listener; 0 = none
        std::string  Tag;
    };

    struct DamageEvent
    {
        entt::entity Damaged    = entt::null;
        entt::entity Instigator = entt::null;
        float        Amount     = 0.0f;
        glm::vec3    EventLocation{ 0.0f };
        glm::vec3    HitLocation{ 0.0f };
        std::string  Tag;
    };
} // namespace Desert::AI::Perception
