#pragma once

// A DESTRUCTIBLE OBJECT (UE: UGeometryCollectionComponent). The entity's world pose places a baked `.dfrac`
// (its RestCollection) in the scene's Destruction::DestructionWorld when Play starts; the entity going (or the
// component being removed) takes every body of it out again — ECS/System/DestructibleLifetime.hpp.
// MeshECSSystem draws its pieces (Destruction/FracturePieces.hpp); this component owns the simulation's inputs
// and, transient, the poses the simulation hands the draw.

#include <Engine/Assets/Common.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Desert::Destruction
{
    struct FractureData;
}

namespace Desert::ECS
{
    /// Where a destructible's pieces come from (DST-RUNTIME, owner decision 10-13). One fragment format and one
    /// simulation for all three; UE has only the first (a Geometry Collection fractured in the editor).
    enum class FractureSource
    {
        Baked,    ///< The `.dfrac` in Rest Collection, cut in the Fracture mode (key objects, an artist's edit)
        OnLoad,   ///< Cut in the background when Play meets the object, its own seed per instance; no file
        AtImpact, ///< Whole until a hit reaches its damage threshold, then cut fine at the hit, coarse away
    };

    struct DestructibleData
    {
        REFLECT()

        PROPERTY( DisplayName( "Fracture Source" ), Category( "Destructible" ), Summary,
                  Tooltip( "Baked: the .dfrac in Rest Collection. On Load: the entity's static mesh is cut in the "
                           "background when Play starts, with a seed of its own (identical objects break "
                           "differently; no file). At Impact: the object stays whole until a hit reaches its "
                           "level-1 damage threshold, then is cut around the hit point (small pieces there, large "
                           "ones away). Both run-time sources leave the object whole for the frame or two the cut "
                           "takes." ) )
        FractureSource Source = FractureSource::Baked;

        PROPERTY( DisplayName( "Rest Collection" ), Category( "Destructible" ), Summary, Asset<FractureAsset>,
                  Tooltip( "The baked fracture this object is (UE RestCollection) — drag a .dfrac from the "
                           "Content Browser. Read when Fracture Source is Baked; an empty slot is refused at Play "
                           "by name, not simulated as nothing." ) )
        Assets::AssetHandle Fracture;

        PROPERTY( DisplayName( "Sites" ), Category( "Runtime Fracture" ), Range( 1, 128 ),
                  Tooltip( "On Load: the Voronoi sites the static mesh is cut with (UE Uniform Voronoi), about "
                           "one piece each." ) )
        int32_t RuntimeSites = 12;

        PROPERTY( DisplayName( "Impact Radius" ), Category( "Runtime Fracture" ), Range( 1.0f, 1000.0f ),
                  Units( "cm" ),
                  Tooltip( "At Impact: how far from the hit the shells of sites reach (UE Radial Radius); "
                           "beyond it the pieces are as large as the outer shell's cells." ) )
        float ImpactRadius = 50.0f;

        PROPERTY( DisplayName( "Impact Shells" ), Category( "Runtime Fracture" ), Range( 1, 8 ),
                  Tooltip( "At Impact: shells of sites around the hit (UE Radial Steps); each shell's cells are "
                           "larger than the one inside it." ) )
        int32_t ImpactShells = 3;

        PROPERTY( DisplayName( "Impact Sites Per Shell" ), Category( "Runtime Fracture" ), Range( 1, 16 ),
                  Tooltip( "At Impact: sites on each shell (UE Angular Steps)." ) )
        int32_t ImpactSitesPerShell = 8;

        PROPERTY( DisplayName( "Interior Material" ), Category( "Runtime Fracture" ), Asset<MaterialAsset>,
                  Tooltip( "On Load and At Impact: what the cut faces draw with (the Fracture mode's Internal "
                           "Material for a baked .dfrac). Empty, they draw with the default surface." ) )
        Assets::AssetHandle InteriorMaterial;

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

        // UE UGeometryCollectionComponent::OnChaosBreakEvent / OnChaosPhysicsCollision / OnChaosRemovalEvent
        // (GeometryCollectionComponent.h): news of THIS destructible. Destruction publishes the facts
        // (DestructionEventQueue, gated by the Notify flags below); the Gameplay Framework delivers them here
        // (ComponentEventSystem) and to the entity's scripts' functions of the same names (ScriptSystem). What
        // answers them (dust, sound, score) is the game's.
        EVENT( Category( "Destructible" ),
               Tooltip( "A group of pieces broke off (UE OnChaosBreakEvent): its first fracture node, its world "
                        "centre of mass in cm, its velocity in cm/s, its mass in kg and how many pieces." ) )
        using OnBreak = void( int32_t node, glm::vec3 position, glm::vec3 velocity, float massKg,
                              int32_t pieceCount );

        EVENT(
             Category( "Destructible" ),
             Tooltip( "A contact on the pieces at or above Collision Event Min Impulse (UE "
                      "OnChaosPhysicsCollision): the unit touched, the world contact point in cm, the normal out "
                      "of the piece, the normal impulse in kg*cm/s, the piece's velocity in cm/s and mass in "
                      "kg." ) )
        using OnCollision = void( int32_t node, glm::vec3 position, glm::vec3 normal, float impulse,
                                  glm::vec3 velocity, float massKg );

        EVENT( Category( "Destructible" ),
               Tooltip( "A piece left the simulation, asleep long enough or killed by a field (UE "
                        "OnChaosRemovalEvent): its fracture node, its world centre of mass in cm, its mass in kg "
                        "and how many pieces." ) )
        using OnRemoved = void( int32_t node, glm::vec3 position, float massKg, int32_t pieceCount );

        PROPERTY( DisplayName( "Notify Breaks" ), Category( "Events" ),
                  Tooltip( "Every group of pieces that breaks off raises On Break for this entity's listeners "
                           "and scripts (UE bNotifyBreaks). Off, breaks are nobody's news." ) )
        bool NotifyBreaks = false;

        PROPERTY( DisplayName( "Notify Collisions" ), Category( "Events" ),
                  Tooltip( "Every contact on the pieces at or above Collision Event Min Impulse raises On "
                           "Collision (UE bNotifyCollisions). Off, contacts are not even recorded." ) )
        bool NotifyCollisions = false;

        PROPERTY( DisplayName( "Notify Removals" ), Category( "Events" ),
                  Tooltip( "Every piece leaving the simulation (asleep long enough, or a kill field) raises On "
                           "Removed (UE bNotifyRemovals)." ) )
        bool NotifyRemovals = false;

        PROPERTY( DisplayName( "Collision Event Min Impulse" ), Category( "Events" ), Range( 0.0f, 10000000.0f ),
                  Units( "kg*cm/s" ),
                  Tooltip( "A contact's normal impulse must reach this to become a collision event (the Chaos "
                           "collision-event filter's MinImpulse)." ) )
        float CollisionEventMinImpulse = 1000.0f;
    };

    struct DestructibleComponent
    {
        COMPONENT( Key( "Destructible" ), Block( Data ), Run( SkyAndAtmosphere ) )
        DestructibleData Data;

        // Transient: the fracture the DestructionWorld simulates for this entity when it was made at run time
        // (Source On Load / At Impact; the whole first, then the cut, At Impact). Written by DestructibleLifetime,
        // drawn by the piece draw (FracturePieceDraw); null for Baked and while not simulated.
        std::shared_ptr<const Destruction::FractureData> RuntimeFracture;
        // Transient: the object in the scene's DestructionWorld (Destruction::DestructibleHandle; created on
        // Play, gone on Stop). Not reflected/serialized.
        uint32_t RuntimeObject = 0xFFFFFFFFu;
        // Transient: per node of the fracture, where its body put it after the last physics step (fracture space
        // -> world; empty = its body is gone). Written by DestructibleLifetime::WritePoses, read by the piece
        // draw (Destruction/FracturePieces.hpp); empty while not simulated, so the pieces draw at rest.
        std::vector<std::optional<glm::mat4>> SimulatedNodeWorld;
    };
} // namespace Desert::ECS
