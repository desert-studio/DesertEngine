#pragma once

// A DESTRUCTIBLE OBJECT (UE: UGeometryCollectionComponent). The entity's world pose places a baked `.dfrac`
// (its RestCollection) in the scene's Destruction::DestructionWorld when Play starts; the entity going (or the
// component being removed) takes every body of it out again — ECS/System/DestructibleLifetime.hpp.
// The pieces are drawn by DST-06; this component owns the simulation's inputs only.

#include <Engine/Assets/Common.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace Desert::ECS
{
    struct DestructibleData
    {
        REFLECT()

        PROPERTY( DisplayName( "Rest Collection" ), Category( "Destructible" ), Summary, Asset<FractureAsset>,
                  Tooltip( "The baked fracture this object is (UE RestCollection) — drag a .dfrac from the "
                           "Content Browser. An empty slot is refused at Play by name, not simulated as "
                           "nothing." ) )
        Assets::AssetHandle Fracture;

        PROPERTY( DisplayName( "Damage Threshold" ), Category( "Damage" ),
                  Tooltip( "Per level of the hierarchy (UE DamageThreshold): entry L is the strain a level-L "
                           "piece breaks off at, and replaces the bake's threshold of that level. Levels past "
                           "the list keep the bake's. Zero or less breaks the level at the first contact." ) )
        std::vector<float> DamageThreshold;

        PROPERTY( DisplayName( "Anchored Nodes" ), Category( "Destructible" ),
                  Tooltip( "Nodes of the fracture that never move, with every leaf below them (UE anchor "
                           "field). A body holding one is static; it still breaks, and only its free pieces "
                           "fly." ) )
        std::vector<int32_t> AnchoredNodes;

        PROPERTY( DisplayName( "Density" ), Category( "Physics" ), Range( 0.00001f, 0.1f ), Units( "kg/cm3" ),
                  Tooltip( "Mass per volume of the pieces; 0.0024 is concrete." ) )
        float DensityKgPerCm3 = 0.0024f;

        PROPERTY( DisplayName( "Friction" ), Category( "Physics" ), Range( 0.0f, 2.0f ) )
        float Friction = 0.6f;

        PROPERTY( DisplayName( "Restitution" ), Category( "Physics" ), Range( 0.0f, 1.0f ) )
        float Restitution = 0.1f;

        PROPERTY( DisplayName( "Remove On Sleep" ), Category( "Removal" ),
                  Tooltip( "A broken-off piece that has slept for its sleep time leaves the simulation (UE "
                           "bRemoveOnMaxSleep)." ) )
        bool RemoveOnSleep = true;

        PROPERTY( DisplayName( "Max Sleep Time" ), Category( "Removal" ), Units( "s" ),
                  Tooltip( "Seconds asleep before removal, drawn per piece in [x, y] (UE MaximumSleepTime)." ) )
        glm::vec2 MaxSleepTime{ 5.0f, 10.0f };

        PROPERTY( DisplayName( "Slow Moving As Sleeping" ), Category( "Removal" ),
                  Tooltip( "A piece creeping slower than the threshold counts as asleep (UE "
                           "bSlowMovingAsSleeping)." ) )
        bool SlowMovingAsSleeping = true;

        PROPERTY( DisplayName( "Slow Moving Velocity Threshold" ), Category( "Removal" ), Range( 0.0f, 1000.0f ),
                  Units( "cm/s" ) )
        float SlowMovingVelocityThreshold = 1.0f;

        // UE UGeometryCollectionComponent::OnChaosBreakEvent (GeometryCollectionComponent.h:1269): a piece of
        // THIS destructible broke off. Destruction publishes the fact (DestructionEventQueue); the Gameplay
        // Framework delivers it here (ComponentEventSystem) to whoever subscribed to this entity.
        EVENT( Category( "Destructible" ),
               Tooltip( "A piece broke off (UE OnChaosBreakEvent): its fracture node, its world centre of mass "
                        "in cm and its velocity in cm/s." ) )
        using OnBreak = void( int32_t node, glm::vec3 position, glm::vec3 velocity );
    };

    struct DestructibleComponent
    {
        COMPONENT( Key( "Destructible" ), Block( Data ), Run( SkyAndAtmosphere ) )
        DestructibleData Data;

        // Transient: the object in the scene's DestructionWorld (Destruction::DestructibleHandle; created on
        // Play, gone on Stop). Not reflected/serialized.
        uint32_t RuntimeObject = 0xFFFFFFFFu;
    };
} // namespace Desert::ECS
