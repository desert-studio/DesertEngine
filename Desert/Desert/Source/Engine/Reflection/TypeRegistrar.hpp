#pragma once

#include <Engine/Reflection/ReflectionRegistry.hpp>

#include <utility>

namespace Desert::Reflection
{
    // Fluent helper used by generated reflection code to assemble and register a TypeInfo.
    //
    //   TypeBuilder( "ExampleData", sizeof( ExampleData ) )
    //       .Field( { .Name = "Tint", .Type = FieldType::Vec4,
    //                 .Offset = offsetof( ExampleData, Tint ),
    //                 .Size = sizeof( glm::vec4 ), .Meta = { .Category = "Camera", .IsColor = true } } )
    //       .Register();
    class TypeBuilder
    {
    public:
        TypeBuilder( std::string name, std::size_t size )
        {
            m_Info.Name = std::move( name );
            m_Info.Size = size;
        }

        TypeBuilder& Field( FieldInfo field )
        {
            m_Info.Fields.push_back( std::move( field ) );
            return *this;
        }

        // Records a provider for a process-wide default-constructed instance of T (its member initializers =
        // the "factory defaults"), so the editor can offer reset-to-default. Called by generated code.
        template <typename T>
        TypeBuilder& WithDefault()
        {
            m_Info.GetDefaultInstance = []() -> const void*
            {
                static const T s_Default{};
                return &s_Default;
            };
            return *this;
        }

        const TypeInfo* Register()
        {
            return ReflectionRegistry::Get().Register( std::move( m_Info ) );
        }

    private:
        TypeInfo m_Info;
    };
} // namespace Desert::Reflection
