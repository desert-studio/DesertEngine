#pragma once

// BUOYANCY (UE: the Water plugin's UBuoyancyComponent, Public/BuoyancyComponent.h + BuoyancyComponentSimulation.h).
// A rigid body that floats: its PONTOONS - spheres fixed in the body - each ask the water query
// (Water::WaterSubsystem, WaterBodyQuery.hpp) how deep they are under the surface the waves raise, and each pushes
// up with the weight of the water its submerged part displaces (Archimedes: rho * g * V, rho the DENSITY of the
// water body it is in, WaterBodyData::Density) and is held back by that water's DRAG towards the water's own
// velocity (a river's current, WaterBodyData::Drag). The forces act at the pontoons, so a body tilts, rights itself
// and is carried by a current; they are added in the physics world's fixed pre-step (PhysicsWorld::
// SetPreStepCallback via IPhysicsSceneFeature::PreStep), at the solver's rate, never the frame's.
//
// Where UE differs: UE's pontoon force is a tuned BuoyancyCoefficient times an immersion ramp, its damping a
// separate per-component BuoyancyDamp. Here the force is the physical one, so a body of density 600 floats 60 %
// deep in water of 1000 with no tuning, and the drag is the water's (one value per water body), not the body's.
//
// The pontoons come either from the body's own collider (Pontoon Source = Collider: the collider's volume cut into
// cells, one sphere of the cell's volume at each cell centre, so the pontoons displace what the body displaces) or
// from the list typed in (Manual, UE's Pontoons array). A collider buoyancy cannot cut (a mesh or a convex hull) is
// refused by name; such a body names its pontoons by hand.

#include <CoreReflection/ReflectionMacros.hpp>

#include <glm/vec4.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Desert::ECS
{
    enum class BuoyancyPontoonSource : uint8_t
    {
        Collider, ///< cut from the entity's ColliderComponent (box, sphere, capsule)
        Manual,   ///< the Pontoons list
    };

    struct BuoyancyData
    {
        REFLECT()

        PROPERTY( DisplayName( "Pontoon Source" ), Category( "Buoyancy" ), Summary,
                  Tooltip( "Collider: the pontoons are cut from this entity's collider, so they displace what the "
                           "body displaces. Manual: the Pontoons list below (UE BuoyancyData.Pontoons)." ) )
        BuoyancyPontoonSource PontoonSource = BuoyancyPontoonSource::Collider;

        PROPERTY( DisplayName( "Collider Cells Per Axis" ), Category( "Buoyancy" ), Range( 1, 8 ),
                  EditCondition( "PontoonSource == Collider" ),
                  Tooltip( "How finely the collider is cut along each axis: N*N*N pontoons for a box. More cells "
                           "follow the waves' shape under the body more closely and cost one water query each." ) )
        int32_t ColliderCellsPerAxis = 3;

        // x, y, z, radius per pontoon, flat (the reflection serializes a vector of scalars, as
        // WaterBodyData::RiverSpline). Body-local, cm.
        PROPERTY( DisplayName( "Pontoons" ), Category( "Buoyancy" ), Advanced, Length,
                  EditCondition( "PontoonSource == Manual" ),
                  Tooltip( "x y z radius per pontoon, body-local, cm (UE FSphericalPontoon RelativeLocation and "
                           "Radius). Each sphere pushes up with the weight of the water its submerged part "
                           "displaces." ) )
        std::vector<float> Pontoons;
    };

    /// @p data's manual Pontoons as (x, y, z, radius) (a view of the flat floats; a trailing partial four is
    /// ignored).
    [[nodiscard]] inline std::span<const glm::vec4> ManualPontoonsOf( const BuoyancyData& data ) noexcept
    {
        static_assert( sizeof( glm::vec4 ) == 4 * sizeof( float ) );
        return { reinterpret_cast<const glm::vec4*>( data.Pontoons.data() ), data.Pontoons.size() / 4 };
    }

    struct BuoyancyComponent
    {
        COMPONENT( Key( "Buoyancy" ), Block( Data ), Run( ActorsAndUI ) )
        BuoyancyData Data;

        // Transient: the submerged share of the pontoons' volume at the last fixed step, [0, 1] (UE
        // IsInWater / the pontoons' bIsInWater), written by the water's physics feature after the steps. Not
        // reflected / serialized.
        float SubmergedShare = 0.0f;
    };
} // namespace Desert::ECS
