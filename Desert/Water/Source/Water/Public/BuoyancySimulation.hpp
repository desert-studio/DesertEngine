#pragma once

// The buoyancy step (UE BuoyancyComponentSimulation.h: FBuoyancyComponentSim::ComputeBuoyancy / ComputeDrag), for
// the bodies of ECS::BuoyancyComponent (BuoyancyComponent.hpp): where their pontoons come from and the forces each
// pontoon takes in the water it is in. Called by the water's physics feature (WaterModule.cpp) in the world's
// fixed pre-step.

#include <Water/Public/WaterBodyQuery.hpp>

#include <PhysicsCore/PhysicsTypes.hpp>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <span>
#include <string>
#include <vector>

namespace Desert::Physics
{
    class PhysicsWorld;
}

namespace Desert::ECS
{
    struct ColliderData;
}

namespace Desert::Water
{
    /// The volume, cm^3, of the part of a sphere of @p radius under a level @p bottomImmersion above its lowest
    /// point (the spherical cap pi h^2 (3r - h) / 3, h clamped to [0, 2r]): 0 out of the water, the sphere's
    /// whole volume once the level is over its top.
    [[nodiscard]] float SubmergedSphereVolume( float radius, float bottomImmersion ) noexcept;

    /// @p collider cut into pontoons (x, y, z, radius; body-local, cm) appended to @p out: a box into
    /// @p cellsPerAxis^3 cells, a capsule into @p cellsPerAxis slices of its cylinder and its two end caps, a
    /// sphere into itself; each pontoon a sphere of its piece's volume at the piece's centroid, so together they
    /// displace the collider's volume. A mesh or a convex hull has no cut: false, @p error names the shape.
    [[nodiscard]] bool CutColliderPontoons( const ECS::ColliderData& collider, int cellsPerAxis,
                                            std::vector<glm::vec4>& out, std::string& error );

    /// A pontoon's last answer from a surface that was ready, for the water body that gave it alone: what it floats
    /// on while that body's surface is not (its wave set is being read, or its ocean's first read-back has not
    /// landed), instead of dropping to the still plane.
    struct PontoonHold
    {
        bool      Valid    = false;
        uint32_t  Body     = 0;    // WaterBodyState::Id
        float     SurfaceY = 0.0f; // cm
        float     Density  = 0.0f; // the water's, kg/m^3
        float     Drag     = 0.0f; // 1/s
        glm::vec3 Velocity{ 0.0f };
    };

    /// Adds to @p world the forces the @p pontoons of @p body take in the water this step (call from the
    /// pre-step: a force added there acts on that step only), each at its pontoon's centre, kg*cm/s^2:
    ///  - up, the weight of the water its submerged part displaces (Archimedes), rho the Density of the water body
    ///    the pontoon is in (WaterBodyState::Density), g the world's gravity;
    ///  - that water's drag towards its own velocity (a river's current): -Drag * displaced mass * (v_point -
    ///    v_water).
    /// @p holds: one per pontoon, kept by the caller across steps; a ready answer fills it, an unready one reads it.
    /// Returns the submerged share of the pontoons' volume, [0, 1].
    float ApplyBuoyancy( Physics::PhysicsWorld& world, Physics::BodyHandle body, std::span<const glm::vec4> pontoons,
                         std::span<PontoonHold> holds, const WaterSubsystem& water );
} // namespace Desert::Water
