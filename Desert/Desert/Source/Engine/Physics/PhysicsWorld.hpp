#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Engine/Physics/CollisionProfiles.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <limits>
#include <span>

namespace Desert::Physics
{
    enum class BodyType
    {
        Static,    // never moves (ground, walls)
        Dynamic,   // simulated (falls, collides)
        Kinematic, // moved by code, pushes dynamics (platforms, the player controller later)
    };

    enum class ShapeType
    {
        Box,
        Sphere,
        Capsule,
        // UE's "complex as simple": the render triangles themselves. Jolt's MeshShape has no volume, so it
        // cannot carry mass — static and kinematic bodies only; a dynamic one is refused by name.
        Mesh,
        // UE's "simple" convex: the hull of the mesh's points, for anything that moves. Jolt's hull builder
        // caps the hull at ConvexHullShape::cMaxPointsInHull (256) and simplifies past it on its own.
        ConvexHull,
    };

    // The body-local axis a capsule's cylinder runs along. Jolt's capsule is built along Y; any other axis is
    // Jolt's capsule rotated onto it.
    enum class CapsuleAxis
    {
        X,
        Y,
        Z,
    };

    struct BodyDesc
    {
        ShapeType Shape       = ShapeType::Box;
        glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f }; // Box
        float     Radius      = 0.5f;                 // Sphere / Capsule
        float     HalfHeight  = 0.5f;                 // Capsule (cylinder half-height, excl. caps)
        CapsuleAxis Axis        = CapsuleAxis::Y;       // Capsule
        glm::vec3   Center      = { 0.0f, 0.0f, 0.0f }; // Box / Sphere / Capsule: body-local offset of the shape
        // Mesh / ConvexHull: body-local points, scale already applied. Mesh also takes MeshIndices, three per
        // triangle; ConvexHull ignores them. Read during CreateBody only — the world keeps its own cooked copy.
        std::span<const glm::vec3> MeshPoints;
        std::span<const uint32_t>  MeshIndices;

        BodyType  Type        = BodyType::Dynamic;
        float     Mass        = 1.0f;  // dynamic only (<=0 => density-derived)
        float     Friction    = 0.5f;
        float     Restitution = 0.1f;

        glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
        glm::quat Rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );

        /// The body's profile in the world's register (PhysicsWorld::GetCollisionProfiles). kNoProfile is refused.
        CollisionProfileId Profile = kNoProfile;
    };

    // Opaque handle wrapping a JPH::BodyID (its index+sequence uint32). kInvalidBody == not created.
    using BodyHandle = uint32_t;
    constexpr BodyHandle kInvalidBody = 0xFFFFFFFFu;

    // ---- Character controller (Jolt CharacterVirtual: a kinematic capsule that walks slopes/steps, is
    // blocked by world geometry, and reports ground contact — the basis for the playable player). ----
    using CharacterHandle                   = uint32_t;
    constexpr CharacterHandle kInvalidCharacter = 0xFFFFFFFFu;

    struct CharacterDesc
    {
        float     Radius      = 0.3f; // capsule radius
        float     HalfHeight  = 0.6f; // capsule cylinder half-height (excl. the two hemisphere caps)
        glm::vec3 Position    = { 0.0f, 0.0f, 0.0f }; // capsule CENTER
        float     MaxSlopeDeg = 50.0f;                // steeper than this = wall (can't walk up)
        /// The capsule is blocked only by profiles its own answers Block (an Overlap pair passes through).
        CollisionProfileId Profile = kNoProfile;
    };

    // Thin engine-side wrapper over a Jolt PhysicsSystem. All Jolt headers stay inside the .cpp (PIMPL),
    // so the rest of the engine never sees Jolt — and Jolt's config defines only need to match within
    // this one translation unit + the Jolt lib.
    /**
     * @brief A static heightfield: a square grid of heights, two planar triangles per cell split on the
     * (x, z)-(x+1, z+1) diagonal — Jolt's split, and not negotiable (HeightFieldShape.cpp,
     * GetTriangleVertices).
     *
     * Sample (x, z) sits at Position + (x·SpacingCm, HeightsCm[z·SampleCount + x], z·SpacingCm).
     */
    struct HeightFieldDesc
    {
        glm::vec3              Position    = { 0.0f, 0.0f, 0.0f };
        uint32_t               SampleCount = 0u; ///< Per side; a multiple of kHeightFieldBlockSize, >= 2 blocks.
        float                  SpacingCm   = 100.0f;
        std::span<const float>
             HeightsCm; ///< SampleCount², row-major, X fastest; kHeightFieldNoCollision = a hole.
        float                  Friction = 0.5f;
        CollisionProfileId     Profile  = kNoProfile; ///< Refused when kNoProfile.
    };

    /// A height that is no height: every triangle touching such a sample has no collision (Jolt's
    /// HeightFieldShapeConstants::cNoCollisionValue, stored as cNoCollisionValue16 — a landscape hole).
    inline constexpr float kHeightFieldNoCollision = std::numeric_limits<float>::max();

    /// The heightfield's compression block. Jolt patches heights only in whole blocks, so an update's
    /// rectangle is widened to this alignment before it is handed over.
    inline constexpr uint32_t kHeightFieldBlockSize = 4u;

    /// What UpdateHeightField had to do.
    enum class HeightFieldUpdate
    {
        Patched, ///< The rectangle was re-quantised in place; nothing else about the body changed.
        Rebuilt, ///< A new height left the range the shape can encode: the shape was rebuilt, the body kept.
    };

    /**
     * @brief A body that is the union of convex parts (UE: a geometry-collection cluster, one rigid made of its
     * pieces' implicits). Each part is the hull of its points, in the body's own space — the frame every
     * piece of a fracture shares, so a piece's mesh draws with the body's transform as it is.
     *
     * Part i of the body is reported as ContactImpulse::Part i. One part is that part's hull alone (Jolt
     * collapses a one-part compound), and its contacts report Part 0.
     */
    struct CompoundBodyDesc
    {
        std::span<const std::span<const glm::vec3>> Parts; ///< Read during CreateCompoundBody only.

        BodyType Type        = BodyType::Dynamic;
        float    Mass        = 1.0f; ///< Dynamic only; kilograms, inertia from the parts at this mass.
        float    Friction    = 0.5f;
        float    Restitution = 0.1f;

        glm::vec3 Position        = { 0.0f, 0.0f, 0.0f };
        glm::quat Rotation        = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        glm::vec3 LinearVelocity  = { 0.0f, 0.0f, 0.0f }; ///< cm/s, at the body's centre of mass
        glm::vec3 AngularVelocity = { 0.0f, 0.0f, 0.0f }; ///< rad/s

        /// The body's contacts are measured every step (GetStepContactImpulses). Off for everything that
        /// does not read them: the estimate is a small solve per contact.
        bool ReportContactImpulses = false;

        CollisionProfileId Profile = kNoProfile; ///< Refused when kNoProfile.
    };

    /**
     * @brief One contact of the last fixed step that touches a body created with ReportContactImpulses: the
     * normal impulse it carries (Jolt EstimateCollisionResponse, summed over the manifold's points), in
     * kg·cm/s — the unit UE's damage thresholds are written in.
     */
    struct ContactImpulse
    {
        BodyHandle Body1   = kInvalidBody;
        BodyHandle Body2   = kInvalidBody;
        uint32_t   Part1   = 0u; ///< Index into Body1's CompoundBodyDesc::Parts; 0 for any other shape.
        uint32_t   Part2   = 0u;
        glm::vec3  Point   = { 0.0f, 0.0f, 0.0f }; ///< World, on Body1's surface
        glm::vec3  Normal  = { 0.0f, 1.0f, 0.0f }; ///< World, from Body1 towards Body2
        float      Impulse = 0.0f;
    };

    /// What a ContactEvent reports (UE OnComponentHit / OnComponentBeginOverlap / OnComponentEndOverlap).
    enum class ContactEventKind : uint8_t
    {
        Hit,          ///< Two bodies began a blocking (solved) contact
        BeginOverlap, ///< Two bodies began to overlap (a sensor contact: an Overlap pair, found and not solved)
        EndOverlap,   ///< The pair stopped overlapping, or one of its bodies left the world
    };

    /**
     * @brief One body-pair event of a fixed step, named by body (ChaosEventRelay / FCollisionNotifyInfo's
     * pattern: collected on the solver's threads, handed to the game thread after the step).
     *
     * A Hit is reported when either body's profile GeneratesHitEvents, once per contact begun (a touch kept
     * across steps is one Hit); Notify1 / Notify2 say whose profile asked. Begin / EndOverlap are reported when
     * both profiles GeneratesOverlapEvents, once per body pair however many shape pairs touch, and every Begin
     * is followed by exactly one End — also when a body is removed while overlapping.
     */
    struct ContactEvent
    {
        ContactEventKind Kind    = ContactEventKind::Hit;
        BodyHandle       Body1   = kInvalidBody;
        BodyHandle       Body2   = kInvalidBody;
        bool             Notify1 = false; ///< Body1's profile asked for this kind of event
        bool             Notify2 = false;
        glm::vec3        Point   = { 0.0f, 0.0f, 0.0f }; ///< World, on Body1's surface; zero for EndOverlap
        glm::vec3        Normal  = { 0.0f, 0.0f, 0.0f }; ///< World, from Body1 towards Body2; zero for EndOverlap
        float            Impulse = 0.0f;                 ///< Hit only: estimated normal impulse, kg·cm/s
    };

    struct RayHit
    {
        BodyHandle Body     = kInvalidBody;
        float      Distance = 0.0f; ///< Along the ray, in units of |direction|.
        glm::vec3  Point    = { 0.0f, 0.0f, 0.0f };
        glm::vec3  Normal   = { 0.0f, 1.0f, 0.0f };
    };

    /// The length of every step the world takes (UE async physics' fixed tick, 60 Hz).
    inline constexpr float kFixedStepSeconds = 1.0f / 60.0f;
    /// The most steps one PhysicsWorld::Step takes (UE MaxSubsteps, 6): a longer frame loses the rest.
    inline constexpr uint32_t kMaxStepsPerFrame = 6;

    class PhysicsWorld
    {
    public:
        PhysicsWorld();
        ~PhysicsWorld();

        PhysicsWorld( const PhysicsWorld& )            = delete;
        PhysicsWorld& operator=( const PhysicsWorld& ) = delete;

        // @p gravityCmPerS2 is the DOWNWARD magnitude in centimetres per second squared (Earth = 981), and
        // it has no default on purpose: the caller owns the value, and a default here is how the scene's
        // own setting came to be ignored in the first place. @p profiles decides which bodies collide (UE
        // collision channels and profiles): each profile is a pair of Jolt object layers (static, moving),
        // and a pair of bodies meets in the narrow phase unless their profiles' response is Ignore. An
        // Overlap pair's contacts are sensor contacts — found, never solved.
        bool Init( float gravityCmPerS2, CollisionProfiles profiles );

        /// The register the world was initialised with; a body's Profile is an id in it.
        [[nodiscard]] const CollisionProfiles& GetCollisionProfiles() const;
        void Shutdown();

        // Applies a new gravity to a running world. Called when the scene's setting changes so the knob is
        // honest while playing, instead of only at the next Play.
        void SetGravity( float gravityCmPerS2 );
        /// The DOWNWARD magnitude the world falls at, cm/s^2 (what Init / SetGravity were given).
        [[nodiscard]] float GetGravity() const;

        /**
         * @brief Banks @p dt seconds of frame time and advances the world by as many whole fixed steps
         * (kFixedStepSeconds) as the bank holds, at most kMaxStepsPerFrame; what a hitch leaves beyond that is
         * dropped, not caught up (UE MaxSubsteps). A frame shorter than a step may take none.
         *
         * The world only ever moves in steps of one length, so its state is a function of the step count and
         * the inputs, never of the frame rate: the premise of a server that owns the simulation (UE async
         * physics' fixed tick). Each step: the poses are kept for interpolation, the pre-step callback runs,
         * the held forces are applied, Jolt solves, the post-step callback runs.
         */
        void Step( float dt );

        /// Steps taken since Init. The world's clock: its time is GetStepCount() * kFixedStepSeconds, the same
        /// on every machine that took the same steps (the water's wave clock reads it).
        [[nodiscard]] uint64_t GetStepCount() const;
        /// GetStepCount() * kFixedStepSeconds, in double so an hour of play does not round the step away.
        [[nodiscard]] double GetSimulatedSeconds() const;
        /// How far the banked time is into the next step, in [0, 1): the weight the Interpolated getters put
        /// on the newest step's pose against the one before (UE async physics' result interpolation).
        [[nodiscard]] float GetInterpolationAlpha() const;

        /// Called before every fixed step inside Step, with the step's length, after the previous step's poses
        /// were kept and before the held forces are applied: where a fixed-rate producer (buoyancy, a character
        /// controller) reads the world and pushes into it. A force added here acts on THIS step only. One
        /// subscriber; an empty function unsubscribes.
        void SetPreStepCallback( std::function<void( float )> callback );

        /// Called after every fixed step inside Step, with that step's length: where a system that reacts to
        /// the solve (DestructionWorld) runs, at the solver's rate rather than the frame's. One subscriber;
        /// an empty function unsubscribes.
        void SetStepCallback( std::function<void( float )> callback );

        /// The contacts of the last fixed step on bodies that asked for them (ReportContactImpulses).
        [[nodiscard]] std::span<const ContactImpulse> GetStepContactImpulses() const;

        /// The contact events of every fixed step the last Step call took, step by step; within a step ordered
        /// by (Body1, Body2, Kind), so the order does not depend on which solver thread found a contact.
        /// Read on the game thread after Step; the next Step replaces them.
        [[nodiscard]] std::span<const ContactEvent> GetContactEvents() const;

        /// Refused by name: a Mesh on a dynamic body, a Mesh or ConvexHull without points, an index out of
        /// range, a shape Jolt cannot cook. Mesh and ConvexHull shapes are cooked once per content (the points,
        /// the indices, the kind) and shared by every body built from the same data.
        Common::ResultStr<BodyHandle> CreateBody( const BodyDesc& desc );

        /// Refused by name: no parts, a part without points, a hull Jolt cannot cook. Each part's hull is
        /// cooked once per content and shared, as a ConvexHull collider's is.
        Common::ResultStr<BodyHandle> CreateCompoundBody( const CompoundBodyDesc& desc );

        /// How many distinct Mesh / ConvexHull shapes the world has cooked — the measure of the shape cache.
        [[nodiscard]] uint32_t GetCookedShapeCount() const;
        void       RemoveBody( BodyHandle handle );

        /// A static heightfield body (NON_MOVING layer). Refuses a grid Jolt cannot build, naming the numbers.
        Common::ResultStr<BodyHandle> CreateHeightField( const HeightFieldDesc& desc );

        /**
         * @brief Brings the heightfield @p handle up to date with @p desc inside the sample rectangle
         * [x0, x1) × [z0, z1), leaving the body — its handle, its contacts' identity — in place.
         *
         * @p desc carries the WHOLE grid, as at creation: the rectangle is widened to block alignment, and
         * if a new height falls outside what the current shape can encode (Jolt would CLAMP it, a silent
         * cliff), the shape is rebuilt from the whole grid instead. Sleeping bodies over the rectangle are
         * woken so they fall onto — or out of — the new surface.
         */
        Common::ResultStr<HeightFieldUpdate> UpdateHeightField( BodyHandle handle, const HeightFieldDesc& desc,
                                                                uint32_t x0, uint32_t z0, uint32_t x1,
                                                                uint32_t z1 );

        /// The nearest body the ray meets within @p maxDistance, or nullopt. @p direction is normalised here.
        /// Bodies whose profile takes no part in queries (NoCollision, PhysicsOnly) are not found.
        [[nodiscard]] std::optional<RayHit> CastRay( const glm::vec3& origin, const glm::vec3& direction,
                                                     float maxDistance ) const;

        /// UE SweepSingleByChannel with a sphere: the first body a sphere of @p radius meets travelling from
        /// @p origin along @p direction (normalised here) within @p maxDistance, or nullopt. RayHit::Distance
        /// is how far the sphere's CENTRE travelled before touching; Point / Normal are the contact's. A sphere
        /// that already overlaps something at @p origin reports that body at distance 0.
        [[nodiscard]] std::optional<RayHit> CastSphere( const glm::vec3& origin, const glm::vec3& direction,
                                                        float radius, float maxDistance ) const;

        /// UE OverlapAnyTestByChannel with a capsule standing on Y: does any body intersect the capsule of
        /// @p radius and cylinder @p halfHeight (excluding the caps) centred at @p center? A character's inner
        /// body is never found (GetCharacterBody), so a character never overlaps itself here.
        [[nodiscard]] bool OverlapsCapsule( const glm::vec3& center, float radius, float halfHeight ) const;

        // Read simulated transform (body origin, not center-of-mass).
        glm::vec3 GetPosition( BodyHandle handle ) const;
        glm::quat GetRotation( BodyHandle handle ) const;
        /// The pose to DRAW: between the last two steps' poses by GetInterpolationAlpha, so a body moves
        /// smoothly at any frame rate while the world moves in fixed steps (one step behind the simulation).
        /// A body that did not move in the last step, or was teleported, answers its simulated pose.
        [[nodiscard]] glm::vec3 GetInterpolatedPosition( BodyHandle handle ) const;
        [[nodiscard]] glm::quat GetInterpolatedRotation( BodyHandle handle ) const;

        // Teleport / drive a body (use for Kinematic bodies or resetting on Play).
        void SetTransform( BodyHandle handle, const glm::vec3& position, const glm::quat& rotation );
        /// The pose a Kinematic body travels to (UE: a kinematic body follows its component). Every fixed step
        /// of the next Step moves it there with Jolt's MoveKinematic, the remaining distance shared evenly over
        /// the steps, so contacts and overlaps see it travel rather than teleport. Kept until replaced or the
        /// body is removed. Ignored for a handle that is not a kinematic body.
        void SetKinematicTarget( BodyHandle handle, const glm::vec3& position, const glm::quat& rotation );
        void SetLinearVelocity( BodyHandle handle, const glm::vec3& velocity );
        /// Moves the body to the pose and STOPS it (linear and angular velocity zero), waking it: a restored
        /// save, a respawn — the body is where the game put it, and the next step does not carry the old motion.
        void TeleportBody( BodyHandle handle, const glm::vec3& position, const glm::quat& rotation );
        /// Adds @p impulse (kg*cm/s) at the centre of mass and wakes the body; a static body ignores it.
        void                    AddImpulse( BodyHandle handle, const glm::vec3& impulse );

        // ---- Forces (UE AddForce / AddForceAtLocation / AddTorqueInRadians) ----
        // A force is a rate, so WHEN it is added decides how long it acts:
        //   - from the pre-step callback: on that one fixed step;
        //   - from anywhere else (the game frame): on EVERY fixed step of the next Step call, so a force held
        //     each frame gives the same momentum whatever the frame rate. When that Step takes no step (a frame
        //     shorter than a step), the forces wait for the next one, and the next frame's first force replaces
        //     them all rather than adding to them: the latest frame's set is the force, not the sum of frames.
        // A static or kinematic body, or one removed meanwhile, ignores them. Each wakes the body.

        /// @p force in kg*cm/s^2 at the centre of mass.
        void AddForce( BodyHandle handle, const glm::vec3& force );
        /// @p force in kg*cm/s^2 at the WORLD point @p point: the force plus its torque about the centre of mass.
        void AddForceAtPoint( BodyHandle handle, const glm::vec3& force, const glm::vec3& point );
        /// @p torque in kg*cm^2/s^2, world axes.
        void                    AddTorque( BodyHandle handle, const glm::vec3& torque );
        [[nodiscard]] glm::vec3 GetLinearVelocity( BodyHandle handle ) const;  ///< cm/s, at the centre of mass
        [[nodiscard]] glm::vec3 GetAngularVelocity( BodyHandle handle ) const; ///< rad/s
        /// The velocity of the body's material at the world point @p point (zero for a static body).
        [[nodiscard]] glm::vec3 GetPointVelocity( BodyHandle handle, const glm::vec3& point ) const;
        /// Jolt has not put the body to sleep. A static body is never active.
        [[nodiscard]] bool IsActive( BodyHandle handle ) const;

        // How many bodies / characters the world holds right now. The measure of "destroying an entity gave
        // its body back" — without it, a leak is only visible as a collision with something that is not there.
        [[nodiscard]] uint32_t GetBodyCount() const;
        [[nodiscard]] uint32_t GetCharacterCount() const;

        // ---- Character controller ----
        /// Refused by name when the desc carries no profile.
        Common::ResultStr<CharacterHandle> CreateCharacter( const CharacterDesc& desc );
        void            RemoveCharacter( CharacterHandle handle );
        // Set the character's velocity (incl. caller-integrated gravity/jump) and advance it by dt — Jolt
        // resolves collisions/slopes/steps. Called from the pre-step callback with the fixed step, so a
        // character moves at the world's rate, not the frame's.
        void            UpdateCharacter( CharacterHandle handle, const glm::vec3& velocity, float dt );
        glm::vec3       GetCharacterPosition( CharacterHandle handle ) const; // capsule center
        /// The kinematic capsule body that follows the character (Jolt's CharacterVirtual inner body), in the
        /// character's profile: what a trigger sees of it, and the body its contact events name. Queries
        /// (CastRay, CastSphere, OverlapsCapsule) never find it. kInvalidBody for no character.
        [[nodiscard]] BodyHandle GetCharacterBody( CharacterHandle handle ) const;
        /// The capsule centre to DRAW, interpolated as GetInterpolatedPosition is.
        [[nodiscard]] glm::vec3 GetInterpolatedCharacterPosition( CharacterHandle handle ) const;
        bool            IsCharacterOnGround( CharacterHandle handle ) const;
        void            SetCharacterPosition( CharacterHandle handle, const glm::vec3& position );
        /// Replaces the character's capsule (UE crouch: UCapsuleComponent::SetCapsuleSize). The centre stays
        /// where it is — the caller moves it so the feet stay put. Refused (false, shape unchanged) when the new
        /// capsule would penetrate the world: growing back under a low ceiling is the caller's overlap check
        /// to make first, and this is the guard that it was made.
        bool SetCharacterCapsule( CharacterHandle handle, float radius, float halfHeight );
        /// Moves the capsule centre to @p position and stops it (its velocity zero); see TeleportBody.
        void TeleportCharacter( CharacterHandle handle, const glm::vec3& position );

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
} // namespace Desert::Physics
