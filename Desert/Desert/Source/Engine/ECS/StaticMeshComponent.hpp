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

#include <Engine/Assets/Common.hpp>
#include <Engine/ECS/MaterialParamOverride.hpp>
#include <Engine/ECS/RenderCacheRetire.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Engine/Graphic/Materials/MaterialInstance.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>

namespace Desert::Geometry
{
    class DynamicMesh3;
}

namespace Desert::Reflection::Generated
{
    void RegisterReflection_Engine();
} // namespace Desert::Reflection::Generated

namespace Desert
{
    class DynamicMesh;
} // namespace Desert

namespace Desert::ECS
{
    // StaticMeshComponent's authored state as a value (field meanings on the component below). RuntimeMesh is derived
    // from EditableMesh by ECS::SetEditableMesh only.
    struct StaticMeshDesc
    {
        Assets::AssetHandle MeshHandle;
        std::vector<Assets::AssetHandle> MaterialSlots;
        std::optional<Geometry::PrimitiveType> Primitive;
        std::shared_ptr<const Geometry::DynamicMesh3> EditableMesh;
        std::shared_ptr<DynamicMesh> RuntimeMesh;
        bool OutlineDraw = false;
        int ForcedLOD = -1;
        int LODBias = 0;
        bool CastShadows = true;
        bool ReceiveShadows = true;
        bool CastContactShadow = true;
        float MaxDrawDistance = 0.0f;
        int TranslucencySortPriority = 0;
        uint64_t HiddenSubmeshes = 0;
        std::vector<MaterialParamOverride> ScriptSlotParams;
        Assets::AssetHandle ImpostorMesh;
        Assets::AssetHandle ImpostorMaterial;
        float ImpostorScreenSize = 0.0f;

        [[nodiscard]] bool operator==( const StaticMeshDesc& ) const = default;
    };

    class StaticMeshComponent;
    bool SetStaticMesh( entt::registry& registry, entt::entity entity, const StaticMeshDesc& desc );

    // UE's UStaticMeshComponent: the two authored references (UPROPERTY StaticMesh, OverrideMaterials) are
    // REFLECTED, so Details draws the mesh row from them and DesertCtl `set` addresses them (StaticMesh.MeshHandle,
    // StaticMesh.MaterialSlots[0]); an edit re-states what depends on them through ECS::PostEditStaticMesh (UE
    // PostEditChangeProperty). On disk they are these two fields' own keys, written by reflection as {Guid, Path}
    // (asset references v1, Assets::kAssetRefSchemaTag); World Partition, HLOD and WorldGen read the same keys from
    // raw records through Assets::StaticMeshComponentSer.
    //
    // READ-ONLY, as TransformComponent and MaterialComponent are: the authored state is a VALUE (StaticMeshDesc) and its one
    // writer is ECS::SetStaticMesh, a registry.patch, so on_update<StaticMeshComponent> fires and Core::Scene's render-state dirty
    // set sees every mesh, slot or flag change at the source (UE MarkRenderStateDirty). The derived render caches
    // (Runtime*, SeenMaterialsVersion) are the proxy-side state MeshECSSystem rebuilds; they are reached through
    // accessors and are not part of the value.
    class StaticMeshComponent
    {
        REFLECT()

    public:
        StaticMeshComponent() = default;
        explicit StaticMeshComponent( const StaticMeshDesc& desc )
        {
            Assign( desc );
        }

        [[nodiscard]] StaticMeshDesc GetDesc() const;
        [[nodiscard]] const Assets::AssetHandle& GetMeshHandle() const
        {
            return MeshHandle;
        }
        [[nodiscard]] const std::vector<Assets::AssetHandle>& GetMaterialSlots() const
        {
            return MaterialSlots;
        }
        [[nodiscard]] const std::optional<Geometry::PrimitiveType>& GetPrimitive() const
        {
            return Primitive;
        }
        [[nodiscard]] const std::shared_ptr<const Geometry::DynamicMesh3>& GetEditableMesh() const
        {
            return EditableMesh;
        }
        [[nodiscard]] const std::shared_ptr<DynamicMesh>& GetRuntimeMesh() const
        {
            return RuntimeMesh;
        }
        [[nodiscard]] const bool& GetOutlineDraw() const
        {
            return OutlineDraw;
        }
        [[nodiscard]] const int& GetForcedLOD() const
        {
            return ForcedLOD;
        }
        [[nodiscard]] const int& GetLODBias() const
        {
            return LODBias;
        }
        [[nodiscard]] const bool& GetCastShadows() const
        {
            return CastShadows;
        }
        [[nodiscard]] const bool& GetReceiveShadows() const
        {
            return ReceiveShadows;
        }
        [[nodiscard]] const bool& GetCastContactShadow() const
        {
            return CastContactShadow;
        }
        [[nodiscard]] const float& GetMaxDrawDistance() const
        {
            return MaxDrawDistance;
        }
        [[nodiscard]] const int& GetTranslucencySortPriority() const
        {
            return TranslucencySortPriority;
        }
        [[nodiscard]] const uint64_t& GetHiddenSubmeshes() const
        {
            return HiddenSubmeshes;
        }
        [[nodiscard]] const std::vector<MaterialParamOverride>& GetScriptSlotParams() const
        {
            return ScriptSlotParams;
        }
        [[nodiscard]] const Assets::AssetHandle& GetImpostorMesh() const
        {
            return ImpostorMesh;
        }
        [[nodiscard]] const Assets::AssetHandle& GetImpostorMaterial() const
        {
            return ImpostorMaterial;
        }
        [[nodiscard]] const float& GetImpostorScreenSize() const
        {
            return ImpostorScreenSize;
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
        [[nodiscard]] Graphic::MaterialSlotBindingPtr& RuntimeSlots()
        {
            return m_RuntimeSlots;
        }
        [[nodiscard]] const Graphic::MaterialSlotBindingPtr& RuntimeSlots() const
        {
            return m_RuntimeSlots;
        }
        [[nodiscard]] uint32_t& SeenMaterialsVersion()
        {
            return m_SeenMaterialsVersion;
        }
        [[nodiscard]] const uint32_t& SeenMaterialsVersion() const
        {
            return m_SeenMaterialsVersion;
        }
        // The impostor card's one slot (ImpostorMaterial's runtime instance), built by MeshECSSystem with the card
        // and dropped with the section instances (a material invalidation, a changed ImpostorMaterial).
        [[nodiscard]] Graphic::MaterialSlotBindingPtr& RuntimeImpostorSlots()
        {
            return m_RuntimeImpostorSlots;
        }
        [[nodiscard]] const Graphic::MaterialSlotBindingPtr& RuntimeImpostorSlots() const
        {
            return m_RuntimeImpostorSlots;
        }

    private:
        void Assign( const StaticMeshDesc& desc );
        [[nodiscard]] bool Matches( const StaticMeshDesc& desc ) const;


        PROPERTY( DisplayName( "Static Mesh" ), Category( "Static Mesh" ), Summary, Asset<StaticMeshAsset>,
                  Tooltip( "The mesh asset this component draws (UE StaticMesh). Ignored while a Primitive "
                           "or an edited mesh is set; setting it clears both." ) )
        Assets::AssetHandle MeshHandle;
        // Drawn by the component's Materials list (one row per mesh section, as UE customizes OverrideMaterials).
        PROPERTY( DisplayName( "Materials" ), Category( "Materials" ), Asset<MaterialAsset>, Hidden,
                  Tooltip( "One material per mesh section (UE OverrideMaterials); a null slot takes the "
                           "mesh's own section material." ) )
        std::vector<Assets::AssetHandle> MaterialSlots;
        std::vector<Graphic::MaterialInstancePtr>
             m_RuntimeMaterialInstances; // Cache to keep instances alive and avoid per-frame allocations
        // The render path's CO-OWNED handle on those instances, rebuilt with them and never separately.
        // It replaced a `std::vector<MaterialInstance*> RuntimeSlotPtrs` whose ADDRESS the draw commands
        // took: entt moves components when the pool changes and Lua runs between recording a draw and
        // executing it, so both the vector and the instances in it could be gone by the time the renderer
        // read them. See Graphic::MaterialSlotBinding for the full account (A8-3).
        Graphic::MaterialSlotBindingPtr        m_RuntimeSlots;
        std::optional<Geometry::PrimitiveType> Primitive; // Optional primitive type for dynamic generation
        // A MESH BUILT IN THE EDITOR (CubeGrid, PolyEdit, later the Create/Model tools): the SOURCE OF TRUTH
        // for this entity's geometry, saved as-is (StaticMeshComponentSer::EditMesh). Immutable once set -
        // an edit builds a new DynamicMesh3 and hands it to ECS::SetEditableMesh (Engine/ECS/EditableMesh.hpp),
        // so an undo record can keep the old one by reference and a snapshot never aliases a live edit.
        std::shared_ptr<const Geometry::DynamicMesh3> EditableMesh;
        // DERIVED from EditableMesh by ECS::SetEditableMesh (Geometry::ToRenderMesh), and only by it: null
        // exactly when EditableMesh is. What the render path, picking and the Details panel read.
        std::shared_ptr<DynamicMesh>           RuntimeMesh;
        bool                                   OutlineDraw = false;
        int                                    ForcedLOD   = -1; // -1 = auto (by screen size); 0..N pins a LOD
        int  LODBias        = 0;    // shifts the AUTO-picked LOD (+coarser, -finer); ignored when ForcedLOD >= 0
        bool CastShadows    = true; // false = skipped by the shadow (depth) passes
        bool ReceiveShadows = true; // false = sun shadows are not applied to this mesh (forward path)
        // UE's UPrimitiveComponent::bCastContactShadow: whether this mesh, seen by a light's contact-shadow ray
        // (Common/ContactShadow.glslh), darkens what is behind it with the light's Contact Shadow Casting
        // Intensity (on) or its Non Casting Intensity (off). The G-buffer carries it per pixel (the shading word's
        // NO_CONTACT_SHADOW bit); a mesh drawn after the opaque pass is never in the depth the ray marches.
        bool CastContactShadow = true;
        // UE's LDMaxDrawDistance: cm from the camera beyond which the mesh is not drawn, measured to its bounds'
        // centre and scaled by the view's lens (Graphic::DrawDistanceScale: a narrower FOV draws farther).
        // 0 = drawn at any distance. A culled mesh casts no shadow either.
        float MaxDrawDistance = 0.0f;
        // Translucency pass order override (UE's TranslucencySortPriority): a LOWER value draws first, i.e.
        // behind a higher one whatever their distances; within one value the pass sorts back to front
        // (Graphic::System::TranslucentSortOrder). Only meaningful for a Translucent-blend material.
        int TranslucencySortPriority = 0;
        // Per-submesh visibility: bit i set = submesh i is HIDDEN (skipped at draw). 0 = all visible. Up to
        // 64 submeshes; edited per Element in the Materials panel.
        uint64_t HiddenSubmeshes = 0;
        // Transient: MaterialService invalidation stamp the runtime instances were built against;
        // a mismatch forces a rebuild (see MeshECSSystem) so parent Material* can never dangle.
        uint32_t m_SeenMaterialsVersion = 0;
        // SCRIPT PARAMETER OVERRIDES ON MATERIAL SLOT 0 (UE: the MID UMeshComponent::Set*ParameterValueOnMaterials
        // creates on demand). Runtime, never serialized: written by entity:setMaterialParam / cleared by
        // clearMaterialParams (Libraries/EntityLibrary.cpp, MaterialLibrary). Held HERE, on the mesh that owns the
        // slot, so a write made before the slot-0 instance exists (OnCreate runs before MeshECSSystem builds it)
        // is not lost, and so a rebuild (material invalidation, slot change) does not drop it: MeshECSSystem
        // applies the list to every slot-0 instance it builds. The authored slots stay the source of truth for
        // everything else.
        std::vector<MaterialParamOverride> ScriptSlotParams;

        // THE FAR CARD (WP-FAR-12E; UE ImpostorBaker / the SpeedTree billboard LOD: the last level of a mesh is an
        // octahedral impostor). ImpostorMesh is the card an Impostor HLOD bake stores (Core::HLODImpostorCardMesh,
        // T_I/M_I<key> beside it, one bake per mesh and materials: Core::HLODImpostorKey), ImpostorMaterial its
        // M_I ImpostorSurface material, ImpostorScreenSize the UE screen size below which the card is drawn in
        // place of the mesh (MeshECSSystem pairs the two; SceneRenderer::SubmitStaticScene admits one side per
        // view). 0, or either reference unset: the mesh draws at every size, as without an impostor.
        PROPERTY( DisplayName( "Impostor Mesh" ), Category( "LOD" ), Asset<StaticMeshAsset>,
                  Tooltip( "The impostor card mesh an Impostor HLOD bake stored for this mesh (UE ImpostorBaker)." ) )
        Assets::AssetHandle ImpostorMesh;
        PROPERTY( DisplayName( "Impostor Material" ), Category( "LOD" ), Asset<MaterialAsset>,
                  Tooltip( "The card's M_I ImpostorSurface material from the same bake." ) )
        Assets::AssetHandle ImpostorMaterial;
        PROPERTY( DisplayName( "Impostor Screen Size" ), Category( "LOD" ),
                  Tooltip( "UE screen size (bounds diameter / view height) below which the card replaces the "
                           "mesh; 0 = never." ) )
        float ImpostorScreenSize = 0.0f;
        Graphic::MaterialSlotBindingPtr m_RuntimeImpostorSlots;

        friend void ::Desert::Reflection::Generated::RegisterReflection_Engine();
        friend bool SetStaticMesh( entt::registry& registry, entt::entity entity, const StaticMeshDesc& desc );
    };

    inline StaticMeshDesc StaticMeshComponent::GetDesc() const
    {
        StaticMeshDesc desc;
        desc.MeshHandle = MeshHandle;
        desc.MaterialSlots = MaterialSlots;
        desc.Primitive = Primitive;
        desc.EditableMesh = EditableMesh;
        desc.RuntimeMesh = RuntimeMesh;
        desc.OutlineDraw = OutlineDraw;
        desc.ForcedLOD = ForcedLOD;
        desc.LODBias = LODBias;
        desc.CastShadows = CastShadows;
        desc.ReceiveShadows = ReceiveShadows;
        desc.CastContactShadow = CastContactShadow;
        desc.MaxDrawDistance = MaxDrawDistance;
        desc.TranslucencySortPriority = TranslucencySortPriority;
        desc.HiddenSubmeshes = HiddenSubmeshes;
        desc.ScriptSlotParams = ScriptSlotParams;
        desc.ImpostorMesh = ImpostorMesh;
        desc.ImpostorMaterial = ImpostorMaterial;
        desc.ImpostorScreenSize = ImpostorScreenSize;
        return desc;
    }

    inline bool StaticMeshComponent::Matches( const StaticMeshDesc& desc ) const
    {
        return MeshHandle == desc.MeshHandle
            && MaterialSlots == desc.MaterialSlots
            && Primitive == desc.Primitive
            && EditableMesh == desc.EditableMesh
            && RuntimeMesh == desc.RuntimeMesh
            && OutlineDraw == desc.OutlineDraw
            && ForcedLOD == desc.ForcedLOD
            && LODBias == desc.LODBias
            && CastShadows == desc.CastShadows
            && ReceiveShadows == desc.ReceiveShadows
            && CastContactShadow == desc.CastContactShadow
            && MaxDrawDistance == desc.MaxDrawDistance
            && TranslucencySortPriority == desc.TranslucencySortPriority
            && HiddenSubmeshes == desc.HiddenSubmeshes
            && ScriptSlotParams == desc.ScriptSlotParams
            && ImpostorMesh == desc.ImpostorMesh
            && ImpostorMaterial == desc.ImpostorMaterial
            && ImpostorScreenSize == desc.ImpostorScreenSize;
    }

    inline void StaticMeshComponent::Assign( const StaticMeshDesc& desc )
    {
        MeshHandle = desc.MeshHandle;
        MaterialSlots = desc.MaterialSlots;
        Primitive = desc.Primitive;
        EditableMesh = desc.EditableMesh;
        RuntimeMesh = desc.RuntimeMesh;
        OutlineDraw = desc.OutlineDraw;
        ForcedLOD = desc.ForcedLOD;
        LODBias = desc.LODBias;
        CastShadows = desc.CastShadows;
        ReceiveShadows = desc.ReceiveShadows;
        CastContactShadow = desc.CastContactShadow;
        MaxDrawDistance = desc.MaxDrawDistance;
        TranslucencySortPriority = desc.TranslucencySortPriority;
        HiddenSubmeshes = desc.HiddenSubmeshes;
        ScriptSlotParams = desc.ScriptSlotParams;
        ImpostorMesh = desc.ImpostorMesh;
        ImpostorMaterial = desc.ImpostorMaterial;
        ImpostorScreenSize = desc.ImpostorScreenSize;
    }

    // THE ONE WRITER of StaticMeshComponent: adds it when absent (on_construct), patches it when the value differs
    // (on_update), writes nothing when it is equal. A changed MaterialSlots drops the runtime instances, so the
    // mesh system rebuilds them from the new slots. Returns whether anything changed.
    inline bool SetStaticMesh( entt::registry& registry, entt::entity entity, const StaticMeshDesc& desc )
    {
        if ( auto* component = registry.try_get<StaticMeshComponent>( entity ) )
        {
            if ( component->Matches( desc ) )
                return false;
            registry.patch<StaticMeshComponent>( entity,
                                    [&desc]( StaticMeshComponent& c )
                                    {
                                        if ( c.GetMaterialSlots() != desc.MaterialSlots )
                                        {
                                            RetireRenderCaches( c.m_RuntimeMaterialInstances );
                                            RetireRenderCache( c.m_RuntimeSlots );
                                            c.m_RuntimeMaterialInstances.clear();
                                            c.m_RuntimeSlots.reset();
                                            c.m_SeenMaterialsVersion = 0;
                                        }
                                        if ( c.ImpostorMaterial != desc.ImpostorMaterial )
                                        {
                                            RetireRenderCache( c.m_RuntimeImpostorSlots );
                                            c.m_RuntimeImpostorSlots.reset();
                                        }
                                        // Assign replaces the edited mesh the draws in flight may name.
                                        if ( c.RuntimeMesh != desc.RuntimeMesh )
                                            RetireRenderCache( c.RuntimeMesh );
                                        c.Assign( desc );
                                    } );
            return true;
        }
        registry.emplace<StaticMeshComponent>( entity, desc );
        return true;
    }

    // EVERY RENDER CACHE A COMPONENT HOLDS, parked when the component is destroyed (Core::Scene connects these to
    // on_destroy: the Details panel's remove, an entity deleted, an undo that rebuilds an entity, a world
    // cleared). The copies are the owners that outlive the component (RetireRenderCache).
    inline void RetireRenderCachesOf( const StaticMeshComponent& c )
    {
        RetireRenderCache( c.GetRuntimeMesh() );
        RetireRenderCaches( c.RuntimeMaterialInstances() );
        RetireRenderCache( c.RuntimeSlots() );
        RetireRenderCache( c.RuntimeImpostorSlots() );
    }
} // namespace Desert::ECS
