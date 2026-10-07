#pragma once

// THE DESTRUCTION WORLD: baked fractures (FractureFormat.hpp) simulated on Jolt — UE's rigid clustering
// (Chaos/PBDRigidClustering.cpp) on our PhysicsWorld, one per scene, running after every fixed physics step.
//
// A BODY is a group of sibling nodes of the hierarchy simulated as one rigid: a compound of their leaves'
// hulls, in the fracture's own space, so a piece draws with its body's transform as it is. At spawn the one
// body is the root. The BREAKABLE UNITS of a body are its member's children when it has one member, and its
// members when it has several (UE: a cluster's children, an internal cluster's children).
//
// STRAIN (port of ComputeStrainFromCollision :2226-2428 and ReleaseClusterParticlesImpl :1074-1179): each
// step, the normal impulse of every contact on a body is added to the unit holding the part touched (the
// shape-index mapping of UE's cluster unions, :2311, exact instead of the proximity box :2384). A unit whose
// applied impulse reaches its internal strain — its level's damage threshold (SetInternalStrain :1049), a
// threshold of zero or less breaks at once (:1730) — is released as its own body. The units left behind
// regroup by connectivity, each connected set a body of its own (UE's bCreateNewClusters); a set of one is
// that unit's own body. Applied impulses reset every step (ResetCollisionImpulseArray).
//
// VELOCITY (our own; the clone has none): a new body starts with the velocity the old one had at the new
// body's centre of mass, and the old body's angular velocity — the material keeps moving as it moved.
//
// ANCHOR (our own; UE's anchor field makes particles kinematic): an anchored node and every leaf below it
// never move. A body holding an anchored leaf is static; it still takes strain and breaks, and only its
// unanchored units fly.
//
// REMOVE ON SLEEP (UE GeometryCollectionObject.h:794 bRemoveOnMaxSleep, MaximumSleepTime, Slow-Moving as
// sleeping): a body made by a break that has slept — or crept below the slow-moving speed — for its sleep
// time is removed; the time is drawn in [min, max] per body, deterministically from its first member.
//
// Every number is in centimetres, kilograms and seconds.

#include <Engine/Destruction/DestructionField.hpp>
#include <Engine/Destruction/FractureFormat.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Desert::Destruction
{
    struct DestructionSettings
    {
        float DensityKgPerCm3 = 0.0024f; ///< Mass per volume of the pieces; concrete by default
        float Friction        = 0.6f;
        float Restitution     = 0.1f;

        bool      RemoveOnSleep        = true;
        glm::vec2 MaxSleepTime         = { 5.0f, 10.0f }; ///< Seconds asleep before removal, drawn in [x, y]
        bool      SlowMovingAsSleeping = true;
        float     SlowMovingVelocityThreshold = 1.0f; ///< cm/s; slower than this counts as asleep
    };

    struct DestructibleDesc
    {
        glm::vec3            Position = { 0.0f, 0.0f, 0.0f }; ///< Where the fracture's origin is placed
        glm::quat            Rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        std::vector<int32_t> AnchoredNodes; ///< Each node, and every leaf below it, never moves
        /// UE component DamageThreshold: entry L replaces the bake's threshold of every level-L node; the
        /// levels past the list keep the bake's (UE ApplyAssetDefaults: the asset is the default)
        std::vector<float>   DamageThreshold;
        DestructionSettings  Settings;
    };

    using DestructibleHandle                                 = uint32_t;
    inline constexpr DestructibleHandle kInvalidDestructible = 0xFFFFFFFFu;

    enum class DestructionEventKind : uint8_t
    {
        Break,   ///< A unit left its body as a body of its own
        Removed, ///< A broken-off body slept long enough and left the simulation (one event per member)
    };

    struct DestructionEvent
    {
        DestructionEventKind Kind     = DestructionEventKind::Break;
        DestructibleHandle   Object   = kInvalidDestructible;
        int32_t              Node     = -1;
        glm::vec3            Position = { 0.0f, 0.0f, 0.0f }; ///< World centre of mass of the node
        glm::vec3            Velocity = { 0.0f, 0.0f, 0.0f }; ///< Its velocity, cm/s
    };

    class DestructionWorld
    {
    public:
        /// Subscribes to @p physics's fixed step (PhysicsWorld::SetStepCallback); @p physics outlives this.
        explicit DestructionWorld( Physics::PhysicsWorld& physics );
        ~DestructionWorld();

        DestructionWorld( const DestructionWorld& )            = delete;
        DestructionWorld& operator=( const DestructionWorld& ) = delete;

        /// Spawns @p data whole, as one body. Refused by name: no nodes, a hierarchy out of order, a leaf
        /// without a hull, an anchored node out of range, a density that is not positive, a sleep range
        /// upside down, a hull Jolt cannot cook.
        Common::ResultStr<DestructibleHandle> Add( std::shared_ptr<const FractureData> data,
                                                   const DestructibleDesc&             desc );
        /// Takes every body of @p object out of the physics world.
        void Remove( DestructibleHandle object );

        /// The body carrying @p node now: its own, its ancestor's or its group's. kInvalidBody once removed,
        /// and for a node above the bodies (the root after the first break).
        [[nodiscard]] Physics::BodyHandle GetNodeBody( DestructibleHandle object, int32_t node ) const;
        /// How many bodies @p object is in now.
        [[nodiscard]] uint32_t GetBodyCount( DestructibleHandle object ) const;

        /// Fires @p command once, now (DestructionField.hpp): strains break at once (UE MaxAppliedStrain =
        /// max(collision, external), PBDRigidClustering.cpp:1178), an impulse then pushes the bodies the break
        /// left, a kill removes bodies with Removed events, an anchor makes the bodies holding its leaves
        /// static. Returns how many bodies it acted on. The one entry for every trigger (component, Sequencer).
        uint32_t ApplyField( const FieldCommand& command );

        /// Breaks and removals since ClearEvents, in the order they happened.
        [[nodiscard]] std::span<const DestructionEvent> GetEvents() const
        {
            return m_Events;
        }
        void ClearEvents()
        {
            m_Events.clear();
        }

    private:
        struct NodeState
        {
            int32_t              Parent = -1;
            std::vector<int32_t> Children;
            std::vector<int32_t> Leaves;     // the node itself when it is a leaf
            std::vector<int32_t> Neighbours; // leaves only: the leaves whose hulls touch this one's
            float                InternalStrain   = 0.0f;
            float                CollisionImpulse = 0.0f;
            float                ExternalStrain   = 0.0f; // a field's strain, this instant only
            bool                 Anchored         = false; // the subtree holds an anchored leaf
            int32_t              Body             = -1;    // index into Object::Bodies
        };

        struct BodyState
        {
            Physics::BodyHandle  Handle = Physics::kInvalidBody;
            std::vector<int32_t> Members;  // siblings
            std::vector<int32_t> PartLeaf; // compound part -> leaf
            bool                 Static    = false;
            bool                 Broken    = false; // made by a break: may be removed on sleep
            float                SleepTime = 0.0f;
            float                MaxSleep  = 0.0f;
        };

        struct Object
        {
            std::shared_ptr<const FractureData> Data;
            DestructionSettings                 Settings;
            std::vector<NodeState>              Nodes;
            std::vector<BodyState>              Bodies; // a freed slot has an invalid Handle
        };

        struct BodyRef
        {
            uint32_t Object = 0;
            uint32_t Body   = 0;
        };

        void Advance( float dt );
        /// Breaks every unit of @p strained bodies whose applied strain reaches its internal strain.
        void Release( std::vector<BodyRef> strained );
        [[nodiscard]] glm::vec3 WorldPoint( const BodyState& body, const glm::dvec3& local ) const;
        void Break( uint32_t objectIndex, uint32_t bodyIndex, const std::vector<int32_t>& released );
        Common::ResultStr<uint32_t> SpawnBody( uint32_t objectIndex, std::vector<int32_t> members,
                                               const glm::vec3& position, const glm::quat& rotation,
                                               const glm::vec3& linear, const glm::vec3& angular, bool broken );
        void                        DestroyBody( uint32_t objectIndex, uint32_t bodyIndex );
        void                        AssignBody( Object& object, int32_t node, int32_t body );

        [[nodiscard]] std::vector<int32_t> UnitsOf( const Object& object, const BodyState& body ) const;
        [[nodiscard]] int32_t    UnitOfLeaf( const Object& object, const BodyState& body, int32_t leaf ) const;
        [[nodiscard]] glm::dvec3 CenterOfMass( const Object& object, const std::vector<int32_t>& members ) const;

        Physics::PhysicsWorld&                           m_Physics;
        std::vector<Object>                              m_Objects; // a removed object has no Data
        std::unordered_map<Physics::BodyHandle, BodyRef> m_BodyLookup;
        std::vector<DestructionEvent>                    m_Events;
    };
} // namespace Desert::Destruction
