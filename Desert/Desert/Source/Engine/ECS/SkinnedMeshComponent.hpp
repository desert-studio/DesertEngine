#pragma once

#include <Common/Core/Entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <Engine/Assets/Common.hpp>
#include <Engine/ECS/RenderCacheRetire.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>

namespace Desert
{
    class SkinnedMesh;
    namespace Animation
    {
        class Skeleton;
    } // namespace Animation
} // namespace Desert

namespace Desert::ECS
{
    // SkinnedMeshComponent's authored state as a value (field meanings on the component below).
    struct SkinnedMeshDesc
    {
        Assets::AssetHandle MeshHandle;
        std::vector<Assets::AssetHandle> MaterialSlots;
        bool CastShadows = true;
        bool ReceivesDecals = false;
        std::shared_ptr<SkinnedMesh> RuntimeMesh;
        std::shared_ptr<Animation::Skeleton> RuntimeSkeleton;

        [[nodiscard]] bool operator==( const SkinnedMeshDesc& ) const = default;
    };

    class SkinnedMeshComponent;
    bool SetSkinnedMesh( entt::registry& registry, entt::entity entity, const SkinnedMeshDesc& desc );


    //
    // READ-ONLY, as TransformComponent and MaterialComponent are: the authored state is a VALUE (SkinnedMeshDesc) and its one
    // writer is ECS::SetSkinnedMesh, a registry.patch, so on_update<SkinnedMeshComponent> fires and Core::Scene's render-state dirty
    // set sees every mesh, slot or flag change at the source (UE MarkRenderStateDirty). The derived render caches
    // (Runtime*, SeenMaterialsVersion) are the proxy-side state MeshECSSystem rebuilds; they are reached through
    // accessors and are not part of the value.
    class SkinnedMeshComponent
    {
    public:
        SkinnedMeshComponent() = default;
        explicit SkinnedMeshComponent( const SkinnedMeshDesc& desc )
        {
            Assign( desc );
        }

        [[nodiscard]] SkinnedMeshDesc GetDesc() const;
        [[nodiscard]] const Assets::AssetHandle& GetMeshHandle() const
        {
            return MeshHandle;
        }
        [[nodiscard]] const std::vector<Assets::AssetHandle>& GetMaterialSlots() const
        {
            return MaterialSlots;
        }
        [[nodiscard]] const bool& GetCastShadows() const
        {
            return CastShadows;
        }
        [[nodiscard]] const bool& GetReceivesDecals() const
        {
            return ReceivesDecals;
        }
        [[nodiscard]] const std::shared_ptr<SkinnedMesh>& GetRuntimeMesh() const
        {
            return RuntimeMesh;
        }
        [[nodiscard]] const std::shared_ptr<Animation::Skeleton>& GetRuntimeSkeleton() const
        {
            return RuntimeSkeleton;
        }

        // Derived render caches (never part of the value, never serialized).
        [[nodiscard]] std::vector<Graphic::MaterialInstancePtr>& RuntimeMaterialInstances()
        {
            return m_RuntimeMaterialInstances;
        }
        [[nodiscard]] const std::vector<Graphic::MaterialInstancePtr>& RuntimeMaterialInstances() const
        {
            return m_RuntimeMaterialInstances;
        }
        [[nodiscard]] std::vector<Graphic::MaterialInstancePtr>& RuntimeDisplacedInstances()
        {
            return m_RuntimeDisplacedInstances;
        }
        [[nodiscard]] const std::vector<Graphic::MaterialInstancePtr>& RuntimeDisplacedInstances() const
        {
            return m_RuntimeDisplacedInstances;
        }
        [[nodiscard]] uint32_t& SeenMaterialsVersion()
        {
            return m_SeenMaterialsVersion;
        }
        [[nodiscard]] const uint32_t& SeenMaterialsVersion() const
        {
            return m_SeenMaterialsVersion;
        }
        // The draw's co-owned slot binding over the instances above (Displaced set for a displaced mesh): built
        // the update the instances are (re)built, then shared by every frame's command (one atomic increment),
        // never rebuilt per frame. Dropped whenever the instances or the mesh it was built against change.
        [[nodiscard]] Graphic::MaterialSlotBindingPtr& RuntimeSlots()
        {
            return m_RuntimeSlots;
        }
        [[nodiscard]] const Graphic::MaterialSlotBindingPtr& RuntimeSlots() const
        {
            return m_RuntimeSlots;
        }
        // The mesh RuntimeSlots was built for (identity only, never dereferenced): another mesh, another binding.
        [[nodiscard]] const void*& RuntimeSlotsMesh()
        {
            return m_RuntimeSlotsMesh;
        }

    private:
        void Assign( const SkinnedMeshDesc& desc );
        [[nodiscard]] bool Matches( const SkinnedMeshDesc& desc ) const;

        Assets::AssetHandle              MeshHandle;
        std::vector<Assets::AssetHandle> MaterialSlots;
        std::vector<Graphic::MaterialInstancePtr>
                 m_RuntimeMaterialInstances; // Cache to keep instances alive and avoid per-frame allocations
        // The displaced (base-cell) siblings of the same slots, drawn over the displaced twin (CONTRACT-DISPLACE
        // §9.8); built when the mesh is displaced, empty otherwise. Transient like the cache above.
        std::vector<Graphic::MaterialInstancePtr> m_RuntimeDisplacedInstances;
        Graphic::MaterialSlotBindingPtr           m_RuntimeSlots;                   // see RuntimeSlots
        const void*                               m_RuntimeSlotsMesh     = nullptr; // see RuntimeSlotsMesh
        uint32_t m_SeenMaterialsVersion = 0; // see StaticMeshComponent
        bool     CastShadows          = true; // false = skipped by the shadow (depth) passes, like the static twin
        // UE's bReceivesDecals, OFF by default here (UE characters turn it off by convention): a decal projected
        // onto a deforming surface swims as the pose moves under it. On = the G-buffer write takes the DBuffer.
        bool     ReceivesDecals       = false;

        // In-editor rig: a skinned mesh built at runtime by "Convert to Skinned" (from a static mesh + placed
        // bones, auto-weighted), NOT yet a cooked asset. When set, RuntimeMesh overrides MeshHandle in the
        // render path. RuntimeSkeleton owns the Skeleton that RuntimeMesh references by raw pointer, keeping it
        // alive for the mesh's lifetime (destroyed together with the component).
        std::shared_ptr<SkinnedMesh>         RuntimeMesh;
        std::shared_ptr<Animation::Skeleton> RuntimeSkeleton;

        friend bool SetSkinnedMesh( entt::registry& registry, entt::entity entity, const SkinnedMeshDesc& desc );
    };

    inline SkinnedMeshDesc SkinnedMeshComponent::GetDesc() const
    {
        SkinnedMeshDesc desc;
        desc.MeshHandle = MeshHandle;
        desc.MaterialSlots = MaterialSlots;
        desc.CastShadows = CastShadows;
        desc.ReceivesDecals = ReceivesDecals;
        desc.RuntimeMesh = RuntimeMesh;
        desc.RuntimeSkeleton = RuntimeSkeleton;
        return desc;
    }

    inline bool SkinnedMeshComponent::Matches( const SkinnedMeshDesc& desc ) const
    {
        return MeshHandle == desc.MeshHandle
            && MaterialSlots == desc.MaterialSlots
            && CastShadows == desc.CastShadows
            && ReceivesDecals == desc.ReceivesDecals
            && RuntimeMesh == desc.RuntimeMesh
            && RuntimeSkeleton == desc.RuntimeSkeleton;
    }

    inline void SkinnedMeshComponent::Assign( const SkinnedMeshDesc& desc )
    {
        MeshHandle = desc.MeshHandle;
        MaterialSlots = desc.MaterialSlots;
        CastShadows = desc.CastShadows;
        ReceivesDecals = desc.ReceivesDecals;
        RuntimeMesh = desc.RuntimeMesh;
        RuntimeSkeleton = desc.RuntimeSkeleton;
    }

    // THE ONE WRITER of SkinnedMeshComponent: adds it when absent (on_construct), patches it when the value differs
    // (on_update), writes nothing when it is equal. A changed MaterialSlots drops the runtime instances, so the
    // mesh system rebuilds them from the new slots. Returns whether anything changed.
    inline bool SetSkinnedMesh( entt::registry& registry, entt::entity entity, const SkinnedMeshDesc& desc )
    {
        if ( auto* component = registry.try_get<SkinnedMeshComponent>( entity ) )
        {
            if ( component->Matches( desc ) )
                return false;
            registry.patch<SkinnedMeshComponent>( entity,
                                    [&desc]( SkinnedMeshComponent& c )
                                    {
                                        if ( c.GetMaterialSlots() != desc.MaterialSlots )
                                        {
                                            RetireRenderCaches( c.m_RuntimeMaterialInstances );
                                            RetireRenderCaches( c.m_RuntimeDisplacedInstances );
                                            c.m_RuntimeMaterialInstances.clear();
                                            c.m_RuntimeDisplacedInstances.clear();
                                            c.m_SeenMaterialsVersion = 0;
                                        }
                                        // Any value change rebuilds the binding (new slots, a new mesh).
                                        RetireRenderCache( c.m_RuntimeSlots );
                                        c.m_RuntimeSlots.reset();
                                        c.m_RuntimeSlotsMesh = nullptr;
                                        // Assign replaces the mesh and skeleton the draws in flight may name.
                                        if ( c.RuntimeMesh != desc.RuntimeMesh )
                                            RetireRenderCache( c.RuntimeMesh );
                                        if ( c.RuntimeSkeleton != desc.RuntimeSkeleton )
                                            RetireRenderCache( c.RuntimeSkeleton );
                                        c.Assign( desc );
                                    } );
            return true;
        }
        registry.emplace<SkinnedMeshComponent>( entity, desc );
        return true;
    }

    inline void RetireRenderCachesOf( const SkinnedMeshComponent& c )
    {
        RetireRenderCache( c.GetRuntimeMesh() );
        RetireRenderCache( c.GetRuntimeSkeleton() );
        RetireRenderCaches( c.RuntimeMaterialInstances() );
        RetireRenderCaches( c.RuntimeDisplacedInstances() );
        RetireRenderCache( c.RuntimeSlots() );
    }
} // namespace Desert::ECS
