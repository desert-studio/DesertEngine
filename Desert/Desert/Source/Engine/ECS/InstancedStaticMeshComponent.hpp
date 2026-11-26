#pragma once

#include <entt/entt.hpp>

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
#include <atomic>

#include <Engine/Assets/Common.hpp>
#include <Engine/ECS/RenderCacheRetire.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>
#include <cstring>

namespace Desert
{
    class DynamicMesh;
} // namespace Desert

namespace Desert::ECS
{
    // InstancedStaticMeshComponent's authored state as a value (field meanings on the component below).
    struct InstancedStaticMeshDesc
    {
        Assets::AssetHandle MeshHandle;
        std::vector<Assets::AssetHandle> MaterialSlots;
        std::vector<glm::mat4> InstanceTransforms;
        bool CastShadows = true;
        bool CastContactShadow = true;
        float MaxDrawDistance = 0.0f;
        std::optional<Geometry::PrimitiveType> Primitive;

        [[nodiscard]] bool operator==( const InstancedStaticMeshDesc& ) const = default;
    };

    class InstancedStaticMeshComponent;
    bool SetInstancedStaticMesh( entt::registry& registry, entt::entity entity, const InstancedStaticMeshDesc& desc );

    // The share of an instance snapshot drawn at a density below 1, and the snapshot index of each transform.
    struct ThinnedInstances
    {
        std::shared_ptr<const std::vector<glm::mat4>> Snapshot;
        std::shared_ptr<const std::vector<glm::mat4>> Transforms;
        std::shared_ptr<const std::vector<uint32_t>>  InstanceIds;
        float                                         Density = 1.0f;
    };

    // UE-style Instanced Static Mesh: ONE mesh + ONE material drawn N times (per-instance world transforms)
    // as a single instanced draw call. The per-instance transforms live in a GPU storage buffer the
    // instanced shader reads by gl_InstanceIndex, so 1000 instances cost ~1 draw + 1 material setup instead
    // of N. Use for repeated static props / NPCs / buildings in the city.
    //
    // READ-ONLY, as TransformComponent and MaterialComponent are: the authored state is a VALUE (InstancedStaticMeshDesc) and its one
    // writer is ECS::SetInstancedStaticMesh, a registry.patch, so on_update<InstancedStaticMeshComponent> fires and Core::Scene's render-state dirty
    // set sees every mesh, slot or flag change at the source (UE MarkRenderStateDirty). The derived render caches
    // (Runtime*, SeenMaterialsVersion) are the proxy-side state MeshECSSystem rebuilds; they are reached through
    // accessors and are not part of the value.
    class InstancedStaticMeshComponent
    {
    public:
        InstancedStaticMeshComponent() = default;
        explicit InstancedStaticMeshComponent( const InstancedStaticMeshDesc& desc )
        {
            Assign( desc );
        }

        [[nodiscard]] InstancedStaticMeshDesc GetDesc() const;
        [[nodiscard]] const Assets::AssetHandle& GetMeshHandle() const
        {
            return MeshHandle;
        }
        [[nodiscard]] const std::vector<Assets::AssetHandle>& GetMaterialSlots() const
        {
            return MaterialSlots;
        }
        [[nodiscard]] const std::vector<glm::mat4>& GetInstanceTransforms() const
        {
            return InstanceTransforms;
        }
        // Moves exactly when InstanceTransforms change (bitwise), through the one writer below; never 0 once
        // the component holds a value. Process-unique, so a component copied to another entity carries a
        // revision no other contents ever had. The render path keys its snapshot off it (UE: the ISM proxy
        // is updated from the component's instance change, never by comparing every instance per frame).
        [[nodiscard]] uint64_t GetInstanceRevision() const
        {
            return m_InstanceRevision;
        }
        [[nodiscard]] const bool& GetCastShadows() const
        {
            return CastShadows;
        }
        [[nodiscard]] const bool& GetCastContactShadow() const
        {
            return CastContactShadow;
        }
        [[nodiscard]] const float& GetMaxDrawDistance() const
        {
            return MaxDrawDistance;
        }
        [[nodiscard]] const std::optional<Geometry::PrimitiveType>& GetPrimitive() const
        {
            return Primitive;
        }

        // Derived render caches (never part of the value, never serialized).
        [[nodiscard]] std::shared_ptr<DynamicMesh>& RuntimeMesh()
        {
            return m_RuntimeMesh;
        }
        [[nodiscard]] const std::shared_ptr<DynamicMesh>& RuntimeMesh() const
        {
            return m_RuntimeMesh;
        }
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
        [[nodiscard]] std::shared_ptr<const std::vector<glm::mat4>>& RuntimeInstanceSnapshot()
        {
            return m_RuntimeInstanceSnapshot;
        }
        [[nodiscard]] const std::shared_ptr<const std::vector<glm::mat4>>& RuntimeInstanceSnapshot() const
        {
            return m_RuntimeInstanceSnapshot;
        }
        // The GetInstanceRevision() the snapshot above was taken at (0 = none taken).
        [[nodiscard]] uint64_t& RuntimeInstanceSnapshotRevision()
        {
            return m_RuntimeInstanceSnapshotRevision;
        }
        // The snapshot's share the renderer draws below full density (MeshECSSystem::ThinIsmRecord); valid for
        // the Snapshot and Density it names.
        [[nodiscard]] ThinnedInstances& RuntimeThinnedInstances()
        {
            return m_RuntimeThinnedInstances;
        }
        [[nodiscard]] const ThinnedInstances& RuntimeThinnedInstances() const
        {
            return m_RuntimeThinnedInstances;
        }

    private:
        void Assign( const InstancedStaticMeshDesc& desc );
        [[nodiscard]] bool Matches( const InstancedStaticMeshDesc& desc ) const;
        [[nodiscard]] static uint64_t NextInstanceRevision()
        {
            static std::atomic<uint64_t> s_Revision{ 0 };
            return s_Revision.fetch_add( 1, std::memory_order_relaxed ) + 1;
        }

        Assets::AssetHandle              MeshHandle;
        std::vector<Assets::AssetHandle> MaterialSlots;
        std::vector<glm::mat4>           InstanceTransforms; // per-instance world matrices
        uint64_t                         m_InstanceRevision = 0; // see GetInstanceRevision

        // false = skipped by the shadow (depth) passes, exactly like the static and skinned twins.
        // Until this existed an ISM was the ONE mesh kind whose shadow could not be turned off: the
        // cascade pass read StaticMeshComponent::CastShadows and SkinnedMeshComponent::CastShadows and
        // appended every ISM unconditionally. A UE-style foliage field of grass is the first thing a
        // scene wants to take out of the cascades, and it was the only thing that could not be.
        bool CastShadows = true;

        // UE's bCastContactShadow, as StaticMeshComponent::CastContactShadow (one flag for the whole field).
        bool CastContactShadow = true;

        // UE's LDMaxDrawDistance, per INSTANCE here: cm from the camera beyond which an instance is not drawn,
        // scaled by the view's lens like the static twin's. Per instance and not by the component's bounds
        // because one ISM spans a whole field: its centre says nothing about where its far instances are.
        // A foliage type's CullDistance fade is clipped to it (Graphic::ClipToMaxDrawDistance). 0 = no limit.
        float MaxDrawDistance = 0.0f;

        // Transient runtime state (not serialized): generated mesh for primitives, the material instance,
        // and a dirty flag so the renderer re-uploads the instance SSBO only when the transforms change.
        std::optional<Geometry::PrimitiveType>    Primitive;
        std::shared_ptr<DynamicMesh>              m_RuntimeMesh;
        std::vector<Graphic::MaterialInstancePtr> m_RuntimeMaterialInstances;
        // The displaced siblings of the same slots (built when the mesh is displaced, empty otherwise).
        std::vector<Graphic::MaterialInstancePtr> m_RuntimeDisplacedInstances;
        uint32_t                                  m_SeenMaterialsVersion = 0; // see StaticMeshComponent

        // `InstancesDirty` STOOD HERE, WRITTEN BY FIVE PLACES AND READ BY NONE. Every one of the five was
        // an authoring site raising it after editing InstanceTransforms; nothing downstream ever asked.
        // A8-3 declined to key the snapshot below off it — a stale picture would then have depended on
        // every future mutation site remembering to raise a flag — which left it with no possible
        // consumer at all, and a write-only field is a dead setting by §1.3 of the contract. Deleted with
        // its five writes rather than wired: the thing it would have driven is already driven by a
        // comparison that cannot drift.

        // The render path's CO-OWNED snapshot of InstanceTransforms above. The draw command used to carry
        // `&InstanceTransforms` — the address of a vector member of an ECS component — and the same two
        // things were wrong with it as with the static path's slot view: entt moves components when the
        // pool changes, and Lua running between the record and the read can destroy this entity outright.
        // See Graphic::MaterialSlotBinding for the full account (A8-3).
        //
        // REBUILT BY REVISION, NOT BY A FLAG — see the note above on the flag that used to sit beside it.
        // The revision cannot drift the way that flag could: InstanceTransforms is private, its only
        // writer is Assign (reached only through ECS::SetInstancedStaticMesh and the constructor), and
        // Assign moves m_InstanceRevision whenever the bytes change. So no mutation site has anything to
        // remember, and an unchanged frame costs one integer compare instead of a memcmp of every instance.
        // A copied component carries both the revision and the snapshot taken at it, so the pair stays true.
        std::shared_ptr<const std::vector<glm::mat4>> m_RuntimeInstanceSnapshot;
        uint64_t                                      m_RuntimeInstanceSnapshotRevision = 0;
        ThinnedInstances                              m_RuntimeThinnedInstances; // derived from the snapshot

        friend bool SetInstancedStaticMesh( entt::registry& registry, entt::entity entity, const InstancedStaticMeshDesc& desc );
    };

    inline InstancedStaticMeshDesc InstancedStaticMeshComponent::GetDesc() const
    {
        InstancedStaticMeshDesc desc;
        desc.MeshHandle = MeshHandle;
        desc.MaterialSlots = MaterialSlots;
        desc.InstanceTransforms = InstanceTransforms;
        desc.CastShadows = CastShadows;
        desc.CastContactShadow = CastContactShadow;
        desc.MaxDrawDistance = MaxDrawDistance;
        desc.Primitive = Primitive;
        return desc;
    }

    inline bool InstancedStaticMeshComponent::Matches( const InstancedStaticMeshDesc& desc ) const
    {
        return MeshHandle == desc.MeshHandle
            && MaterialSlots == desc.MaterialSlots
            && InstanceTransforms == desc.InstanceTransforms
            && CastShadows == desc.CastShadows
            && CastContactShadow == desc.CastContactShadow
            && MaxDrawDistance == desc.MaxDrawDistance
            && Primitive == desc.Primitive;
    }

    inline void InstancedStaticMeshComponent::Assign( const InstancedStaticMeshDesc& desc )
    {
        MeshHandle = desc.MeshHandle;
        MaterialSlots = desc.MaterialSlots;
        // Bitwise, as the GPU rows see it: a changed bit is a changed transform. The first value always
        // takes a revision, so a revision of 0 never names contents.
        if ( m_InstanceRevision == 0 || InstanceTransforms.size() != desc.InstanceTransforms.size() ||
             ( !InstanceTransforms.empty() &&
               std::memcmp( InstanceTransforms.data(), desc.InstanceTransforms.data(),
                            InstanceTransforms.size() * sizeof( glm::mat4 ) ) != 0 ) )
        {
            InstanceTransforms = desc.InstanceTransforms;
            m_InstanceRevision = NextInstanceRevision();
        }
        CastShadows = desc.CastShadows;
        CastContactShadow = desc.CastContactShadow;
        MaxDrawDistance = desc.MaxDrawDistance;
        Primitive = desc.Primitive;
    }

    // THE ONE WRITER of InstancedStaticMeshComponent: adds it when absent (on_construct), patches it when the value differs
    // (on_update), writes nothing when it is equal. A changed MaterialSlots drops the runtime instances, so the
    // mesh system rebuilds them from the new slots. Returns whether anything changed.
    inline bool SetInstancedStaticMesh( entt::registry& registry, entt::entity entity, const InstancedStaticMeshDesc& desc )
    {
        if ( auto* component = registry.try_get<InstancedStaticMeshComponent>( entity ) )
        {
            if ( component->Matches( desc ) )
                return false;
            registry.patch<InstancedStaticMeshComponent>( entity,
                                    [&desc]( InstancedStaticMeshComponent& c )
                                    {
                                        if ( c.GetMaterialSlots() != desc.MaterialSlots )
                                        {
                                            RetireRenderCaches( c.m_RuntimeMaterialInstances );
                                            RetireRenderCaches( c.m_RuntimeDisplacedInstances );
                                            c.m_RuntimeMaterialInstances.clear();
                                            c.m_RuntimeDisplacedInstances.clear();
                                            c.m_SeenMaterialsVersion = 0;
                                        }
                                        // A new mesh source (asset or built-in shape) makes the generated
                                        // mesh describe the old one: drop it, the mesh system resolves the new.
                                        if ( c.GetMeshHandle() != desc.MeshHandle ||
                                             c.GetPrimitive() != desc.Primitive )
                                        {
                                            RetireRenderCache( c.m_RuntimeMesh );
                                            c.m_RuntimeMesh.reset();
                                        }
                                        c.Assign( desc );
                                    } );
            return true;
        }
        registry.emplace<InstancedStaticMeshComponent>( entity, desc );
        return true;
    }

    inline void RetireRenderCachesOf( const InstancedStaticMeshComponent& c )
    {
        RetireRenderCache( c.RuntimeMesh() );
        RetireRenderCaches( c.RuntimeMaterialInstances() );
        RetireRenderCaches( c.RuntimeDisplacedInstances() );
        RetireRenderCache( c.RuntimeInstanceSnapshot() );
        RetireRenderCache( c.RuntimeThinnedInstances().Transforms );
        RetireRenderCache( c.RuntimeThinnedInstances().InstanceIds );
    }
} // namespace Desert::ECS
