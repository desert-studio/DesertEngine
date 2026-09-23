#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>

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
    };

    struct BodyDesc
    {
        ShapeType Shape       = ShapeType::Box;
        glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f }; // Box
        float     Radius      = 0.5f;                 // Sphere / Capsule
        float     HalfHeight  = 0.5f;                 // Capsule (cylinder half-height, excl. caps)

        BodyType  Type        = BodyType::Dynamic;
        float     Mass        = 1.0f;  // dynamic only (<=0 => density-derived)
        float     Friction    = 0.5f;
        float     Restitution = 0.1f;

        glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
        glm::quat Rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
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
    };

    // Thin engine-side wrapper over a Jolt PhysicsSystem. All Jolt headers stay inside the .cpp (PIMPL),
    // so the rest of the engine never sees Jolt — and Jolt's config defines only need to match within
    // this one translation unit + the Jolt lib.
    class PhysicsWorld
    {
    public:
        PhysicsWorld();
        ~PhysicsWorld();

        PhysicsWorld( const PhysicsWorld& )            = delete;
        PhysicsWorld& operator=( const PhysicsWorld& ) = delete;

        // @p gravityCmPerS2 is the DOWNWARD magnitude in centimetres per second squared (Earth = 981), and
        // it has no default on purpose: the caller owns the value, and a default here is how the scene's
        // own setting came to be ignored in the first place.
        bool Init( float gravityCmPerS2 );
        void Shutdown();

        // Applies a new gravity to a running world. Called when the scene's setting changes so the knob is
        // honest while playing, instead of only at the next Play.
        void SetGravity( float gravityCmPerS2 );

        // Advance the simulation by dt seconds (fixed-step accumulated internally).
        void Step( float dt );

        BodyHandle CreateBody( const BodyDesc& desc );
        void       RemoveBody( BodyHandle handle );

        // Read simulated transform (body origin, not center-of-mass).
        glm::vec3 GetPosition( BodyHandle handle ) const;
        glm::quat GetRotation( BodyHandle handle ) const;

        // Teleport / drive a body (use for Kinematic bodies or resetting on Play).
        void SetTransform( BodyHandle handle, const glm::vec3& position, const glm::quat& rotation );
        void SetLinearVelocity( BodyHandle handle, const glm::vec3& velocity );

        // How many bodies / characters the world holds right now. The measure of "destroying an entity gave
        // its body back" — without it, a leak is only visible as a collision with something that is not there.
        [[nodiscard]] uint32_t GetBodyCount() const;
        [[nodiscard]] uint32_t GetCharacterCount() const;

        // ---- Character controller ----
        CharacterHandle CreateCharacter( const CharacterDesc& desc );
        void            RemoveCharacter( CharacterHandle handle );
        // Set the character's velocity (incl. caller-integrated gravity/jump) and advance it by dt — Jolt
        // resolves collisions/slopes/steps. Call once per frame, AFTER Step().
        void            UpdateCharacter( CharacterHandle handle, const glm::vec3& velocity, float dt );
        glm::vec3       GetCharacterPosition( CharacterHandle handle ) const; // capsule center
        bool            IsCharacterOnGround( CharacterHandle handle ) const;
        void            SetCharacterPosition( CharacterHandle handle, const glm::vec3& position );

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
        float                 m_Accumulator = 0.0f;
    };
} // namespace Desert::Physics
