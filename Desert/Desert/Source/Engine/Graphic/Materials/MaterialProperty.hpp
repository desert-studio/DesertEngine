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

    // A fresh material parameter revision, unique across the process (MaterialPropertySet::GetRevision,
    // DataDrivenMaterial::GetParamRowRevision): an object created at a freed one's address never shows the old
    // object's revision, so a cache keyed by address and revision cannot alias.
    [[nodiscard]] uint64_t NextMaterialRevision();

    class MaterialPropertySet
    {
    public:
        MaterialPropertySet() = default;
        // A copy is another set: its own revision (a cache that held the source's never takes the copy for it).
        MaterialPropertySet( const MaterialPropertySet& other ) : m_Properties( other.m_Properties ) {}
        MaterialPropertySet& operator=( const MaterialPropertySet& other )
        {
            CopyFrom( other );
            return *this;
        }

        struct MaterialProperty
        {
            std::string           Name;
            MaterialPropertyType  Type = MaterialPropertyType::Invalid;
            MaterialPropertyValue Value;
            MaterialPropertyValue DefaultValue;
            bool                  bIsOverridden = false;
            bool                  bIsTexture    = false;
        };


        void SetProperty( const std::string& name, const MaterialPropertyValue& value, MaterialPropertyType type );
        MaterialPropertyValue GetProperty( const std::string& name ) const;
        MaterialPropertyType  GetPropertyType( const std::string& name ) const;
        bool                  HasProperty( const std::string& name ) const;
        /// Drop @p name's entry; false when there was none.
        bool RemoveProperty( const std::string& name );

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

        // Changes with every write of a property's value, override flag or presence (every mutating member above
        // takes a fresh NextMaterialRevision) and at no other time: MeshRenderer::RegisterMaterialRow re-reads an
        // instance's overrides only when it moved (UE: a MaterialInstance parameter change marks the cached mesh
        // draw commands that use it dirty).
        [[nodiscard]] uint64_t GetRevision() const
        {
            return m_Revision;
        }

    private:
        void Changed()
        {
            m_bIsDirty = true;
            m_Revision = NextMaterialRevision();
        }

        std::unordered_map<std::string, MaterialProperty> m_Properties;
        bool                                              m_bIsDirty = true;
        uint64_t                                          m_Revision = NextMaterialRevision();
    };
} // namespace Desert::Graphic