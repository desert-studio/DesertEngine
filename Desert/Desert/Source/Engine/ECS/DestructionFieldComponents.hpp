#pragma once

// FIELDS PLACED IN THE SCENE (UE: AFieldSystemActor with a UFieldSystemComponent). Each component is one field
// at its entity's world pose; ECS::FireDestructionField (ECS/System/DestructionFields.hpp) builds it into a
// Destruction::FieldCommand and fires it once, at that instant, into the scene's DestructionWorld. Nothing here
// acts on its own every frame: the field is an event, as UE's ApplyPhysicsField is.
//
// Units: centimetres, kilograms, seconds.

#include <Engine/Destruction/DestructionField.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>

#include <glm/vec3.hpp>

namespace Desert::ECS
{
    /// UE RadialFalloff * RadialVector on LinearForce + ExternalClusterStrain: pushes pieces away from the
    /// entity and strains them by the impulse's length.
    struct RadialImpulseFieldData
    {
        REFLECT()

        PROPERTY( DisplayName( "Magnitude" ), Category( "Field" ), Summary, Range( 0.0f, 1.0e8f ),
                  Units( "kg*cm/s" ),
                  Tooltip( "The impulse at the centre; the falloff takes it to zero at the radius. Its length "
                           "is also the strain a piece inside reads, against the piece's Damage Threshold." ) )
        float Magnitude = 1.0e5f;

        PROPERTY( DisplayName( "Radius" ), Category( "Field" ), Length, Range( 0.0f, 1.0e5f ),
                  Tooltip( "Pieces whose centre of mass is farther than this are not touched." ) )
        float Radius = 200.0f;

        PROPERTY( DisplayName( "Falloff" ), Category( "Field" ),
                  Tooltip( "How the magnitude goes from the centre to the radius (UE Falloff Type)." ) )
        Destruction::FieldFalloff Falloff = Destruction::FieldFalloff::Linear;
    };

    struct RadialImpulseFieldComponent
    {
        COMPONENT( Key( "RadialImpulseField" ), Block( Data ), Run( SkyAndAtmosphere ) )
        RadialImpulseFieldData Data;
    };

    /// UE RadialFalloff on ExternalClusterStrain: breaks pieces without a contact and without pushing them.
    struct StrainFieldData
    {
        REFLECT()

        PROPERTY( DisplayName( "Magnitude" ), Category( "Field" ), Summary, Range( 0.0f, 1.0e8f ),
                  Tooltip( "The strain at the centre; a piece breaks off where the strain it reads reaches its "
                           "Damage Threshold." ) )
        float Magnitude = 1.0e5f;

        PROPERTY( DisplayName( "Radius" ), Category( "Field" ), Length, Range( 0.0f, 1.0e5f ) )
        float Radius = 200.0f;

        PROPERTY( DisplayName( "Falloff" ), Category( "Field" ),
                  Tooltip( "How the magnitude goes from the centre to the radius (UE Falloff Type)." ) )
        Destruction::FieldFalloff Falloff = Destruction::FieldFalloff::Linear;
    };

    struct StrainFieldComponent
    {
        COMPONENT( Key( "StrainField" ), Block( Data ), Run( SkyAndAtmosphere ) )
        StrainFieldData Data;
    };

    /// UE RadialIntMask on Kill: every body whose centre of mass is inside leaves the simulation.
    struct KillFieldData
    {
        REFLECT()

        PROPERTY( DisplayName( "Radius" ), Category( "Field" ), Summary, Length, Range( 0.0f, 1.0e5f ) )
        float Radius = 200.0f;
    };

    struct KillFieldComponent
    {
        COMPONENT( Key( "KillField" ), Block( Data ), Run( SkyAndAtmosphere ) )
        KillFieldData Data;
    };

    /// UE BoxFalloff as an anchor: every leaf whose centre of mass is inside the box stays where it is from now
    /// on, and the body holding it turns static.
    struct AnchorFieldData
    {
        REFLECT()

        PROPERTY( DisplayName( "Extent" ), Category( "Field" ), Summary, Length,
                  Tooltip( "The box's full size along the entity's own axes, before the entity's scale." ) )
        glm::vec3 Extent{ 100.0f, 100.0f, 100.0f };
    };

    struct AnchorFieldComponent
    {
        COMPONENT( Key( "AnchorField" ), Block( Data ), Run( SkyAndAtmosphere ) )
        AnchorFieldData Data;
    };
} // namespace Desert::ECS
