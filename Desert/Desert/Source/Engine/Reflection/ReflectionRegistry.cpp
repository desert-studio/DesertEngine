#include <Engine/Reflection/ReflectionRegistry.hpp>

namespace Desert::Reflection
{
    ReflectionRegistry& ReflectionRegistry::Get()
    {
        // Meyers singleton: safe even when called from other static initializers (the generated
        // registrars), because the local static is constructed on first use.
        static ReflectionRegistry s_Instance;
        return s_Instance;
    }

    const TypeInfo* ReflectionRegistry::Register( TypeInfo info )
    {
        // The key is read into a local BEFORE the value is moved. `insert_or_assign( info.Name,
        // std::move( info ) )` reads and consumes one object in one argument list, and C++ does not order
        // arguments — clang left to right, MSVC right to left, both conforming. It happens to be harmless
        // today only because insert_or_assign binds an rvalue reference and moves inside its own body; a
        // callee that took its value BY VALUE would make the same line construct the key from a moved-from
        // string on one compiler and not the other, with nothing in the diff to show it. The shape is what
        // Desert/Tests/Engine/ArgumentOrder refuses, not the outcome, because the outcome is invisible here.
        const std::string key = info.Name;
        auto [it, inserted]   = m_Types.insert_or_assign( key, std::move( info ) );
        return &it->second;
    }

    const TypeInfo* ReflectionRegistry::Find( const std::string& name ) const
    {
        auto it = m_Types.find( name );
        return it != m_Types.end() ? &it->second : nullptr;
    }

    void ReflectionRegistry::ResolveStructLinks()
    {
        for ( auto& [name, type] : m_Types )
        {
            for ( auto& field : type.Fields )
            {
                if ( field.Type == FieldType::Struct && field.StructType == nullptr )
                {
                    field.StructType = Find( field.TypeName );
                    if ( field.StructType == nullptr )
                    {
                        // Registry keys are short names; the field may be spelled qualified (Ns::Type).
                        const auto pos = field.TypeName.rfind( "::" );
                        if ( pos != std::string::npos )
                            field.StructType = Find( field.TypeName.substr( pos + 2 ) );
                    }
                }
            }
        }
    }
} // namespace Desert::Reflection
