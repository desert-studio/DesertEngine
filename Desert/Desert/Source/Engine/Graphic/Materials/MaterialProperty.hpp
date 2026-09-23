#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>

namespace Desert::Graphic
{
    enum class MaterialPropertyType
    {
        Float,
        Int,
        Bool,
        Vec2,
        Vec3,
        Vec4,
        Mat4,
        Texture,
        Invalid
    };

    using MaterialPropertyValue = std::variant<float, int, bool, glm::vec2, glm::vec3, glm::vec4, glm::mat4,
                                               void* // texture pointer
                                               >;
    class MaterialPropertySet
    {
    public:

        struct MaterialProperty
        {
            std::string           Name;
            MaterialPropertyType  Type = MaterialPropertyType::Invalid;
            MaterialPropertyValue Value;
            MaterialPropertyValue DefaultValue;
            bool                  bIsOverridden    = false;
            bool                  bIsTexture       = false;
            uint32_t              LastUpdatedFrame = 0;
        };


        void SetProperty( const std::string& name, const MaterialPropertyValue& value, MaterialPropertyType type );
        MaterialPropertyValue GetProperty( const std::string& name ) const;
        MaterialPropertyType  GetPropertyType( const std::string& name ) const;
        bool                  HasProperty( const std::string& name ) const;

        const auto& GetProperties() const
        {
            return m_Properties;
        }

        void MarkDirty()
        {
            m_bIsDirty = true;
        }
        bool IsDirty() const
        {
            return m_bIsDirty;
        }
        void ClearDirty()
        {
            m_bIsDirty = false;
        }

        void ResetToDefaults();
        void CopyFrom( const MaterialPropertySet& other );

    private:
        std::unordered_map<std::string, MaterialProperty> m_Properties;
        bool                                              m_bIsDirty = true;
    };
} // namespace Desert::Graphic