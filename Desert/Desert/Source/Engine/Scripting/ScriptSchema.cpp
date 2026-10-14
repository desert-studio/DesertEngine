#include <Engine/Scripting/ScriptSchema.hpp>

#include <algorithm>

namespace Desert::Scripting
{
    const char* PropertyTypeName( PropertyType type )
    {
        switch ( type )
        {
            case PropertyType::Number:
                return "Number";
            case PropertyType::Bool:
                return "Bool";
            case PropertyType::String:
                return "String";
            case PropertyType::Int:
                return "Int";
            case PropertyType::Color:
                return "Color";
            case PropertyType::Vector:
                return "Vector";
            case PropertyType::Asset:
                return "Asset";
            case PropertyType::Entity:
                return "Entity";
            case PropertyType::Enum:
                return "Enum";
            case PropertyType::Array:
                return "Array";
        }
        return "Unknown";
    }

    const ScriptPropertyDecl* FindDecl( const ScriptSchema& schema, const std::string& name )
    {
        const auto found = std::find_if( schema.Properties.begin(), schema.Properties.end(),
                                         [&]( const ScriptPropertyDecl& d ) { return d.Name == name; } );
        return found == schema.Properties.end() ? nullptr : &*found;
    }
} // namespace Desert::Scripting
