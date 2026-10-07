#include <Engine/ECS/System/DestructionFields.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>

#include <glm/gtc/matrix_transform.hpp>

namespace Desert::ECS
{
    Common::ResultStr<uint32_t> FireDestructionField( entt::registry& registry, entt::entity entity,
                                                      Destruction::DestructionWorld& world )
    {
        using namespace Destruction;
        if ( !registry.valid( entity ) )
            return Common::MakeError<uint32_t>( "the field entity does not exist" );

        const glm::mat4 pose   = Entity( entity, registry ).GetWorldTransform();
        const glm::vec3 centre = glm::vec3( pose[3] );
        uint32_t        acted  = 0u;
        bool            fired  = false;

        // UE ARadialFalloff + FRadialVector under one SumVector (the "master field" pairing of
        // FieldSystemActor blueprints): the length strains the piece, the direction pushes it away.
        if ( const auto* field = registry.try_get<RadialImpulseFieldComponent>( entity ) )
        {
            const RadialImpulseFieldData& data = field->Data;
            SumVector                     impulse;
            impulse.Scalar = RadialFalloff{
                 .Magnitude = data.Magnitude, .Radius = data.Radius, .Position = centre, .Falloff = data.Falloff };
            impulse.Left = RadialVector{ .Magnitude = 1.0f, .Position = centre };
            acted += world.ApplyField( FieldCommand{ .Type = FieldPhysicsType::Impulse, .Vector = impulse } );
            fired = true;
        }
        if ( const auto* field = registry.try_get<StrainFieldComponent>( entity ) )
        {
            const StrainFieldData& data = field->Data;
            acted += world.ApplyField( FieldCommand{ .Type   = FieldPhysicsType::ExternalStrain,
                                                     .Scalar = RadialFalloff{ .Magnitude = data.Magnitude,
                                                                              .Radius    = data.Radius,
                                                                              .Position  = centre,
                                                                              .Falloff   = data.Falloff } } );
            fired = true;
        }
        if ( const auto* field = registry.try_get<KillFieldComponent>( entity ) )
        {
            acted += world.ApplyField(
                 FieldCommand{ .Type   = FieldPhysicsType::Kill,
                               .Scalar = RadialIntMask{ .Radius = field->Data.Radius, .Position = centre } } );
            fired = true;
        }
        if ( const auto* field = registry.try_get<AnchorFieldComponent>( entity ) )
        {
            // FBoxFalloff's box is 100 cm: the entity's pose scaled to the authored extent.
            acted += world.ApplyField(
                 FieldCommand{ .Type   = FieldPhysicsType::Anchor,
                               .Scalar = BoxFalloff{ .Transform = glm::scale( pose, field->Data.Extent / 100.0f ),
                                                     .Falloff   = FieldFalloff::None } } );
            fired = true;
        }

        if ( !fired )
            return Common::MakeError<uint32_t>( "the entity has no field component (RadialImpulseField, "
                                                "StrainField, KillField or AnchorField)" );
        return Common::MakeSuccess( std::move( acted ) );
    }
} // namespace Desert::ECS
