#pragma once

#include <entt/entt.hpp>


#include <cstdint>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include <atomic>

#include <Engine/ECS/MaterialParamOverride.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    // One overridden texture param (keyed by the shader's Texture2D property name) -> texture asset.
    struct MaterialTextureOverride
    {
        std::string Name;
        uint64_t    TextureHandle = 0; // Assets::AssetHandle as uint64 (0 = unset -> shader fallback)

        [[nodiscard]] bool operator==( const MaterialTextureOverride& ) const = default;
    };

    // Assigns an arbitrary shader (by program name) to whatever renderer draws this entity, with its
    // parameters edited generically in Details (built from the shader's Properties block). The
    // renderer builds a DataDrivenMaterial from ShaderName and applies these overrides.
    // ShaderName is the ShaderService COMPILE KEY of a template that is NOT `Role StandardSurface`, resolved from
    // the template's handle by whoever sets it (scene load, a role lookup). Params and Textures belong to THAT
    // shader and to nothing else: a lit mesh's per-entity parameters are its slot instances' (script writes land
    // on StaticMeshComponent::ScriptSlotParams), and a scene block carrying Params with no Shader is refused at
    // load with its path. No decision compares ShaderName to a template's name.
    //
    // The authored state is a VALUE (MaterialDesc, as LocalTransform is for TransformComponent): a writer builds
    // or copies one, changes it, and hands it to ECS::SetMaterial, the one writer, which is a registry.patch so
    // Core::Scene's render-state dirty set sees a shader or parameter swap at the source.
    struct MaterialDesc
    {
        std::string                          ShaderName;
        std::vector<MaterialParamOverride>   Params; // scalar/vector overrides (on top of the Properties defaults)
        std::vector<MaterialTextureOverride> Textures; // texture2D overrides (unset -> backend fallback)

        [[nodiscard]] bool operator==( const MaterialDesc& ) const = default;
    };

    class MaterialComponent;
    bool SetMaterial( entt::registry& registry, entt::entity entity, const MaterialDesc& desc );

    class MaterialComponent
    {
    public:
        MaterialComponent() : m_Revision( NextRevision() )
        {
        }
        explicit MaterialComponent( MaterialDesc desc ) : m_Desc( std::move( desc ) ), m_Revision( NextRevision() )
        {
        }

        [[nodiscard]] const MaterialDesc& GetDesc() const
        {
            return m_Desc;
        }
        [[nodiscard]] const std::string& GetShaderName() const
        {
            return m_Desc.ShaderName;
        }
        [[nodiscard]] const std::vector<MaterialParamOverride>& GetParams() const
        {
            return m_Desc.Params;
        }
        [[nodiscard]] const std::vector<MaterialTextureOverride>& GetTextures() const
        {
            return m_Desc.Textures;
        }
        // UE: the material's uniform-expression cache is keyed by the material's revision. Process-unique, taken
        // again by every SetMaterial that changes the desc, so a draw built from this component (MeshECSSystem's
        // GenericMeshDrawParams) is rebuilt only when it moves. Runtime only: not part of the desc, not serialized
        // (MaterialComponentSer is built from GetDesc). A copy keeps it, since it keeps the same desc.
        [[nodiscard]] uint64_t GetRevision() const
        {
            return m_Revision;
        }

    private:
        [[nodiscard]] static uint64_t NextRevision()
        {
            static std::atomic<uint64_t> s_Revision{ 0 };
            return s_Revision.fetch_add( 1, std::memory_order_relaxed ) + 1;
        }

        MaterialDesc m_Desc;
        uint64_t     m_Revision = 0; // see GetRevision

        friend bool SetMaterial( entt::registry& registry, entt::entity entity, const MaterialDesc& desc );
    };

    // THE ONE WRITER of MaterialComponent: adds it when absent (on_construct), patches it when the value differs
    // (on_update), writes nothing when it is equal. Returns whether anything changed.
    inline bool SetMaterial( entt::registry& registry, entt::entity entity, const MaterialDesc& desc )
    {
        if ( auto* component = registry.try_get<MaterialComponent>( entity ) )
        {
            if ( component->m_Desc == desc )
                return false;
            registry.patch<MaterialComponent>( entity,
                                               [&desc]( MaterialComponent& c )
                                               {
                                                   c.m_Desc     = desc;
                                                   c.m_Revision = MaterialComponent::NextRevision();
                                               } );
            return true;
        }
        registry.emplace<MaterialComponent>( entity, desc );
        return true;
    }
} // namespace Desert::ECS
