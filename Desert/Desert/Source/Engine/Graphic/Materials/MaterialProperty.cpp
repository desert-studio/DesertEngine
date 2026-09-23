#include "MaterialProperty.hpp"

#include <string>

namespace Desert::Graphic
{
    void MaterialPropertySet::SetProperty( const std::string& name, const MaterialPropertyValue& value,
                                           MaterialPropertyType type )
    {
        auto it = m_Properties.find( name );
        if ( it != m_Properties.end() )
        {
            it->second.Value         = value;
            it->second.Type          = type;
            it->second.bIsOverridden = true;
        }
        else
        {
            MaterialProperty prop;
            prop.Name          = name;
            prop.Type          = type;
            prop.Value         = value;
            prop.DefaultValue  = value;
            prop.bIsOverridden = true;
            m_Properties[name] = prop;
        }
        m_bIsDirty = true;
    }

    MaterialPropertyValue MaterialPropertySet::GetProperty( const std::string& name ) const
    {
        auto it = m_Properties.find( name );
        if ( it != m_Properties.end() )
            return it->second.Value;

        LOG_WARN( "Property not found: {}", name );
        return float( 0.0f );
    }

    MaterialPropertyType MaterialPropertySet::GetPropertyType( const std::string& name ) const
    {
        auto it = m_Properties.find( name );
        if ( it != m_Properties.end() )
            return it->second.Type;

        return MaterialPropertyType::Invalid;
    }

    bool MaterialPropertySet::HasProperty( const std::string& name ) const
    {
        return m_Properties.find( name ) != m_Properties.end();
    }

    void MaterialPropertySet::ResetToDefaults()
    {
        for ( auto& [name, prop] : m_Properties )
        {
            prop.Value         = prop.DefaultValue;
            prop.bIsOverridden = false;
        }
        m_bIsDirty = true;
    }

    void MaterialPropertySet::CopyFrom( const MaterialPropertySet& other )
    {
        m_Properties = other.m_Properties;
        m_bIsDirty   = true;
    }
} // namespace Desert::Graphic