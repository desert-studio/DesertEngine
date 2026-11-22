#include "ReflectedComponents.hpp"

#include <Engine/Core/Serialize/ComponentRegistration.hpp>
#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <string>
#include <vector>

namespace Desert::ECS
{
    namespace
    {
        // The accessor half of every module's registered rows (Core/Serialize/ComponentRegistration.hpp): one
        // contiguous table, in the serializer's (key) order, so AllReflectedComponents can hand out a span.
        const std::vector<ReflectedComponent>& Rows()
        {
            static const std::vector<ReflectedComponent> rows = []
            {
                std::vector<ReflectedComponent> all;
                for ( const Core::Serialize::ModuleComponent& row : Core::Serialize::ModuleComponents() )
                    all.push_back( row.Component );
                return all;
            }();
            return rows;
        }
    } // namespace

    const Reflection::TypeInfo* ReflectedComponent::Type() const
    {
        return Reflection::ReflectionRegistry::Get().Find( std::string( TypeName ) );
    }

    std::span<const ReflectedComponent> AllReflectedComponents()
    {
        return Rows();
    }

    const ReflectedComponent* FindReflectedComponent( std::string_view name )
    {
        for ( const auto& row : Rows() )
            if ( row.Name == name )
                return &row;
        return nullptr;
    }
} // namespace Desert::ECS
