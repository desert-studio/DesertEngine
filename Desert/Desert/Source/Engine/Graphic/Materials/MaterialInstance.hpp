#pragma once

#include "MaterialProperty.hpp"
#include <memory>
#include <string>
#include <vector>

namespace Desert::Graphic
{
    class Material;
    class MaterialInstance;

    using MaterialInstancePtr = std::shared_ptr<MaterialInstance>;
    using MaterialPtr         = std::shared_ptr<Material>;

    // ONE ENTITY'S MATERIAL SLOTS, AS THE RENDER PATH IS ALLOWED TO HOLD THEM.
    //
    // WHY THIS TYPE EXISTS AT ALL — A8-3. The mesh path used to carry
    // `const std::vector<MaterialInstance*>*`: the ADDRESS OF A VECTOR MEMBER OF AN ECS COMPONENT. Both
    // halves of that were unsound, and the second half is the one that a copy alone would not have fixed:
    //
    //   * the vendored entt keeps components BY VALUE in a flat `std::vector`, so `AddComponent` moves
    //     every component already in the pool and `DestroyEntity` swap-and-pops another one over the hole
    //     (asserted, not argued, by PointerOwnership.EnttComponentAddressesAreNotStable);
    //   * and the MaterialInstances themselves are owned by that same component's
    //     `RuntimeMaterialInstances`, so destroying the entity destroys them too.
    //
    // Between the two there is a real window and it is not a narrow one: `MeshECSSystem` RECORDS the draw,
    // `ScriptSystem` — registered after it — then runs USER LUA (`World.spawnMarker` adds a component,
    // `entity:destroy()` removes one), and only afterwards does `RenderCommandBuffer::ExecuteAll` read what
    // was recorded, with five more passes reading it after that. The path from "took the address" to "read
    // it" runs through code this project does not write.
    //
    // Ownership question 1 — who is obliged to destroy this? — has two answers here whose order is not
    // fixed: the component that authored the slots, and any draw still in flight. Two owners with unordered
    // deaths is precisely what a `shared_ptr` is for, so the render path CO-OWNS the binding instead of
    // pointing at the component's storage. The cost is one atomic increment per mesh per frame, not the
    // per-frame allocate-and-copy of a slot vector that the raw pointer was introduced to avoid (measured
    // at ~0.9 ms of ExecuteAll for 256 meshes in Debug, which is why the naive fix is the wrong one).
    struct MaterialSlotBinding
    {
        // Keeps every instance alive for as long as ANY holder of this binding lives. The render path never
        // reads this vector; it exists to make the raw view below safe to read.
        std::vector<MaterialInstancePtr> Owned;

        // What the render path reads. Parallel to Owned, and rebuilt with it — never separately.
        std::vector<MaterialInstance*> Slots;
    };

    using MaterialSlotBindingPtr = std::shared_ptr<const MaterialSlotBinding>;

    class MaterialInstance : public std::enable_shared_from_this<MaterialInstance>
    {
    public:
        MaterialInstance( Material* parentMaterial, const std::string& name = "" );
        virtual ~MaterialInstance() = default;

        // Getters with type safety
        float     GetFloat( const std::string& name, float defaultValue = 0.0f ) const;
        int       GetInt( const std::string& name, int defaultValue = 0 ) const;
        bool      GetBool( const std::string& name, bool defaultValue = false ) const;
        glm::vec2 GetVec2( const std::string& name, const glm::vec2& defaultValue = glm::vec2( 0.0f ) ) const;
        glm::vec3 GetVec3( const std::string& name, const glm::vec3& defaultValue = glm::vec3( 0.0f ) ) const;
        glm::vec4 GetVec4( const std::string& name, const glm::vec4& defaultValue = glm::vec4( 0.0f ) ) const;
        glm::mat4 GetMat4( const std::string& name, const glm::mat4& defaultValue = glm::mat4( 1.0f ) ) const;
        void*     GetTexture( const std::string& name ) const;

        // Setters with dirty tracking
        void SetFloat( const std::string& name, float value );
        void SetInt( const std::string& name, int value );
        void SetBool( const std::string& name, bool value );
        void SetVec2( const std::string& name, const glm::vec2& value );
        void SetVec3( const std::string& name, const glm::vec3& value );
        void SetVec4( const std::string& name, const glm::vec4& value );
        void SetMat4( const std::string& name, const glm::mat4& value );
        void SetTexture( const std::string& name, void* texture );

        // Generic by-name write from a packed vec4, dispatched on the param's OWN reflected type (float reads
        // .x, vec2 .xy, vec3 .xyz, vec4 all). This is the data-driven counterpart to the typed setters: apply a
        // MaterialComponent's (name, vec4) overrides with no per-name if-chain — the shader schema decides the
        // type. Mirrors DataDrivenMaterial::SetParamRaw for the instance path. Returns false if `name` is unknown
        // or its type isn't vec4-packable (mat4/texture). See MaterialProperty for the type enum.
        bool SetParamFromVec4( const std::string& name, const glm::vec4& value );

        // Batch operations
        void SetParameters( const std::vector<std::pair<std::string, MaterialPropertyValue>>& params );
        void SetParameters( const MaterialPropertySet& properties );

        // Instance management
        MaterialInstancePtr CreateChildInstance( const std::string& name = "" );
        Material*           GetParentMaterial() const
        {
            return m_ParentMaterial;
        }
        MaterialInstancePtr GetParentInstance() const
        {
            return m_ParentInstance.lock();
        }
        const std::vector<MaterialInstancePtr>& GetChildInstances() const
        {
            return m_ChildInstances;
        }

        // Property access
        bool                       HasParameter( const std::string& name ) const;
        const MaterialPropertySet& GetPropertySet() const
        {
            return m_Properties;
        }
        MaterialPropertySet& GetPropertySetMutable()
        {
            return m_Properties;
        }

        // Drops every instance override so the instance renders with the parent material's values
        // again — the inverse of SetParamFromVec4 (exposed to scripts as clearMaterialParams()).
        void ResetOverrides()
        {
            m_Properties.ResetToDefaults();
            MarkNeedsApply();
        }

        // GPU update
        void MarkNeedsApply()
        {
            m_bNeedsApply = true;
        }
        void Apply();
        bool NeedsApply() const
        {
            return m_bNeedsApply;
        }

        // Debug
        const std::string& GetName() const
        {
            return m_Name;
        }
        void SetName( const std::string& name )
        {
            m_Name = name;
        }

    private:
        void                  PropagateToChildren();
        MaterialPropertyValue ResolveProperty( const std::string& name ) const;

        Material*                        m_ParentMaterial;
        std::string                      m_Name;
        MaterialPropertySet              m_Properties;
        std::weak_ptr<MaterialInstance>  m_ParentInstance;
        std::vector<MaterialInstancePtr> m_ChildInstances;
        bool                             m_bNeedsApply = true;
    };
} // namespace Desert::Graphic