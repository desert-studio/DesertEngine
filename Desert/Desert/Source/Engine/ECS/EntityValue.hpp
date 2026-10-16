#pragma once

// An ECS::Entity as a reflected function's parameter or result: it travels as Reflection::Value::EntityRef (its
// id and its registry), so a script language hands the function the entity object it holds and gets one back.
// Included by every header whose FUNCTION(...) takes or returns an ECS::Entity.

#include <Engine/ECS/Entity.hpp>
#include <Engine/Reflection/FunctionThunk.hpp>

namespace Desert::Reflection
{
    template <>
    struct ValueTraits<ECS::Entity>
    {
        static constexpr FieldType Kind = FieldType::Entity;
        static ECS::Entity         From( const Value& v )
        {
            const Value::EntityRef& ref = *v.Get<Value::EntityRef>();
            if ( ref.World == nullptr )
                return {};
            return ECS::Entity( static_cast<entt::entity>( ref.Id ), *static_cast<entt::registry*>( ref.World ) );
        }
        static Value To( const ECS::Entity& entity )
        {
            if ( !entity )
                return Value::Entity( {} );
            return Value::Entity( { entity.GetRegistry(), static_cast<std::uint32_t>( entity.GetHandle() ) } );
        }
    };
} // namespace Desert::Reflection
