#include <Water/Public/BuoyancySimulation.hpp>

#include <Engine/Physics/ColliderComponent.hpp>
#include <PhysicsCore/PhysicsWorld.hpp>

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Water
{
    namespace
    {
        // kg/m^3 -> kg/cm^3: the bodies' densities are typed per cubic metre (WaterBodyData::Density), the
        // pontoons' volumes are cubic centimetres.
        constexpr float kCubicMetrePerCubicCm = 1.0e-6f;

        [[nodiscard]] float SphereRadiusOfVolume( const float volume ) noexcept
        {
            return std::cbrt( 3.0f * volume / ( 4.0f * glm::pi<float>() ) );
        }

        [[nodiscard]] glm::vec3 AxisOf( const Physics::CapsuleAxis axis ) noexcept
        {
            switch ( axis )
            {
                case Physics::CapsuleAxis::X:
                    return { 1.0f, 0.0f, 0.0f };
                case Physics::CapsuleAxis::Z:
                    return { 0.0f, 0.0f, 1.0f };
                case Physics::CapsuleAxis::Y:
                    break;
            }
            return { 0.0f, 1.0f, 0.0f };
        }
    } // namespace

    float SubmergedSphereVolume( const float radius, const float bottomImmersion ) noexcept
    {
        const float h = glm::clamp( bottomImmersion, 0.0f, 2.0f * radius );
        return glm::pi<float>() * h * h * ( 3.0f * radius - h ) / 3.0f;
    }

    bool CutColliderPontoons( const ECS::ColliderData& collider, const int cellsPerAxis, std::vector<glm::vec4>& out,
                              std::string& error )
    {
        const int n = std::max( cellsPerAxis, 1 );
        switch ( collider.Shape )
        {
            case Physics::ShapeType::Box:
            {
                const glm::vec3 cell   = 2.0f * collider.HalfExtents / static_cast<float>( n );
                const float     radius = SphereRadiusOfVolume( cell.x * cell.y * cell.z );
                for ( int x = 0; x < n; ++x )
                    for ( int y = 0; y < n; ++y )
                        for ( int z = 0; z < n; ++z )
                        {
                            const glm::vec3 centre =
                                 collider.Center - collider.HalfExtents +
                                 cell * ( glm::vec3( static_cast<float>( x ), static_cast<float>( y ),
                                                     static_cast<float>( z ) ) +
                                          0.5f );
                            out.emplace_back( centre, radius );
                        }
                return true;
            }
            case Physics::ShapeType::Sphere:
                out.emplace_back( collider.Center, collider.Radius );
                return true;
            case Physics::ShapeType::Capsule:
            {
                const glm::vec3 axis   = AxisOf( collider.Axis );
                const float     slice  = 2.0f * collider.HalfHeight / static_cast<float>( n );
                const float     radius = SphereRadiusOfVolume( glm::pi<float>() * collider.Radius * collider.Radius *
                                                               slice );
                for ( int i = 0; i < n; ++i )
                    out.emplace_back( collider.Center +
                                           axis * ( -collider.HalfHeight + slice * ( static_cast<float>( i ) + 0.5f ) ),
                                      radius );
                // The end caps: a hemisphere each, its centroid 3R/8 beyond the cylinder's end.
                const float capRadius = collider.Radius * std::cbrt( 0.5f );
                const float capOffset = collider.HalfHeight + 0.375f * collider.Radius;
                out.emplace_back( collider.Center + axis * capOffset, capRadius );
                out.emplace_back( collider.Center - axis * capOffset, capRadius );
                return true;
            }
            case Physics::ShapeType::Mesh:
                error = "a Mesh collider has no volume to cut into pontoons; set Pontoon Source to Manual";
                return false;
            case Physics::ShapeType::ConvexHull:
                break;
        }
        error = "a ConvexHull collider is not cut into pontoons; set Pontoon Source to Manual";
        return false;
    }

    float ApplyBuoyancy( Physics::PhysicsWorld& world, const Physics::BodyHandle body,
                         const std::span<const glm::vec4> pontoons, const WaterSubsystem& water )
    {
        const glm::vec3 position = world.GetPosition( body );
        const glm::quat rotation = world.GetRotation( body );
        const float     gravity  = world.GetGravity();
        const auto      bodies   = water.Bodies();

        float total     = 0.0f;
        float submerged = 0.0f;
        for ( const glm::vec4& pontoon : pontoons )
        {
            const float radius = pontoon.w;
            const float volume = SubmergedSphereVolume( radius, 2.0f * radius );
            total += volume;
            const glm::vec3 centre = position + rotation * glm::vec3( pontoon );
            // Asked at the pontoon's LOWEST point: the query answers only for a point in the water, and a pontoon
            // is in the water as soon as its bottom is; the surface over the bottom is the surface over the centre.
            const glm::vec3                    bottom = centre - glm::vec3( 0.0f, radius, 0.0f );
            const std::optional<WaterQueryHit> hit    = water.Query( bottom );
            if ( !hit )
                continue;
            const float displaced = SubmergedSphereVolume( radius, hit->Result.ImmersionDepth );
            if ( displaced <= 0.0f )
                continue;
            submerged += displaced;

            const WaterBodyState& waterBody = bodies[hit->Body];
            // The mass of the water the pontoon pushes aside: its weight is the lift, and it is the mass the
            // water's drag pulls to the water's own velocity.
            const float     displacedKg = waterBody.Density * kCubicMetrePerCubicCm * displaced;
            const glm::vec3 lift( 0.0f, displacedKg * gravity, 0.0f );
            const glm::vec3 relative = world.GetPointVelocity( body, centre ) - hit->Result.Velocity;
            const glm::vec3 drag     = -waterBody.Drag * displacedKg * relative;
            world.AddForceAtPoint( body, lift + drag, centre );
        }
        return total > 0.0f ? submerged / total : 0.0f;
    }
} // namespace Desert::Water
