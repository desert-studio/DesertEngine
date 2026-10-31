#pragma once

#include <entt/entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <Engine/Geometry/Mesh.hpp>
#include <Engine/Geometry/PrimitiveType.hpp>
#include <Engine/Graphic/Environment/SceneEnvironment.hpp>

#include <Engine/Assets/Common.hpp>
#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Projection.hpp>
#include <Engine/Graphic/Materials/Material.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Timeline/AudioVoices.hpp>
#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <PhysicsCore/PhysicsWorld.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::Assets::Serialization
{
    struct VFXSystemData;
}
#include <UI/Args/UIArgs.hpp>

// Components big enough to own a file. They live in Desert::ECS like everything below, and are included
// here so that "the components" remains one include for every consumer.
#include <Engine/ECS/ExponentialHeightFogComponent.hpp>
#include <Engine/ECS/LightUnits.hpp>
#include <Common/Settings/RenderMethods.hpp>
#include <Engine/ECS/HeroCloudComponent.hpp>
#include <Engine/ECS/ProceduralFoliageComponent.hpp>
#include <Engine/ECS/DestructibleComponent.hpp>
#include <Engine/ECS/WaterBodyComponent.hpp>
#include <Engine/ECS/RagdollComponent.hpp>
#include <Engine/ECS/WaterZoneComponent.hpp>
#include <Engine/ECS/DestructionFieldComponents.hpp>
#include <Engine/ECS/PostProcessVolumeComponent.hpp>
#include <Engine/ECS/VolumeComponent.hpp>
#include <Engine/ECS/ReflectionCaptureComponent.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/ECS/WindSourceComponent.hpp>
#include <Engine/ECS/SkyAtmosphereComponent.hpp>
#include <Engine/ECS/VFXPointSet.hpp>
#include <Engine/ECS/VFXUserOverride.hpp>
#include <Engine/ECS/TimeOfDayComponent.hpp>
#include <Engine/World/Landscape/LandscapeEditLayers.hpp>
#include <Engine/World/Landscape/LandscapeSplines.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>
#include <string_view>

namespace Desert::Geometry
{
    class DynamicMesh3;
}

namespace Desert
{
    class Mesh;
    class DynamicMesh;
    class SkinnedMesh;
    namespace Animation
    {
        class Skeleton;
        namespace Graph
        {
            struct AnimGraph;
            class Evaluator;
        } // namespace Graph
    } // namespace Animation
} // namespace Desert

namespace Desert::Graphic
{
    class Image2D;
}

namespace Desert::Reflection::Generated
{
    void RegisterReflection_Engine();
} // namespace Desert::Reflection::Generated

namespace Desert::ECS
{
    struct TagComponent
    {
        std::string Tag;
    };

    struct UUIDComponent
    {
        // The one place in the engine that WANTS a fresh random id per construction: every entity must be
        // distinguishable from every other, and there is no path or file to derive that from. Spelled out
        // because `Common::UUID` alone is null — see UUID.hpp.
        Common::UUID UUID = Common::UUID::Generate();
    };

    // "Reflected render-data block": editable, reflected fields the editor draws and the renderer maps
    // to its GPU representation. This is the general concept — a surface material (its template's
    // MaterialLayout) is just ONE specialization; camera and lights are others. NOT a material, hence the member
    // is `Data`.
    struct CameraData
    {
        REFLECT()

        // UE's UCameraComponent::bAutoActivateForPlayer: Play views through this camera when the pawn has
        // none of its own. Off by default, because a default of true on EVERY camera made "which one?" a
        // question of entity order; more than one set is refused at Play (Core::Scene::ResolveViewTarget).
        PROPERTY( DisplayName( "Auto Activate for Player" ), Category( "Camera" ),
                  Tooltip( "Play views through this camera when the player's pawn has no camera of its own. "
                           "At most one camera in a level may have it." ) )
        bool AutoActivateForPlayer = false;

        PROPERTY( DisplayName( "Field of View" ), Category( "Camera" ), Range( 10.0f, 120.0f ),
                  Header( "Projection" ), Units( "deg" ), Summary,
                  Tooltip( "Vertical field of view, in degrees." ) )
        float FOV = 45.0f;

        PROPERTY( DisplayName( "Near" ), Category( "Camera" ), Range( 1.0f, 1000.0f ), Length,
                  Tooltip( "Near clip plane distance. Anything closer is not drawn." ) )
        float Near = Core::kDefaultNearPlane;

        // The range reaches 100 km because reversed-Z made it affordable — see Core/Camera.hpp for why
        // the default is 50 km and why it used to be 1 km. A slider that stopped at 10 km would have made
        // the new default unreachable from the editor, which is a dead setting by another name.
        PROPERTY( DisplayName( "Far" ), Category( "Camera" ), Range( 1000.0f, 10000000.0f ), Length,
                  Tooltip( "Far clip plane distance. Anything beyond is not drawn." ) )
        float Far = Core::kDefaultFarPlane;
    };

    struct CameraComponent
    {
        COMPONENT( Key( "Camera" ), Block( Data ), Run( ActorsAndUI ) )
        std::shared_ptr<Core::Camera> Camera;
        CameraData                    Data;
    };

    class VisibilityComponent;
    bool SetVisible( entt::registry& registry, entt::entity entity, bool visible );

    // Whether the entity is drawn (UE USceneComponent::bVisible). READ-ONLY, for the reason TransformComponent
    // is: every write goes through ECS::SetVisible, a registry.patch, so on_update<VisibilityComponent> fires
    // and Core::Scene's render-state dirty set sees the toggle at the source. Construction with a value
    // (emplace<VisibilityComponent>( e, false )) is not a write: it fires on_construct.
    class VisibilityComponent
    {
    public:
        VisibilityComponent() = default;
        explicit VisibilityComponent( bool visible ) : m_Visible( visible ) {}

        [[nodiscard]] bool IsVisible() const
        {
            return m_Visible;
        }

    private:
        bool m_Visible = true;

        friend bool SetVisible( entt::registry& registry, entt::entity entity, bool visible );
    };

    // THE ONE WRITER of VisibilityComponent (UE USceneComponent::SetVisibility -> MarkRenderStateDirty). Adds
    // the component when the entity has none (on_construct), patches it when the value changes (on_update), and
    // is no write at all when the value is unchanged. Returns whether anything changed. Scene::SetVisibleRecursive
    // calls it per entity of a subtree.
    inline bool SetVisible( entt::registry& registry, entt::entity entity, bool visible )
    {
        if ( auto* component = registry.try_get<VisibilityComponent>( entity ) )
        {
            if ( component->m_Visible == visible )
                return false;
            registry.patch<VisibilityComponent>( entity, [visible]( VisibilityComponent& c )
                                                 { c.m_Visible = visible; } );
            return true;
        }
        registry.emplace<VisibilityComponent>( entity, visible );
        return true;
    }

    // One overridden material parameter (keyed by the shader's Properties name). vec4 stores any
    // scalar/vector value (float uses .x, color uses rgba). Data-driven: NOT a fixed C++ field per param.
    struct MaterialParamOverride
    {
        std::string Name;
        glm::vec4   Value = glm::vec4( 0.0f );

        [[nodiscard]] bool operator==( const MaterialParamOverride& ) const = default;
    };

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
            && ScriptSlotParams == desc.ScriptSlotParams;
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
                                            c.m_RuntimeMaterialInstances.clear();
                                            c.m_RuntimeSlots.reset();
                                            c.m_SeenMaterialsVersion = 0;
                                        }
                                        c.Assign( desc );
                                    } );
            return true;
        }
        registry.emplace<StaticMeshComponent>( entity, desc );
        return true;
    }

    // World-space SDF text. FontService bakes the .ttf into an SDF atlas; TextECSSystem lays the
    // string out into a per-entity quad mesh and draws it through the generic path with the TextSDF
    // shader — into the HDR composite, so EmissiveIntensity > ~1 blooms like any emissive surface.
    struct TextComponent
    {
        std::string         Text = "Text";
        Assets::AssetHandle Font; // SDF font asset (drag a .ttf or pick a preloaded one);
                                  // unset = the engine's built-in default (Roboto).
        glm::vec4 Color             = glm::vec4( 1.0f );
        float     Size              = 1.0f;  // world units per em (scales the baked metrics)
        float     EmissiveIntensity = 1.0f;  // >1 => the text blooms
        bool      Billboard         = false; // face the camera (added by the system per frame)

        // Transient: the laid-out glyph-quad mesh, rebuilt only when the text/font/size changes.
        std::shared_ptr<DynamicMesh> RuntimeMesh;
        std::string                  BuiltText;
        std::string                  BuiltFont; // resolved ttf path used for the current mesh (font-change guard)
        float                        BuiltSize = 0.0f;
    };

    // SkinnedMeshComponent's authored state as a value (field meanings on the component below).
    struct SkinnedMeshDesc
    {
        Assets::AssetHandle MeshHandle;
        std::vector<Assets::AssetHandle> MaterialSlots;
        bool CastShadows = true;
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
        uint32_t m_SeenMaterialsVersion = 0; // see StaticMeshComponent
        bool     CastShadows          = true; // false = skipped by the shadow (depth) passes, like the static twin

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
        desc.RuntimeMesh = RuntimeMesh;
        desc.RuntimeSkeleton = RuntimeSkeleton;
        return desc;
    }

    inline bool SkinnedMeshComponent::Matches( const SkinnedMeshDesc& desc ) const
    {
        return MeshHandle == desc.MeshHandle
            && MaterialSlots == desc.MaterialSlots
            && CastShadows == desc.CastShadows
            && RuntimeMesh == desc.RuntimeMesh
            && RuntimeSkeleton == desc.RuntimeSkeleton;
    }

    inline void SkinnedMeshComponent::Assign( const SkinnedMeshDesc& desc )
    {
        MeshHandle = desc.MeshHandle;
        MaterialSlots = desc.MaterialSlots;
        CastShadows = desc.CastShadows;
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
                                            c.m_RuntimeMaterialInstances.clear();
                                            c.m_RuntimeDisplacedInstances.clear();
                                            c.m_SeenMaterialsVersion = 0;
                                        }
                                        c.Assign( desc );
                                    } );
            return true;
        }
        registry.emplace<SkinnedMeshComponent>( entity, desc );
        return true;
    }

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
                                            c.m_RuntimeMaterialInstances.clear();
                                            c.m_RuntimeDisplacedInstances.clear();
                                            c.m_SeenMaterialsVersion = 0;
                                        }
                                        // A new mesh source (asset or built-in shape) makes the generated
                                        // mesh describe the old one: drop it, the mesh system resolves the new.
                                        if ( c.GetMeshHandle() != desc.MeshHandle ||
                                             c.GetPrimitive() != desc.Primitive )
                                            c.m_RuntimeMesh.reset();
                                        c.Assign( desc );
                                    } );
            return true;
        }
        registry.emplace<InstancedStaticMeshComponent>( entity, desc );
        return true;
    }

    // A FOLIAGE FIELD (UE: one FFoliageInfo of an AInstancedFoliageActor). Sits beside an
    // InstancedStaticMeshComponent that holds the painted instances; WHAT is painted — the mesh and the scatter
    // numbers — is the `.defoliage` named here (Assets::FoliageTypeAsset), shared by every field painted with
    // the same type. Saved as {FoliageTypeGuid, FoliageTypePath}; a type the project does not have refuses the
    // scene's load with both (ComponentRegistry.cpp) instead of painting with defaults.
    struct FoliageComponent
    {
        Assets::AssetHandle FoliageType;
    };

    // One binding of the sequence re-pointed at another entity of THIS scene (UE: a binding override on
    // ALevelSequenceActor). The `.dseq` stays the same file for every actor that plays it.
    struct LevelSequenceBindingOverride
    {
        Animation::Timeline::BindingGuid Binding;
        Common::UUID                     Entity = Common::UUID::Null();
    };

    // A LEVEL SEQUENCE ACTOR (UE: ALevelSequenceActor + FMovieSceneSequencePlaybackSettings). Plays the
    // `.dseq` named by `Sequence` in Play (ECS/System/LevelSequenceSystem.hpp); its Entity bindings name
    // entities of this scene by UUID. Saved as {SequenceGuid, SequencePath, Loop, AutoPlay, PlayRate,
    // BindingOverrides} (ComponentRegistry.cpp); a sequence the project does not have refuses the load with both.
    struct LevelSequenceComponent
    {
        Assets::AssetHandle                       Sequence;
        Animation::Timeline::LoopMode             Loop     = Animation::Timeline::LoopMode::Once;
        bool                                      AutoPlay = true;
        // Sequence seconds per game second (UE: FMovieSceneSequencePlaybackSettings::PlayRate). Read every
        // frame by ApplyLevelSequencePlaySettings, so a change while playing takes effect on the next step; a
        // negative rate plays backwards, 0 holds the current frame.
        double                                    PlayRate = 1.0;
        std::vector<LevelSequenceBindingOverride> BindingOverrides;
    };

    // THE LOCAL PLAYER'S INPUT CONTEXTS (GP1b; UE: the AddMappingContext calls a pawn makes on its local
    // player's UEnhancedInputLocalPlayerSubsystem at BeginPlay). When Play begins, ScriptSystem's
    // Input::LocalPlayerInput adds every context named here to the player's EnhancedInputSubsystem; scripts
    // add and remove more at run time (Lua Input.addContext / removeContext).
    struct EnhancedInputPlayerData
    {
        REFLECT()

        // The `.deinputcontext`s, HIGHEST PRIORITY FIRST: entry i is added at BasePriority + (count - 1 - i),
        // so the list reads top-down the way the stack consumes keys. The order is the priority because a
        // reflected vector holds handles, not (handle, priority) pairs, and two parallel vectors would be two
        // answers to one question.
        PROPERTY( DisplayName( "Mapping Contexts" ), Category( "Input" ), Asset<InputMappingContextAsset> )
        std::vector<Assets::AssetHandle> Contexts;

        // Where this list sits among contexts scripts add (Input.addContext's priority): the LAST entry gets
        // exactly this priority.
        PROPERTY( DisplayName( "Base Priority" ), Category( "Input" ) )
        int32_t BasePriority = 0;
    };

    struct EnhancedInputPlayerComponent
    {
        COMPONENT( Key( "EnhancedInputPlayer" ), Block( Data ), Run( ActorsAndUI ) )
        EnhancedInputPlayerData Data;
    };

    // HOW A LANDSCAPE LOOKS (UE: ALandscape::LandscapeMaterial), on the root entity beside its
    // LandscapeComponent. Apart from the frame because the frame is authored as raw numbers
    // (MakeAuthored) and this is reflected: one asset handle the Details panel builds.
    struct LandscapeMaterialData
    {
        REFLECT()

        // The landscape's material, a `.demat` of domain Terrain like every other material — the surface
        // is drawn by ONE program whose parameters (Tint) this material sets. The ground's layers are the
        // root's painted layer infos (LandscapeComponent::Layers), never built-in ones: where nothing is
        // painted the first layer shows, as UE's landscape does. One handle and not a slot vector: a vector
        // would promise a material per layer, and nothing downstream could consume one. Unset = the
        // shader's own schema defaults.
        //
        // Read by Engine/ECS/System/LandscapeECSSystem.cpp, which resolves it through
        // Runtime::MaterialService and forwards the values as named overrides on every tile of the root.
        //
        // Hidden from the auto-built Details: the builder's asset slot is texture-oriented, and the
        // Landscape Material entry draws a material field with an Edit button that opens the Material
        // Editor window. Still serialized; Hidden is editor-only.
        PROPERTY( DisplayName( "Material" ), Category( "Landscape" ), Asset<MaterialAsset>, Hidden )
        Assets::AssetHandle Material;
    };

    struct LandscapeMaterialComponent
    {
        COMPONENT( Key( "LandscapeMaterial" ), Block( Data ), Run( Landscape ) )
        LandscapeMaterialData Data;
    };

    // THE LANDSCAPE ROOT (UE: ALandscape). Owns the frame every tile is placed by: the entity's own world
    // position is sample (0, 0) of tile (0, 0), and these three say how far apart samples are, how tall a
    // sample step is and how many quads a tile has. It owns NO heights and NO tile list — the tiles are the
    // entities whose LandscapeTileComponent names this entity, and a count here would be a second answer
    // (World/Landscape/LandscapeLayout.hpp says what is deliberately not stored).
    //
    // Rotation and scale of the root entity are not part of the frame: LandscapeFrame has no rotation, as
    // the TES sampling it feeds has none. That is stated here rather than hidden behind a transform the
    // tiles would silently ignore; a landscape that must turn is a new frame field, not a gizmo.
    struct LandscapeComponent
    {
        uint32_t QuadsPerTile = World::Landscape::kLandscapeDefaultTileQuads; // UE section size, 7..255
        float    SpacingCm    = World::Landscape::kLandscapeDefaultSpacingCm; // cm between neighbouring samples
        float    ZScale       = World::Landscape::kLandscapeDefaultZScale;    // cm per local height unit
        // UE target layers, in panel order: each names a `.delayerinfo` (UE: ALandscape's target layer ->
        // ULandscapeLayerInfoObject). Every tile's weight plane is keyed by the asset's LayerName, and the
        // paint stroke reads its Hardness/NoWeightBlend (Runtime::LandscapeLayerInfoService resolves them).
        std::vector<Assets::AssetHandle> Layers;
        // UE edit layers (ALandscape's stack), bottom first: order, name, visibility, lock and alphas. Each
        // layer's data lives on the tiles it touched (LandscapeTileData::EditLayers, in the DLHT blob), and a
        // tile's samples are their merge (MergeLandscapeEditLayers).
        World::Landscape::LandscapeEditLayerStack EditLayers;
        // UE ULandscapeSplinesComponent: the landscape's spline control points and segments, world cm. They
        // write the stack's Splines edit layer (RasterizeLandscapeSplines); the heights they make are that
        // layer's data on the tiles, so a cooked landscape needs only the merged heights.
        World::Landscape::LandscapeSplines Splines;
    };

    // ONE TILE OF A LANDSCAPE (UE: ALandscapeStreamingProxy of one component).
    //
    // `Landscape` names the root BY ID AND NOT AS A PARENT, and that is the whole reason this is a field:
    // a parent link is containment, and WorldPartition keeps a composite whole in one cell — every tile of
    // a landscape would be one composite and one cell would grow to hold the entire terrain. As an
    // OBSERVATION (WorldPartitionRules.hpp, kEntityReferences) each tile is its own composite, placed by its
    // own rectangle; a tile whose root is not loaded simply has no frame yet.
    //
    // `HeightFile` is where the heights live — a DLHT blob beside the scene, never inside the .desce: a
    // 64x64 tile is 8 KiB of samples and JSON would carry it as text, eleven times over. The path is
    // written as the scene file spells its other files (relative to the working directory), and it is
    // RE-DERIVED on every save from the destination (LandscapeTileFiles.hpp, LandscapeTileBlobPath), so "Save As"
    // never writes one scene's heights into another's files.
    //
    // `Heights` is the loaded tile — the single source of truth for this tile's terrain (landscape
    // analysis A2). Runtime state: it is not in the scene block, it is what the block's file decodes to.
    // HELD BY VALUE, because the component is its one owner (PointerOwnership's Q1): a pointer here would
    // answer a sharing question nobody has asked yet. A consumer that must keep the tile across frames
    // (the GPU copy, LS-4) cannot keep an address into an entt pool anyway, and decides its own form then.
    struct LandscapeTileComponent
    {
        Common::UUID Landscape = Common::UUID::Null(); // the root entity; null = no frame
        int32_t      TileX     = 0;                    // which tile of the root's grid, either side of it
        int32_t      TileZ     = 0;
        std::string  HeightFile;

        std::optional<World::Landscape::LandscapeTileData> Heights;
    };

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
        MaterialComponent() = default;
        explicit MaterialComponent( MaterialDesc desc ) : m_Desc( std::move( desc ) ) {}

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

    private:
        MaterialDesc m_Desc;

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
            registry.patch<MaterialComponent>( entity, [&desc]( MaterialComponent& c ) { c.m_Desc = desc; } );
            return true;
        }
        registry.emplace<MaterialComponent>( entity, desc );
        return true;
    }

    struct AnimationComponent
    {
        // active Animator (runtime instance)
        std::unique_ptr<Animation::Animator> Animator;

        // current name (debug / editor)
        std::string CurrentClip;

        bool Playing = true;
        bool Loop    = true;

        float PlaybackSpeed = 1.0f;

        /**
         * @brief UE's `bUpdateAnimationInEditor`: whether this component advances in the EDITOR world (Edit
         *        mode). Off by default, as in UE — an edited level holds still while it is being laid out;
         *        Play always advances. AUTHORED (scene + prefab block); read by AnimationECSSystem through
         *        Animation::AnimationAdvanceSeconds. Scrubbing Time in Details poses the character either way.
         */
        bool UpdateAnimationInEditor = false;

        /**
         * @brief UE ERootMotionMode (Animation/RootMotion.hpp). AUTHORED (scene + prefab block, by name; absent
         *        = NoExtraction, UE's default). With RootMotionFromEverything the Animator locks the root bone
         *        and AnimationECSSystem hands its movement, in world space, to the CharacterControllerComponent
         *        of the entity's parent — consumed by CharacterMovement::Step as the planar velocity, UE
         *        PhysWalking's root-motion velocity — or, with no controller, moves this entity's
         *        TransformComponent itself. Play (and UpdateAnimationInEditor) only: a scrub moves nothing.
         */
        Animation::RootMotionMode RootMotion = Animation::RootMotionMode::NoExtraction;

        // Notify events of the Animator THIS frame (instant markers crossed, notify states begun / ended).
        // Filled by AnimationECSSystem, drained + dispatched to the entity's scripts (OnAnimationNotify /
        // OnAnimationNotifyBegin / OnAnimationNotifyEnd) by ScriptSystem. Transient.
        std::vector<Animation::NotifyEvent> PendingNotifies;

        /**
         * @brief The `.danimgraph` this entity plays — a data-driven state machine that PICKS the clip
         *        from live parameters. AUTHORED, and the only half of the graph a scene file states.
         *
         * IT USED TO BE A JSON STRING INSIDE THE ENTITY. `AnimationComponentSer::GraphJson` carried
         * `Animation::Graph::Serialize(*Graph)` verbatim, so the graph had no identity of its own: two
         * characters could not share one walk graph, and copying it copied a blob that then drifted.
         *
         * AND THE COMMENT THAT STOOD HERE WAS FALSE BY THE TIME IT MATTERED. It said the schema question
         * could wait because "today the repository has zero such scenes and the window is open" — the
         * repository had 10 scenes carrying an `Animation` block and 6 non-empty `GraphJson` blobs across
         * three of them, all three load-bearing for a test suite. The window had closed; the step that
         * closed it is `kSceneVersionAnimGraphAsset` (21) and it converted those three.
         */
        Assets::AssetHandle GraphAsset;

        /**
         * @brief The graph object itself. TRANSIENT, and SHARED with every other entity naming the same
         *        file: it is `AnimGraphAsset`'s own `shared_ptr`, handed over by AnimationECSSystem.
         *
         * NOT A COPY, deliberately. A copy per entity would mean an edit in the Anim Graph window reached
         * exactly one of the characters using that graph, which is the defect the asset was made to end.
         * The EVALUATORS are per entity — each holds its own copy of the graph and its own live parameter
         * values — so two characters share a graph and still stand in different states.
         */
        std::shared_ptr<Animation::Graph::AnimGraph> Graph;
        std::shared_ptr<Animation::Graph::Evaluator> GraphEvaluator; // transient runtime state

        /**
         * @brief Parameter writes a script has asked for and the graph has not consumed yet. TRANSIENT.
         *
         * WHY AN INTENT AND NOT A DIRECT WRITE INTO THE EVALUATOR. The evaluator does not exist until
         * AnimationECSSystem has seen this entity WITH a loaded skinned mesh — so a script setting a
         * parameter in `OnStart`, or on any frame before an async mesh load finishes, would be writing into
         * a null. Swallowing that is the silent no-op this whole task is about; refusing it would make the
         * feature depend on asset timing the script author cannot see. The queue makes the write survive
         * until there is something to apply it to.
         *
         * It is also the shape this tree already uses for script intent —
         * `CharacterControllerComponent::MoveInput` / `JumpRequested` are written by Lua and executed by a
         * system that runs later — so the ordering argument is made once, in one place, for both.
         *
         * THE NAME AND THE TYPE ARE VALIDATED AT THE WRITE, not here: the refusal has to name the script
         * that made the mistake, and by the time this queue is drained that caller is gone.
         *
         * IT CARRIES NO TYPE, and that is deliberate rather than economical. A type field here would be a
         * SECOND statement about what a parameter is, and the graph already makes the first one
         * (`Parameter::Type`, chosen by an artist in the panel). The drain reads the declaration it is
         * about to write into, so the two cannot drift; a copy travelling in the queue could.
         */
        struct PendingGraphParam
        {
            std::string Name;
            float       Value = 0.0f; // bool as 0/1, int as a whole number — read through the declaration
        };
        std::vector<PendingGraphParam> PendingGraphParams;

        /**
         * @brief The `.danimgraph`s whose implemented layers answer this entity's LinkedAnimLayer nodes, in
         *        link order (UE: the Default Linked Layers of the AnimBP plus what LinkAnimClassLayers /
         *        UnlinkAnimClassLayers did since). AUTHORED — the scene states it — and the one list both the
         *        Details default and a script's `linkAnimLayers` write, so "what is linked" has one home.
         *
         * Applied by AnimationECSSystem whenever the entity's pose graph is set or the list / a listed
         * graph changes: every link is undone and the list is linked again in order, so a later entry
         * replaces an earlier one's interfaces exactly as a later LinkAnimClassLayers does. A GUID and not
         * a name, because a link is identified by its graph (UE: its class) and two files may share a name.
         */
        std::vector<Assets::AssetHandle> LinkedLayerGraphs;

        /// What the Animator's links were last built from: per entry of LinkedLayerGraphs its GUID, graph
        /// object and asset revision. TRANSIENT, plain numbers (not references; see BuiltGraphSource).
        struct AppliedLayerLink
        {
            uint64_t                           Guid                                        = 0;
            const Animation::Graph::AnimGraph* Graph                                       = nullptr;
            uint32_t                           Revision                                    = 0;
            bool                               operator==( const AppliedLayerLink& ) const = default;
        };
        std::vector<AppliedLayerLink> AppliedLayerLinks;

        /**
         * @brief What this entity's evaluator was built FROM. TRANSIENT, and the same shape as
         *        BuiltRigSource/BuiltRigRevision below.
         *
         * THERE IS NO `GraphRevision` ON THE COMPONENT ANY MORE, and its removal is the point. It was
         * bumped by whoever edited the graph — which could only ever be the component in front of the
         * editor, so a graph shared by two characters would have re-synced ONE of them and left the other
         * evaluating the previous shape with no sign that anything was stale. The counter belongs to the
         * thing that changes: `AnimGraphAsset::GetRevision()`, one number for every entity that names it.
         */
        // A PLAIN uint64 AND NOT AN AssetHandle, exactly like BuiltRigSource below, and the census is what
        // insists on it: an `AssetHandle` on a component is a REFERENCE the eviction root walk must mark
        // (Desert/Tests/Engine/AssetRoots names any it cannot find). This field is not a reference — it is
        // a fingerprint of the last build, and marking it would keep a graph alive after the slot that
        // named it was cleared. The authored reference is `GraphAsset` above, and that one IS a root.
        uint64_t BuiltGraphSource   = 0; // the handle the evaluator was built from
        uint32_t BuiltGraphRevision = 0; // the asset revision it was built at

        /**
         * @brief What the Animator's CURRENT control-rig stage was built from. TRANSIENT, and the same
         *        shape as BuiltGraphRevision above.
         *
         * A `ControlRigStage` holds bone INDICES resolved against one skeleton, out of a file whose every
         * reference is a NAME. Rebuilding it per frame would throw away the animator's live control poses
         * sixty times a second — the manipulator would be undraggable — and resolving nothing would leave a
         * hot-reloaded rig, a re-pointed slot or a swapped mesh silently running the old rig.
         *
         * So the three things a build depends on are remembered, and a rebuild happens when any of them
         * moves: the handle (the author picked another rig), the asset's revision (the file was edited on
         * disk) and the skeleton's signature (this entity's mesh changed under it).
         *
         * It lives HERE and not beside the handle in ControlRigComponent because it describes the live
         * Animator, which is this component's own property: `ControlRigComponent` is authored data that
         * undo rewrites, duplicate copies and the prefab path rebuilds, and a copied stamp would tell a
         * duplicated entity that a stage it does not have is up to date.
         */
        // A PLAIN INTEGER AND NOT AN `AssetHandle`, and the type is the statement: this is an IDENTITY
        // STAMP, not a reference. Nothing dereferences it, and nothing must keep the rig resident on its
        // account — the component's own `ControlRigData::Rig` is the reference, and SceneAssetRoots marks
        // that one. Spelt as a handle it was indistinguishable from a second, unmarked reference, and
        // Tests/Engine/AssetRoots said so by name on the first sweep.
        uint64_t BuiltRigSource    = 0;
        uint32_t BuiltRigRevision  = 0;
        uint64_t BuiltRigSignature = 0;

        /**
         * @brief The signature of the skeleton `Animator` was constructed on. TRANSIENT, same shape as the
         *        stamps above.
         *
         * A reimport re-reads the rig AT THE SAME ADDRESS (SkeletonAsset::LoadFromFile), so the Animator's
         * `const Skeleton&` stays valid while its bind pose, pose buffers and clip bindings were all sized
         * from the OLD bone list. The address cannot tell a reimported rig from the one it was built on; the
         * signature can. AnimationECSSystem rebuilds the Animator when this differs (UE: the anim instance is
         * re-initialised when the skeleton changes).
         */
        uint64_t BuiltSkeletonSignature = 0;

        AnimationComponent() = default;

        explicit AnimationComponent( std::unique_ptr<Animation::Animator>&& animator )
             : Animator( std::move( animator ) )
        {
        }
    };

    /**
     * @brief TWO-BONE IK ON ONE LIMB OF THIS ENTITY'S RIG. The first skeletal control to reach a scene.
     *
     * REFLECTED, unlike AnimationComponent next door — which is hand-serialised because it owns an
     * `Animator`, a graph and a notify queue, none of which are values an artist types. This one is four
     * values an artist types, so it is a `Data` block and gets its Details page, its undo, its duplicate,
     * its prefab and its Lua binding from the same table as every other reflected component.
     *
     * THE COMPONENT IS THE AUTHORED DATA; THE ANIMATOR OWNS THE SOLVER. `AnimationECSSystem` copies these
     * four values into the entity's `TwoBoneIKControl` every frame and creates or drops that control as
     * this component appears or goes. The alternative — storing the control here — would put a live,
     * rig-resolved object into a struct that is copied by duplicate, written by undo and rebuilt by the
     * prefab path, and every one of those would carry bone indices resolved against a different rig.
     *
     * It follows `AnimationComponent::Playing`, because that flag gates the whole pose pipeline for the
     * entity: with playback stopped the Animator is not updated at all and no stage runs, this one
     * included.
     */
    struct TwoBoneIKData
    {
        REFLECT()

        // THE ONLY AUTHORED BONE. The chain is this bone, its parent and its grandparent — UE authors it
        // the same way and has no root-bone pin at all. Naming both ends would let an artist name two
        // bones that are not related, and there is nothing to be done about that except detect it; this
        // way the invalid case cannot be typed.
        PROPERTY( DisplayName( "End Bone" ), Category( "Two-Bone IK" ),
                  Tooltip( "The hand/foot bone. Its parent and grandparent become the two limbs" ) )
        std::string EndBone;

        // COMPONENT (MESH-LOCAL) SPACE, CENTIMETRES — the space the pose itself is resolved in, so no
        // conversion stands between what is typed here and what the solver reads. A world-space goal would
        // need the entity's transform, which is a second source of truth for where the goal is whenever
        // the entity moves.
        PROPERTY( DisplayName( "Goal" ), Category( "Two-Bone IK" ),
                  Tooltip( "Where the end bone should land, in mesh-local centimetres" ) )
        glm::vec3 Goal = glm::vec3( 0.0f );

        // A POINT the joint bends towards, not a direction (report 03 §785) — so it can become a bone or a
        // socket later without this field changing meaning. On the root->goal line it names no plane, and
        // the solver then keeps the pose's own bend rather than inventing an axis.
        PROPERTY( DisplayName( "Pole Target" ), Category( "Two-Bone IK" ),
                  Tooltip( "Point the elbow/knee bends towards, in mesh-local centimetres" ) )
        glm::vec3 PoleTarget = glm::vec3( 0.0f );

        // 0 = the animation's own pose, 1 = the solve. Blended in LOCAL space by the control base, so half
        // is a valid pose and not a sheared chain. There is deliberately no separate "Enabled": alpha 0 is
        // off, and two knobs for one fact is a disagreement waiting to be authored.
        PROPERTY( DisplayName( "Alpha" ), Category( "Two-Bone IK" ), Range( 0.0f, 1.0f ),
                  Tooltip( "How far from the animated pose towards the solved one" ) )
        float Alpha = 1.0f;
    };

    struct TwoBoneIKComponent
    {
        COMPONENT( Key( "TwoBoneIK" ), Block( Data ), Run( ActorsAndUI ) )
        TwoBoneIKData Data;
    };

    /**
     * @brief THE CONTROL RIG THIS ENTITY IS POSED BY. The thing that makes tier T5 reachable from a scene.
     *
     * T5 shipped a control hierarchy, a manipulator, keying and a pipeline stage, all proven by suites, and
     * NOT ONE SCENE COULD HAVE A RIG: `Animator::AttachRig` takes a `ControlRigStage` somebody has to build
     * in C++, and nobody did. This component is the "somebody" — one authored value, an asset handle, from
     * which `AnimationECSSystem` builds the stage against this entity's own skeleton every time the handle
     * or the file behind it changes.
     *
     * THE COMPONENT IS THE AUTHORED DATA; THE ANIMATOR OWNS THE STAGE, exactly as `TwoBoneIKData` next door
     * splits them and for the identical reason: a `ControlRigStage` holds bone indices resolved against one
     * skeleton, and this struct is copied by duplicate, rewritten by undo and rebuilt by the prefab path.
     * A duplicated entity with a different mesh would inherit indices into a skeleton it does not have.
     *
     * THERE IS DELIBERATELY NO ALPHA. `ControlRigStage::Evaluate` applies its overrides at 1.0 and says
     * why: the rig IS the authored override, and a weight nothing sets is a knob for a knob's sake. An
     * empty handle is "no rig", which is the off switch, and it is the same one bit the stage membership
     * test reads.
     *
     * It follows `AnimationComponent::Playing` for `TwoBoneIKData`'s reason: with playback stopped the
     * Animator is not updated at all and no stage runs, this one included.
     */
    struct ControlRigData
    {
        REFLECT()

        // THE ONLY AUTHORED VALUE. Empty = this entity has no rig, which is how the stage is turned off
        // without a second flag that could disagree with it.
        PROPERTY( DisplayName( "Rig" ), Category( "Control Rig" ), Asset<ControlRigAsset>,
                  Tooltip( "The .derig whose controls pose this entity's skeleton" ) )
        Assets::AssetHandle Rig;
    };

    struct ControlRigComponent
    {
        COMPONENT( Key( "ControlRig" ), Block( Data ), Run( ActorsAndUI ) )
        ControlRigData Data;
    };

    /**
     * @brief THE RIG PAIR THIS ENTITY PLAYS ITS CLIPS THROUGH. What makes tier T6.2 reachable from a scene.
     *
     * T6.2 shipped a three-stage retargeting pipeline measured against `JPH::SkeletonMapper` on the same
     * rigs and clips — worst limb-length error 0.000024 % against 7.934 %, a pelvis that rises by the ratio
     * of the two rigs' heights instead of by 1.00 — and NOT ONE SCENE COULD USE IT: `Retargeter::Initialize`
     * takes a `RetargetSetup` somebody has to fill in in C++, and nobody did. This component is the
     * "somebody", exactly as `ControlRigData` next door is for T5.
     *
     * ONE AUTHORED VALUE, AND THE PAIR IS NOT IN IT. A retarget is a statement about two rigs; the TARGET
     * rig is this entity's own mesh, and the SOURCE rig is named by the `.retarget` file, by signature
     * (see Engine/Assets/Serialization/Retarget.hpp). Authoring the source rig here as a second handle
     * would let the same file be pointed at a rig it was not authored against — accepted whenever the bone
     * names happened to resolve, and wrong by whatever the proportions differ by. One value, no way to
     * write a disagreement.
     *
     * THE COMPONENT IS THE AUTHORED DATA; THE ANIMATOR OWNS THE RETARGETER, for `ControlRigData`'s reason
     * and one more of its own: a `Retargeter` caches bone indices for BOTH rigs and a copy of the source
     * skeleton, and this struct is copied by duplicate, rewritten by undo and rebuilt by the prefab path.
     *
     * THERE IS DELIBERATELY NO ALPHA AND NO "SOURCE CLIP" FIELD. An empty handle is "no retarget", which
     * is the off switch and the same one bit the Animator's membership test reads; and which clip plays is
     * already `AnimationComponent::CurrentClip` — a second name for it here would be two statements about
     * one fact.
     *
     * It follows `AnimationComponent::Playing` for `TwoBoneIKData`'s reason: with playback stopped the
     * Animator is not updated at all and no source stage runs, this one included.
     */
    struct RetargetData
    {
        REFLECT()

        // THE ONLY AUTHORED VALUE. Empty = this entity's clips are on its own rig, which is how the
        // retarget is turned off without a second flag that could disagree with it.
        PROPERTY( DisplayName( "Retarget" ), Category( "Retarget" ), Asset<RetargetAsset>,
                  Tooltip( "The .retarget whose rig pair this entity's clips are played through" ) )
        Assets::AssetHandle Retarget;
    };

    struct RetargetComponent
    {
        COMPONENT( Key( "Retarget" ), Block( Data ), Run( ActorsAndUI ) )
        RetargetData Data;
    };

    // Blendshape / morph-target weights for the entity's mesh. Weight k (0..1, though over/undershoot is
    // allowed) scales morph target k of the mesh asset; the blend is base + Σ(weight·delta) BEFORE skinning
    // (UE GPUMorphUpdateCS: Programs/Compute/MorphTargets.shader, run by MeshRenderer's morph update for a
    // skinned mesh; Geometry::ApplyMorphTargets is its CPU twin). TargetNames mirrors the mesh's target names
    // for the Details UI and stays index-aligned with the weights. Non-reflected (like AnimationComponent) —
    // edited via the Morph widget.
    //
    // The weights are CLOSED: they are written only through SetWeight / SetWeights (UE
    // USkinnedMeshComponent::SetMorphTarget), and the setter itself moves Revision when a value changes. Every
    // writer — the Details sliders, ReadComponent, and any future Lua / Sequencer binding — goes through it,
    // so no writer can forget to tell the renderer.
    struct MorphComponent
    {
        std::vector<std::string> TargetNames;

        const std::vector<float>& GetWeights() const;
        // Weight of target @p index, or 0 when the component holds no weight for it.
        float GetWeight( std::size_t index ) const;
        // Sets the weight of target @p index, growing the list with zeros if it is shorter.
        void     SetWeight( std::size_t index, float weight );
        void     SetWeights( std::vector<float> weights );
        uint64_t GetRevision() const;

    private:
        std::vector<float> Weights;

        // THE REVISION OF Weights (UE FMorphVertexBuffer's revision number): moved by the setters on every
        // change, and the renderer re-blends a component's morphed vertices only when the revision it blended
        // last differs - one integer compare per frame instead of a copy of the weights. Transient (not
        // serialized): a freshly read component has no blend yet, so the renderer blends it once whatever the
        // number is.
        uint64_t Revision = 0;
    };

    inline const std::vector<float>& MorphComponent::GetWeights() const
    {
        return Weights;
    }

    inline float MorphComponent::GetWeight( std::size_t index ) const
    {
        return index < Weights.size() ? Weights[index] : 0.0f;
    }

    inline void MorphComponent::SetWeight( std::size_t index, float weight )
    {
        if ( index >= Weights.size() )
            Weights.resize( index + 1, 0.0f );
        else if ( Weights[index] == weight )
            return;
        Weights[index] = weight;
        ++Revision;
    }

    inline void MorphComponent::SetWeights( std::vector<float> weights )
    {
        if ( weights == Weights )
            return;
        Weights = std::move( weights );
        ++Revision;
    }

    inline uint64_t MorphComponent::GetRevision() const
    {
        return Revision;
    }

    // A parent-relative transform AS A VALUE: what a TransformComponent holds, what a caller builds before
    // handing it to SetLocalTransform, and what code that never touches a registry (gizmo maths, prefab
    // records, tests) passes around. UE's FTransform next to USceneComponent::RelativeLocation/Rotation/Scale.
    struct LocalTransform
    {
        glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Rotation    = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Scale       = { 1.0f, 1.0f, 1.0f };

        [[nodiscard]] glm::mat4 GetTransform() const
        {
            return glm::translate( glm::mat4( 1.0f ), Translation ) * glm::toMat4( glm::quat( Rotation ) ) *
                   glm::scale( glm::mat4( 1.0f ), Scale );
        }

        [[nodiscard]] bool operator==( const LocalTransform& other ) const
        {
            return Translation == other.Translation && Rotation == other.Rotation && Scale == other.Scale;
        }
        [[nodiscard]] bool operator!=( const LocalTransform& other ) const
        {
            return !( *this == other );
        }
    };

    class TransformComponent;
    bool SetLocalTransform( entt::registry& registry, entt::entity entity, const LocalTransform& local );

    // THE ENTITY'S LOCAL TRANSFORM, READ-ONLY. Nothing may write it in place: every write goes through
    // SetLocalTransform (below), which is a registry.patch, so EnTT's on_update<TransformComponent> fires for
    // every change and Core::Scene's render-state dirty set (UE MarkRenderStateDirty) sees it at the source —
    // a direct field write would move the entity on screen while the dirty set, and every persistent render
    // proxy fed from it, said nothing changed. Construction (emplace / AddComponent with a LocalTransform) is
    // not a write: it fires on_construct. Whole-object assignment stays possible only because EnTT's storage
    // moves components by assignment; replace a component with registry.replace, which fires on_update.
    class TransformComponent
    {
    public:
        TransformComponent() = default;
        TransformComponent( const LocalTransform& local ) : m_Local( local ) {}

        [[nodiscard]] const LocalTransform& GetLocal() const
        {
            return m_Local;
        }
        [[nodiscard]] const glm::vec3& GetTranslation() const
        {
            return m_Local.Translation;
        }
        [[nodiscard]] const glm::vec3& GetRotation() const
        {
            return m_Local.Rotation;
        }
        [[nodiscard]] const glm::vec3& GetScale() const
        {
            return m_Local.Scale;
        }
        [[nodiscard]] glm::mat4 GetTransform() const
        {
            return m_Local.GetTransform();
        }

    private:
        LocalTransform m_Local;

        friend bool SetLocalTransform( entt::registry& registry, entt::entity entity, const LocalTransform& local );
    };

    // THE ONE WRITER of a TransformComponent (UE USceneComponent::SetRelativeTransform, which ends in
    // MarkRenderTransformDirty). An unchanged value is not a write (UE early-outs the same way): no on_update,
    // so a system that re-asserts the same pose every frame (attachments, physics at rest) dirties nothing.
    // Returns whether the transform changed.
    inline bool SetLocalTransform( entt::registry& registry, entt::entity entity, const LocalTransform& local )
    {
        if ( registry.get<TransformComponent>( entity ).m_Local == local )
            return false;
        registry.patch<TransformComponent>( entity, [&local]( TransformComponent& component )
                                            { component.m_Local = local; } );
        return true;
    }

    // One field of the local transform; each is SetLocalTransform with the other two fields kept.
    inline bool SetLocalTranslation( entt::registry& registry, entt::entity entity, const glm::vec3& translation )
    {
        LocalTransform local = registry.get<TransformComponent>( entity ).GetLocal();
        local.Translation    = translation;
        return SetLocalTransform( registry, entity, local );
    }
    inline bool SetLocalRotation( entt::registry& registry, entt::entity entity, const glm::vec3& rotation )
    {
        LocalTransform local = registry.get<TransformComponent>( entity ).GetLocal();
        local.Rotation       = rotation;
        return SetLocalTransform( registry, entity, local );
    }
    inline bool SetLocalScale( entt::registry& registry, entt::entity entity, const glm::vec3& scale )
    {
        LocalTransform local = registry.get<TransformComponent>( entity ).GetLocal();
        local.Scale          = scale;
        return SetLocalTransform( registry, entity, local );
    }

    // ON THE TWO PAIRS OF SUN NUMBERS. Colour x Intensity here is the ILLUMINANCE arriving at scene
    // surfaces — what every lit surface integrates. SkyAtmosphereData::SunColor x SunIntensity is the
    // RADIANCE of the sky and of the solar disk — what the camera sees when it looks up. Two different
    // quantities with different consumers, not one value stored twice: neither is derived from the other,
    // and no code path reads one where it means the other. See SkyAtmosphereComponent.hpp.
    struct DirectionalLightData
    {
        REFLECT()

        PROPERTY( DisplayName( "Color" ), Category( "Light" ), Color, Temperature,
                  Tooltip( "Tint of the sun light (UE). With a Physical Atmosphere sky this is also the sun "
                           "the sky scatters and the disc you see." ) )
        glm::vec3 Color = glm::vec3( 1.0f );

        // UE UDirectionalLightComponent::Intensity: ILLUMINANCE in lux on a surface facing the light (a clear
        // midday sun is ~100 000 lux, an overcast day ~1 000, a full moon ~0.25). The renderer multiplies the BRDF
        // by Color x Intensity directly - lux IS its unit, the same unit a point light's candelas reach a surface
        // in (ECS::LightPhotometry) - and the exposure range (PostProcessSettings, EV100) is what makes a physical
        // value look right. Scenes authored before units existed stated the same number; it was always lux.
        // As in UE it is the OUTER-SPACE illuminance of the atmosphere sun: the Physical Atmosphere sky
        // scatters exactly this (Graphic::SkySunInputs), and the ground receives it times the transmittance.
        PROPERTY( DisplayName( "Intensity" ), Category( "Light" ), Range( 0.0f, 150000.0f ), Summary,
                  Units( "lux" ),
                  Tooltip( "Illuminance in lux on a surface facing the light (UE): ~100000 clear sun, ~1000 "
                           "overcast, ~0.25 full moon. With a Physical Atmosphere sky this also drives the "
                           "sky's brightness and the sun disc." ) )
        float Intensity = 1.0f;

        // Defaults to true so that every scene authored before this field existed keeps working: a field
        // missing from the file keeps its C++ default, so the one directional light such a scene has
        // becomes its atmosphere sun with no migration.
        PROPERTY( DisplayName( "Atmosphere Sun Light" ), Category( "Atmosphere" ),
                  Tooltip( "This light drives the sky and the sky's IBL bake." ) )
        bool AtmosphereSunLight = true;

        PROPERTY( DisplayName( "Atmosphere Sun Light Index" ), Category( "Atmosphere" ), Range( 0, 1 ),
                  EditCondition( "AtmosphereSunLight" ),
                  Tooltip( "0 = the sun, 1 = a second atmosphere light such as the moon (UE). Both light the "
                           "sky, draw a disc and light the scene; the time-of-day clock and the cascaded "
                           "shadows belong to index 0." ) )
        int AtmosphereSunLightIndex = 0;

        // UE's "Affected By Atmosphere Transmittance", same name, same default (ON): in
        // SkyModel::PhysicalAtmosphere the colour above is multiplied by the atmosphere's transmittance
        // toward the sun at ground level, so a sunset reddens and dims the light on geometry by the same
        // law that reddens the sky behind it. Consumed by SceneRenderer::OnUpdate through the
        // AtmosphereEnv::SunTransmittanceAtGround the SkyboxRenderer publishes.
        //
        // Switching it off returns the light to exactly its authored colour — the artist's escape from a
        // physical sun, and the ONLY way to keep an authored colour in the physical model.
        // SkyModel::ArtisticGradient ignores this field entirely: there the sky's radiance and the
        // surface illuminance are independent by documented design (SkyAtmosphereComponent.hpp), and
        // this coupling does not exist to be switched off.
        PROPERTY( DisplayName( "Affected By Atmosphere Transmittance" ), Category( "Atmosphere" ),
                  EditCondition( "AtmosphereSunLight" ),
                  Tooltip( "Multiply this light's colour by the atmosphere's transmittance toward the "
                           "sun at ground level, so it reddens and dims at sunset. Physical Atmosphere "
                           "only." ) )
        bool AffectedByAtmosphereTransmittance = true;

        // UE UDirectionalLightComponent::AtmosphereSunDiskColorScale, same name, same default (white): scales
        // ONLY the disc the physical sky draws for this sun — not the sky's scattering, not the surfaces.
        // Scenes from before v57, whose sky had its own sun numbers, state here what their disc was.
        PROPERTY( DisplayName( "Atmosphere Sun Disk Color Scale" ), Category( "Atmosphere" ), Color,
                  EditCondition( "AtmosphereSunLight" ),
                  Tooltip( "Scales the sun disc the Physical Atmosphere sky draws for this light (UE)." ) )
        glm::vec3 AtmosphereSunDiskColorScale = glm::vec3( 1.0f );

        // ---- Light Shafts (UE's category, UE's names, UE's defaults) ----------------------------------
        // The screen-space sun streaks: a bright-pass of the HDR scene around the sun's position on
        // screen, radially blurred toward it and added back before tonemapping. Occlusion is inherited
        // from the scene colour itself — whatever stands in front of the sun composites with its real
        // transmittance, so the shafts exist exactly where the sun breaks through. Consumed by
        // System::LightShaftRenderer via the SunLightFx slice of the ProceduralSkyCommand; only the
        // atmosphere sun's values are read.
        PROPERTY( DisplayName( "Light Shaft Bloom" ), Category( "Light Shafts" ),
                  Tooltip( "Radial streaks of the sun's light through gaps in whatever occludes it, "
                           "added to the scene before tonemapping." ) )
        bool LightShaftBloom = false;

        PROPERTY( DisplayName( "Bloom Scale" ), Category( "Light Shafts" ), Range( 0.0f, 10.0f ),
                  EditCondition( "LightShaftBloom" ), Tooltip( "Overall strength of the light-shaft bloom." ) )
        float BloomScale = 0.2f;

        PROPERTY( DisplayName( "Bloom Threshold" ), Category( "Light Shafts" ), Range( 0.0f, 4.0f ),
                  EditCondition( "LightShaftBloom" ),
                  Tooltip( "Scene luminance below this contributes nothing to the shafts." ) )
        float BloomThreshold = 0.0f;

        PROPERTY( DisplayName( "Bloom Max Brightness" ), Category( "Light Shafts" ), Range( 0.0f, 100.0f ),
                  EditCondition( "LightShaftBloom" ),
                  Tooltip( "Cap on the energy a single pixel may contribute — stops one blown-out pixel "
                           "from owning the whole streak." ) )
        float BloomMaxBrightness = 100.0f;

        PROPERTY( DisplayName( "Bloom Tint" ), Category( "Light Shafts" ), Color,
                  EditCondition( "LightShaftBloom" ), Tooltip( "Tint of the light-shaft streaks." ) )
        glm::vec3 BloomTint = glm::vec3( 1.0f );

        // THE CASCADED SHADOW MAPS OF THIS LIGHT (UE: UDirectionalLightComponent's Cascaded Shadow Maps
        // section). They were SceneSettings until SET1 — a level-wide switch for the shadows of the one
        // light that has any. Graphic::ResolveViewSettings reads them from the light it elects, the same
        // one Scene::OnUpdate shades with.
        PROPERTY( DisplayName( "Cast Shadows" ), Category( "Cascaded Shadow Maps" ) )
        bool CastShadows = true;

        // THE SUN'S ANGULAR DIAMETER (UE: UDirectionalLightComponent::LightSourceAngle, the same default). It is
        // what makes a shadow soft: the penumbra grows by tan(angle / 2) per centimetre between the occluder and
        // the receiver, so a shadow is contact-sharp where the caster meets the ground and widens with distance
        // (the cascades' PCSS, Mesh/CascadedShadow.glslh).
        PROPERTY( DisplayName( "Source Angle" ), Category( "Cascaded Shadow Maps" ), Range( 0.0f, 10.0f ),
                  Units( "deg" ),
                  Tooltip( "Angular diameter of the sun in degrees. Larger = softer shadows away from their "
                           "casters; 0 = hard shadows everywhere." ) )
        float SourceAngle = 0.5357f;

        // THE CASTER'S DEPTH BIAS, IN WORLD CENTIMETRES (UE: ULightComponent::ShadowBias / ShadowSlopeBias,
        // UDirectionalLightComponent::ShadowCascadeBiasDistribution). Applied where the caster writes its
        // depth (MeshRenderer::FitShadowCascades -> the caster matrix and CameraUB.ShadowSlopeBias), never
        // at the receiver: a bias in a cascade's NDC depth was 26 cm in the nearest cascade and 10 m in the
        // farthest, and erased every awning, lamp and window frame past the first. Both lengths are what
        // the NEAREST cascade gets; Distribution scales the farther ones by their texel size relative to it.
        PROPERTY( DisplayName( "Shadow Bias" ), Category( "Cascaded Shadow Maps" ), Range( 0.0f, 50.0f ),
                  Units( "cm" ),
                  Tooltip( "How far the caster's depth is pushed away from the sun, in the nearest cascade. "
                           "Larger hides self-shadow acne and detaches contact shadows." ) )
        float ShadowBias = 1.0f;

        PROPERTY( DisplayName( "Shadow Slope Bias" ), Category( "Cascaded Shadow Maps" ), Range( 0.0f, 50.0f ),
                  Units( "cm" ),
                  Tooltip( "Extra depth bias of a caster surface at 45 degrees to the sun (and steeper, "
                           "clamped there), in the nearest cascade." ) )
        float ShadowSlopeBias = 2.0f;

        PROPERTY( DisplayName( "Cascade Bias Distribution" ), Category( "Cascaded Shadow Maps" ),
                  Range( 0.0f, 1.0f ),
                  Tooltip( "0 = every cascade gets the same centimetres; 1 = a cascade's bias grows with its "
                           "texel size, as UE's Shadow Cascade Bias Distribution." ) )
        float ShadowCascadeBiasDistribution = 1.0f;

        PROPERTY( DisplayName( "Cascade Split Lambda" ), Category( "Cascaded Shadow Maps" ), Range( 0.0f, 1.0f ),
                  Tooltip( "0 = uniform cascade splits, 1 = logarithmic; UE's Distribution Exponent plays "
                           "the same part." ) )
        float CascadeSplitLambda = 0.6f;

        // CONTACT SHADOWS (UE: ULightComponent::ContactShadowLength / bContactShadowLengthInWS, the same default 0
        // = off). A short screen-space ray toward the light through the scene depth, marched by the deferred
        // composite (Common/ContactShadow.glslh), that grounds the small and near-contact detail the shadow map is
        // too coarse or too biased to hold. Needs Cast Shadows; its step count is Scalability
        // Shadows.ContactShadowSteps.
        PROPERTY( DisplayName( "Contact Shadow Length" ), Category( "Contact Shadows" ), Range( 0.0f, 100.0f ),
                  Tooltip( "Length of the screen-space shadow ray toward the sun: a fraction of the screen "
                           "height at the receiver (0.1 = a tenth), or centimetres when In World Space. "
                           "0 = no contact shadows." ) )
        float ContactShadowLength = 0.0f;

        PROPERTY( DisplayName( "Contact Shadow Length In World Space" ), Category( "Contact Shadows" ),
                  Tooltip( "Contact Shadow Length is in world centimetres instead of a fraction of the screen." ) )
        bool ContactShadowLengthInWorldSpace = false;

        // UE ULightComponent::ContactShadowCastingIntensity / ContactShadowNonCastingIntensity: how dark the
        // contact shadow is where the ray hits a mesh that casts contact shadows (its Cast Contact Shadow flag),
        // and where it hits one that does not. UE's defaults: 1 and 0.
        PROPERTY( DisplayName( "Contact Shadow Casting Intensity" ), Category( "Contact Shadows" ),
                  Range( 0.0f, 1.0f ),
                  Tooltip( "Occlusion of a contact shadow cast by a mesh whose Cast Contact Shadow is on "
                           "(1 = black, 0 = none)." ) )
        float ContactShadowCastingIntensity = 1.0f;

        PROPERTY( DisplayName( "Contact Shadow Non Casting Intensity" ), Category( "Contact Shadows" ),
                  Range( 0.0f, 1.0f ),
                  Tooltip( "Occlusion where the ray hits a mesh whose Cast Contact Shadow is off "
                           "(0 = such a mesh casts none)." ) )
        float ContactShadowNonCastingIntensity = 0.0f;

        PROPERTY( DisplayName( "Volumetric Scattering Intensity" ), Category( "Light" ), Range( 0.0f, 10.0f ),
                  Tooltip( "How much of this light the volumetric fog scatters (UE ULightComponent::"
                           "VolumetricScatteringIntensity). 0 = the light leaves no shaft or halo in fog." ) )
        float VolumetricScatteringIntensity = 1.0f;
    };

    struct DirectionLightComponent
    {
        COMPONENT( Key( "DirectionLight" ), Block( Data ), Run( ActorsAndUI ) )
        DirectionalLightData Data;
    };

    // LightFalloff and LightUnits (UE ELightUnits) live in Engine/ECS/LightUnits.hpp, next to the one conversion
    // the renderer applies (LightPhotometry::ShaderIntensity).

    struct PointLightData
    {
        REFLECT()

        PROPERTY( DisplayName( "Color" ), Category( "Light" ), Color, Temperature )
        glm::vec3 Color = glm::vec3( 1.0f );

        // UE ULocalLightComponent::Intensity / IntensityUnits: the value in the units beside it (default 8 cd, as
        // UE). Switching the units in Details re-states the value so the brightness holds (EditorPostEdit); the
        // renderer reads it only through LightPhotometry::ShaderIntensity. Physical units need InverseSquare.
        PROPERTY( DisplayName( "Intensity" ), Category( "Light" ), Range( 0.0f, 100000.0f ), Summary,
                  Tooltip( "Brightness in Intensity Units. Candelas: 1 cd lights a surface 1 m away with 1 lux; "
                           "Lumens: total flux (a 60 W bulb ~800 lm, a street lamp ~2500 lm); EV100: 2^(EV-3) cd; "
                           "Unitless: the renderer's multiplier at Min Radius. Physical units apply to "
                           "Inverse Square falloff only." ) )
        float Intensity = 8.0f;

        PROPERTY( DisplayName( "Intensity Units" ), Category( "Light" ),
                  Tooltip( "What Intensity is measured in (UE Intensity Units). Changing it converts the value "
                           "so the light keeps its brightness." ) )
        LightUnits IntensityUnits = LightUnits::Candelas;

        PROPERTY( DisplayName( "Radius" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length, Summary )
        float Radius = 1000.0f;

        // Inner radius where attenuation == 1 (a small emitter "source size"); falloff runs from here to
        // Radius. 0 = point source.
        PROPERTY( DisplayName( "Min Radius" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length, Advanced )
        float MinRadius = 0.0f;

        // UE ULocalLightComponent::SourceRadius: the emitter is a sphere of this radius, not a point. Traced
        // shadows (MegaLights, Shadows Stochastic) aim each ray at a point of that sphere, so a shadow is sharp
        // at its caster and widens with the distance to the receiver. 0 = a point (a hard shadow).
        PROPERTY( DisplayName( "Source Radius" ), Category( "Light" ), Range( 0.0f, 1000.0f ), Length, Advanced )
        float SourceRadius = 0.0f;

        PROPERTY( DisplayName( "Falloff" ), Category( "Light" ), Advanced )
        LightFalloff Falloff = LightFalloff::InverseSquare; // UE bUseInverseSquaredFalloff, on by default

        PROPERTY( DisplayName( "Show Radius" ), Category( "Light" ) )
        bool ShowRadius = false;

        // THE POINT LIGHT'S SHADOW (UE: ULightComponent::CastShadows / ShadowBias / ShadowSlopeBias /
        // ShadowResolutionScale). Drawn into the view's local-light shadow atlas (Graphic/LocalShadows.hpp) and
        // read by the deferred composite. The two biases are applied by every CASTER of the light's views (the
        // meshes and the terrain alike, Graphic::LocalCasterBias), in SHADOW-MAP TEXELS at the caster, not
        // centimetres: one texel of a perspective map grows with the distance from the light, so a fixed length
        // would be too much next to the lamp and too little across the room.
        PROPERTY( DisplayName( "Cast Shadows" ), Category( "Shadows" ) )
        bool CastShadows = true;

        PROPERTY( DisplayName( "Shadow Bias" ), Category( "Shadows" ), Range( 0.0f, 10.0f ), Units( "texels" ),
                  Tooltip( "How far every caster's depth is pushed away from the light, in shadow-map texels at "
                           "that caster. Larger hides acne and detaches contact shadows." ) )
        float ShadowBias = 0.5f;

        PROPERTY( DisplayName( "Shadow Slope Bias" ), Category( "Shadows" ), Range( 0.0f, 10.0f ),
                  Units( "texels" ),
                  Tooltip( "Extra bias of a caster the light grazes, in texels per unit of slope (clamped at "
                           "a slope of 1, 45 degrees - the one clamp every shadow caster has)." ) )
        float ShadowSlopeBias = 0.5f;

        PROPERTY( DisplayName( "Shadow Resolution Scale" ), Category( "Shadows" ), Range( 0.125f, 4.0f ),
                  Units( "x" ), Advanced,
                  Tooltip( "Scales the atlas tile this light asks for (its screen size decides the base); "
                           "the atlas still caps it." ) )
        float ShadowResolutionScale = 1.0f;

        // UE ULightComponent::MegaLightsShadowMethod: under Shadows Stochastic, the source MegaLights tests this
        // light's samples with (Common::Scalability::LightShadowMethod); Auto = the view's. Clamped to what the
        // device runs - DesertCtl state render.shadows.lights names the effective one.
        PROPERTY( DisplayName( "Shadow Method" ), Category( "Shadows" ), Advanced,
                  Tooltip( "Shadows Stochastic only: Auto follows the view; Ray Query, Virtual Maps or Screen "
                           "trace this light's samples that way when the device runs it." ) )
        Common::Scalability::LightShadowMethod ShadowMethod = Common::Scalability::LightShadowMethod::Auto;

        // CONTACT SHADOWS (UE: ULightComponent::ContactShadowLength / bContactShadowLengthInWS, the same default 0
        // = off). A short screen-space ray toward the light through the scene depth, marched by the deferred
        // composite (Common/ContactShadow.glslh), that grounds the small and near-contact detail the shadow map is
        // too coarse or too biased to hold. Needs Cast Shadows; its step count is Scalability
        // Shadows.ContactShadowSteps.
        PROPERTY( DisplayName( "Contact Shadow Length" ), Category( "Contact Shadows" ), Range( 0.0f, 100.0f ),
                  Tooltip( "Length of the screen-space shadow ray toward the light: a fraction of the screen "
                           "height at the receiver (0.1 = a tenth), or centimetres when In World Space. "
                           "0 = no contact shadows." ) )
        float ContactShadowLength = 0.0f;

        PROPERTY( DisplayName( "Contact Shadow Length In World Space" ), Category( "Contact Shadows" ),
                  Tooltip( "Contact Shadow Length is in world centimetres instead of a fraction of the screen." ) )
        bool ContactShadowLengthInWorldSpace = false;

        // UE ULightComponent::ContactShadowCastingIntensity / ContactShadowNonCastingIntensity: how dark the
        // contact shadow is where the ray hits a mesh that casts contact shadows (its Cast Contact Shadow flag),
        // and where it hits one that does not. UE's defaults: 1 and 0.
        PROPERTY( DisplayName( "Contact Shadow Casting Intensity" ), Category( "Contact Shadows" ),
                  Range( 0.0f, 1.0f ),
                  Tooltip( "Occlusion of a contact shadow cast by a mesh whose Cast Contact Shadow is on "
                           "(1 = black, 0 = none)." ) )
        float ContactShadowCastingIntensity = 1.0f;

        PROPERTY( DisplayName( "Contact Shadow Non Casting Intensity" ), Category( "Contact Shadows" ),
                  Range( 0.0f, 1.0f ),
                  Tooltip( "Occlusion where the ray hits a mesh whose Cast Contact Shadow is off "
                           "(0 = such a mesh casts none)." ) )
        float ContactShadowNonCastingIntensity = 0.0f;

        PROPERTY( DisplayName( "Volumetric Scattering Intensity" ), Category( "Light" ), Range( 0.0f, 10.0f ),
                  Tooltip( "How much of this light the volumetric fog scatters (UE ULightComponent::"
                           "VolumetricScatteringIntensity). 0 = the light leaves no shaft or halo in fog." ) )
        float VolumetricScatteringIntensity = 1.0f;
    };

    struct PointLightComponent
    {
        COMPONENT( Key( "PointLight" ), Block( Data ), Run( ActorsAndUI ) )
        PointLightData Data;
    };

    // Spot light: a cone of light from the entity's position (transform translation) aimed along the
    // entity's forward (transform rotation). Inner/Outer cone angles (degrees) give a soft edge.
    struct SpotLightData
    {
        REFLECT()

        PROPERTY( DisplayName( "Color" ), Category( "Light" ), Color, Temperature )
        glm::vec3 Color = glm::vec3( 1.0f );

        // UE ULocalLightComponent::Intensity / IntensityUnits: the value in the units beside it (default 8 cd, as
        // UE). Switching the units in Details re-states the value so the brightness holds (EditorPostEdit); the
        // renderer reads it only through LightPhotometry::ShaderIntensity. Physical units need InverseSquare.
        PROPERTY( DisplayName( "Intensity" ), Category( "Light" ), Range( 0.0f, 100000.0f ), Summary,
                  Tooltip( "Brightness in Intensity Units. Candelas: 1 cd lights a surface 1 m away with 1 lux; "
                           "Lumens: total flux (a 60 W bulb ~800 lm, a street lamp ~2500 lm); EV100: 2^(EV-3) cd; "
                           "Unitless: the renderer's multiplier at Min Radius. Physical units apply to "
                           "Inverse Square falloff only." ) )
        float Intensity = 8.0f;

        PROPERTY( DisplayName( "Intensity Units" ), Category( "Light" ),
                  Tooltip( "What Intensity is measured in (UE Intensity Units). Changing it converts the value "
                           "so the light keeps its brightness." ) )
        LightUnits IntensityUnits = LightUnits::Candelas;

        PROPERTY( DisplayName( "Range" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length, Summary )
        float Range = 1500.0f;

        // Inner radius where attenuation == 1 (the point light's Min Radius); falloff runs from here to Range.
        // 0 = point source. An imported FBX spot sets 100 cm, so InverseSquare reads Intensity / d^2 in metres.
        PROPERTY( DisplayName( "Min Radius" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length, Advanced )
        float MinRadius = 0.0f;

        PROPERTY( DisplayName( "Inner Cone" ), Category( "Light" ), Range( 0.0f, 89.0f ), Units( "deg" ) )
        float InnerConeAngle = 20.0f; // degrees — full intensity inside this half-angle

        PROPERTY( DisplayName( "Outer Cone" ), Category( "Light" ), Range( 0.0f, 90.0f ), Units( "deg" ), Summary )
        float OuterConeAngle = 30.0f; // degrees — zero intensity outside this half-angle

        // UE ULocalLightComponent::SourceRadius: the emitter is a sphere of this radius, not a point. Traced
        // shadows (MegaLights, Shadows Stochastic) aim each ray at a point of that sphere, so a shadow is sharp
        // at its caster and widens with the distance to the receiver. 0 = a point (a hard shadow).
        PROPERTY( DisplayName( "Source Radius" ), Category( "Light" ), Range( 0.0f, 1000.0f ), Length, Advanced )
        float SourceRadius = 0.0f;

        PROPERTY( DisplayName( "Falloff" ), Category( "Light" ), Advanced )
        LightFalloff Falloff = LightFalloff::InverseSquare; // UE bUseInverseSquaredFalloff, on by default

        PROPERTY( DisplayName( "Show Cone" ), Category( "Light" ) )
        bool ShowCone = false;

        // THE SPOT LIGHT'S SHADOW (UE: ULightComponent::CastShadows / ShadowBias / ShadowSlopeBias /
        // ShadowResolutionScale). Drawn into the view's local-light shadow atlas (Graphic/LocalShadows.hpp) and
        // read by the deferred composite. The two biases are applied by every CASTER of the light's views (the
        // meshes and the terrain alike, Graphic::LocalCasterBias), in SHADOW-MAP TEXELS at the caster, not
        // centimetres: one texel of a perspective map grows with the distance from the light, so a fixed length
        // would be too much next to the lamp and too little across the room.
        PROPERTY( DisplayName( "Cast Shadows" ), Category( "Shadows" ) )
        bool CastShadows = true;

        PROPERTY( DisplayName( "Shadow Bias" ), Category( "Shadows" ), Range( 0.0f, 10.0f ), Units( "texels" ),
                  Tooltip( "How far every caster's depth is pushed away from the light, in shadow-map texels at "
                           "that caster. Larger hides acne and detaches contact shadows." ) )
        float ShadowBias = 0.5f;

        PROPERTY( DisplayName( "Shadow Slope Bias" ), Category( "Shadows" ), Range( 0.0f, 10.0f ),
                  Units( "texels" ),
                  Tooltip( "Extra bias of a caster the light grazes, in texels per unit of slope (clamped at "
                           "a slope of 1, 45 degrees - the one clamp every shadow caster has)." ) )
        float ShadowSlopeBias = 0.5f;

        PROPERTY( DisplayName( "Shadow Resolution Scale" ), Category( "Shadows" ), Range( 0.125f, 4.0f ),
                  Units( "x" ), Advanced,
                  Tooltip( "Scales the atlas tile this light asks for (its screen size decides the base); "
                           "the atlas still caps it." ) )
        float ShadowResolutionScale = 1.0f;

        // UE ULightComponent::MegaLightsShadowMethod: under Shadows Stochastic, the source MegaLights tests this
        // light's samples with (Common::Scalability::LightShadowMethod); Auto = the view's. Clamped to what the
        // device runs - DesertCtl state render.shadows.lights names the effective one.
        PROPERTY( DisplayName( "Shadow Method" ), Category( "Shadows" ), Advanced,
                  Tooltip( "Shadows Stochastic only: Auto follows the view; Ray Query, Virtual Maps or Screen "
                           "trace this light's samples that way when the device runs it." ) )
        Common::Scalability::LightShadowMethod ShadowMethod = Common::Scalability::LightShadowMethod::Auto;

        // CONTACT SHADOWS (UE: ULightComponent::ContactShadowLength / bContactShadowLengthInWS, the same default 0
        // = off). A short screen-space ray toward the light through the scene depth, marched by the deferred
        // composite (Common/ContactShadow.glslh), that grounds the small and near-contact detail the shadow map is
        // too coarse or too biased to hold. Needs Cast Shadows; its step count is Scalability
        // Shadows.ContactShadowSteps.
        PROPERTY( DisplayName( "Contact Shadow Length" ), Category( "Contact Shadows" ), Range( 0.0f, 100.0f ),
                  Tooltip( "Length of the screen-space shadow ray toward the light: a fraction of the screen "
                           "height at the receiver (0.1 = a tenth), or centimetres when In World Space. "
                           "0 = no contact shadows." ) )
        float ContactShadowLength = 0.0f;

        PROPERTY( DisplayName( "Contact Shadow Length In World Space" ), Category( "Contact Shadows" ),
                  Tooltip( "Contact Shadow Length is in world centimetres instead of a fraction of the screen." ) )
        bool ContactShadowLengthInWorldSpace = false;

        // UE ULightComponent::ContactShadowCastingIntensity / ContactShadowNonCastingIntensity: how dark the
        // contact shadow is where the ray hits a mesh that casts contact shadows (its Cast Contact Shadow flag),
        // and where it hits one that does not. UE's defaults: 1 and 0.
        PROPERTY( DisplayName( "Contact Shadow Casting Intensity" ), Category( "Contact Shadows" ),
                  Range( 0.0f, 1.0f ),
                  Tooltip( "Occlusion of a contact shadow cast by a mesh whose Cast Contact Shadow is on "
                           "(1 = black, 0 = none)." ) )
        float ContactShadowCastingIntensity = 1.0f;

        PROPERTY( DisplayName( "Contact Shadow Non Casting Intensity" ), Category( "Contact Shadows" ),
                  Range( 0.0f, 1.0f ),
                  Tooltip( "Occlusion where the ray hits a mesh whose Cast Contact Shadow is off "
                           "(0 = such a mesh casts none)." ) )
        float ContactShadowNonCastingIntensity = 0.0f;

        PROPERTY( DisplayName( "Volumetric Scattering Intensity" ), Category( "Light" ), Range( 0.0f, 10.0f ),
                  Tooltip( "How much of this light the volumetric fog scatters (UE ULightComponent::"
                           "VolumetricScatteringIntensity). 0 = the light leaves no shaft or halo in fog." ) )
        float VolumetricScatteringIntensity = 1.0f;
    };

    struct SpotLightComponent
    {
        COMPONENT( Key( "SpotLight" ), Block( Data ), Run( ActorsAndUI ) )
        SpotLightData Data;
    };

    /**
     * @brief THE EFFECT AN ENTITY PLAYS (UE UNiagaraComponent): a `.dfx` system and whether it starts by itself.
     *
     * Every emitter of the system simulates on the GPU through its compiled module stack (Engine/VFX
     * VFXStackCompiler -> the ParticleSimulate host program), born by the emitter's spawn plan
     * (CompileSpawnPlan), at the entity's transform. Empty System = the entity plays nothing.
     */
    struct VFXComponentData
    {
        REFLECT()

        PROPERTY( DisplayName( "System" ), Category( "VFX" ), Asset<VFXSystemAsset>,
                  Tooltip( "The .dfx effect this entity plays" ) )
        Assets::AssetHandle System;

        // UE bAutoActivate: false = the system is placed but bears no particles until it is activated.
        PROPERTY( DisplayName( "Auto Activate" ), Category( "VFX" ) )
        bool AutoActivate = true;

        // UE UNiagaraComponent::OverrideParameters: this placement's values for the system's User parameters
        // (a parameter absent here plays its Default from the `.dfx`). Read by the spawn plan (VFXWorld) and the
        // GPU parameter rows (ParticleWorldGpu) every tick. Drawn by the VFX entry's "User Parameters" section,
        // which lists the SYSTEM's rows (a name, a type, a default) - a bare list of name/value pairs cannot.
        PROPERTY( DisplayName( "User Parameters" ), Category( "VFX" ), Hidden,
                  Tooltip( "This placement's values for the system's User parameters" ) )
        std::vector<VFXUserOverride> Overrides;

        // VFX-15: this placement's point sets, painted in VFX Paint mode and read by the stack's PointSet.<name>
        // bindings (engine:ShapePointSet). Level data, like Overrides: two placements of one system differ by it.
        PROPERTY( DisplayName( "Point Sets" ), Category( "VFX" ), Hidden,
                  Tooltip( "The point sets painted for this placement (VFX Paint mode)" ) )
        std::vector<VFXPointSet> PointSets;

        /// The set named @p name, or null.
        [[nodiscard]] const VFXPointSet* FindPointSet( std::string_view name ) const
        {
            for ( const VFXPointSet& s : PointSets )
                if ( s.Name == name )
                    return &s;
            return nullptr;
        }

        /// UE SetVariable*: this placement's User.<name> becomes @p value (packed like the row's Default),
        /// starting from the next VFX tick. The type is the system's to check (VFX.setVariable does).
        void SetVariable( std::string_view name, const glm::vec4& value )
        {
            for ( VFXUserOverride& o : Overrides )
                if ( o.Name == name )
                {
                    o.Value = value;
                    return;
                }
            Overrides.push_back( VFXUserOverride{ std::string( name ), value } );
        }

        /// The parameter plays its Default again (UE: resetting an override in the Details panel).
        void ClearVariable( std::string_view name )
        {
            std::erase_if( Overrides, [&]( const VFXUserOverride& o ) { return o.Name == name; } );
        }
    };

    /// A Level Sequence's hold on an effect (UE: UNiagaraComponent under a Niagara system track - Activate /
    /// Deactivate plus AgeUpdateMode DesiredAge): Active = the playhead is inside one of its VFX track's
    /// sections and the effect stands at AgeSeconds (VFXWorld replays this instance's fixed steps to it);
    /// inactive = outside every section, the instance holds no particles.
    struct VFXSequencerDrive
    {
        bool   Active     = false;
        double AgeSeconds = 0.0;

        bool operator==( const VFXSequencerDrive& ) const = default;
    };

    struct VFXComponent
    {
        COMPONENT( Key( "VFX" ), Block( Data ), Run( ActorsAndUI ) )
        VFXComponentData Data;

        // One-shot restart from the editor (every emitter of the system starts over). Transient - not reflected.
        bool RequestRestart = false;

        // Set by a Level Sequence's VFX track (LevelSequenceEntityHost::DriveEffect); nullopt = the effect runs on
        // the world's time as authored. While set, this instance's age is the sequence's - the scene's delta and
        // AutoActivate do not move it. The Sequencer preview gives it back on Restore. Transient - not reflected.
        std::optional<VFXSequencerDrive> SequencerDrive;
    };

    // ============================================================
    // UI — Godot-Control-style screen-space UI (2D). The authored data lives in the framework
    // (Desert/UI/Source/UI/Args, namespace Desert::UI); the ECS stores each one in the component that wraps it.
    // ============================================================

    struct UILayoutGroupComponent
    {
        COMPONENT( Key( "UILayoutGroup" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UILayoutGroupData Data;
    };
    struct UIProgressBarComponent
    {
        COMPONENT( Key( "UIProgressBar" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIProgressBarData Data;
    };
    struct UIPathComponent
    {
        COMPONENT( Key( "UIPath" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIPathData Data;
    };
    struct UIRetainerComponent
    {
        COMPONENT( Key( "UIRetainer" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIRetainerData Data;
    };
    struct UIToggleComponent
    {
        COMPONENT( Key( "UIToggle" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIToggleData Data;
    };
    struct UISliderComponent
    {
        COMPONENT( Key( "UISlider" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UISliderData Data;
    };
    struct UIScrollViewComponent
    {
        COMPONENT( Key( "UIScrollView" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIScrollViewData Data;
    };
    struct UIListViewComponent
    {
        COMPONENT( Key( "UIListView" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIListViewData Data;
    };
    struct UITileViewComponent
    {
        COMPONENT( Key( "UITileView" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UITileViewData Data;
    };
    struct UITreeViewComponent
    {
        COMPONENT( Key( "UITreeView" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UITreeViewData Data;
    };
    struct UIInputFieldComponent
    {
        COMPONENT( Key( "UIInputField" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIInputFieldData Data;
    };
    struct UIDropdownComponent
    {
        COMPONENT( Key( "UIDropdown" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIDropdownData Data;
    };
    struct UIStyleComponent
    {
        COMPONENT( Key( "UIStyle" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIStyleData Data;
    };
    struct UICanvasComponent
    {
        COMPONENT( Key( "UICanvas" ), Block( Data ), Run( ActorsAndUI ) )
        UI::UICanvasData Data;
    };
    struct UILayoutComponent
    {
        COMPONENT( Key( "UILayout" ), Block( Data ), Run( ActorsAndUI ) )
        UI::UILayoutData Data;
    };
    struct UIPanelComponent
    {
        COMPONENT( Key( "UIPanel" ), Block( Data ), Run( ActorsAndUI ) )
        UI::UIPanelData Data;
    };
    struct UITweenComponent
    {
        COMPONENT( Key( "UITween" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UITweenData Data;
    };
    struct UIBindingComponent
    {
        COMPONENT( Key( "UIBinding" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIBindingData Data;
    };

    // A multi-key UI animation: a Timeline::Sequence hosted as UIAnimation (Timeline/Hosts.hpp) — UE's
    // UWidgetAnimation, the same MovieScene core a LevelSequence plays; widgets own no key model of their own.
    // Its Widget bindings name elements by entity UUID, so one clip may drive several elements. Authored on the
    // Sequencer, serialized by hand (ComponentRegistry) as the TMLN text block.
    struct UIAnimData
    {
        Animation::Timeline::Sequence Sequence = []
        {
            Animation::Timeline::Sequence hosted; // every other field keeps the Sequence's own default
            hosted.Host = Animation::Timeline::SequenceHost::UIAnimation;
            return hosted;
        }();
        Animation::Timeline::LoopMode Loop     = Animation::Timeline::LoopMode::Once;
        bool                          AutoPlay = true;

        // Where playback is. RUNTIME only — never serialized, so scrubbing in the editor cannot dirty the
        // scene. Created lazily from Sequence.TickRate/Start/End by the one view that drives scene animation
        // (UI/Ecs/UIAnimationPlayback.hpp); whoever edits the range resets it so the next frame re-creates it.
        std::optional<Animation::Timeline::Player> Playback;

        // The clip's Audio sections sounding at the playhead, as the driving view's LAST frame evaluated them
        // (UIAnimationPlayback.hpp; empty while paused, stopped or scrubbed backwards). RUNTIME only, never
        // serialized: AudioECSSystem consumes and clears it every frame and turns it into voices.
        std::vector<Animation::Timeline::SoundingVoice> Sounding;
    };
    struct UIAnimComponent
    {
        UIAnimData Data;
    };
    struct UIScreenComponent
    {
        COMPONENT( Key( "UIScreen" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIScreenData Data;
    };
    struct UIScreenStackComponent
    {
        COMPONENT( Key( "UIScreenStack" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIScreenStackData Data;
    };
    // A tab bar: the ShowScreen buttons below it select themselves from the switcher they drive
    // (UI::UITabsData, UI/UIScreenMachine.hpp). A modifier on a container element; draws nothing itself.
    struct UITabsComponent
    {
        COMPONENT( Key( "UITabs" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UITabsData Data;
    };
    struct UIPointerEventsComponent
    {
        COMPONENT( Key( "UIPointerEvents" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIPointerEventsData Data;
    };
    struct UIDraggableComponent
    {
        COMPONENT( Key( "UIDraggable" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIDraggableData Data;
    };
    struct UIDropTargetComponent
    {
        COMPONENT( Key( "UIDropTarget" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIDropTargetData Data;
    };
    struct UINavigationComponent
    {
        COMPONENT( Key( "UINavigation" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UINavigationData Data;
    };
    struct UIOverlayComponent
    {
        COMPONENT( Key( "UIOverlay" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIOverlayData Data;
    };
    struct UIOverlayTriggerComponent
    {
        COMPONENT( Key( "UIOverlayTrigger" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIOverlayTriggerData Data;
    };
    struct UIIconComponent
    {
        COMPONENT( Key( "UIIcon" ), Block( Data ), Run( ActorsAndUI ) )
        UI::UIIconData Data;
    };
    struct UIImageComponent
    {
        COMPONENT( Key( "UIImage" ), Block( Data ), Run( UIAfterRenderTexture ) )
        UI::UIImageData Data;
    };
    struct UIRenderTextureComponent
    {
        UI::UIRenderTextureData Data;
    };
    struct UITextComponent2D
    {
        COMPONENT( Key( "UIText" ), Block( Data ), Run( ActorsAndUI ) )
        UI::UITextData Data;
    };
    struct UIButtonComponent
    {
        COMPONENT( Key( "UIButton" ), Block( Data ), Run( ActorsAndUI ) )
        UI::UIButtonData Data;
    };

    // The HDR-cubemap background, and nothing else. The procedural atmosphere (palette, sun, stars, the
    // IBL bake request) moved to SkyAtmosphereComponent; the old fields are gone rather than deprecated,
    // so there is exactly one place each value can live.
    //
    // Reflected (REFLECT/PROPERTY) so it (de)serializes generically — the SkyboxHandle round-trips as an
    // asset PATH via the serializer's AssetResolver. Fields kept flat (no Data sub-struct) so existing
    // accessors are unchanged.
    struct SkyboxComponent
    {
        COMPONENT( Key( "Skybox" ), Whole, Run( SkyAndAtmosphere ) )
        REFLECT()

        // Hidden from the auto-generated Details (the widget draws a proper SkyboxAsset picker + DnD instead
        // of the builder's texture-oriented asset slot). Still serialized — Hidden is editor-only.
        PROPERTY( DisplayName( "Skybox" ), Category( "Skybox" ), Asset<SkyboxAsset>, Hidden )
        Assets::AssetHandle SkyboxHandle;

        // ── THE AUTHORED LOOK ─────────────────────────────────────────────────────────────────────────
        //
        // All three reach the frame through ONE route: ECS::SkyLookOf packs them into a Graphic::SkyLook,
        // and that one value is applied wherever the environment cubes are SAMPLED — the backdrop, and
        // every lit surface's ambient and reflections (Shaders/Common/SkyLook.glslh). Intensity used to be
        // applied to the sky pass alone and therefore lit nothing; the shader census in
        // Tests/Engine/SkyPanorama is what keeps that state unreachable now.
        //
        // THE PRICE IS A UNIFORM WRITE PER FRAME. They used to be baked into the cubes, and every slider
        // value cost a device-idling rebake of ~0.7 s; the cubes are now the file as authored.

        PROPERTY( DisplayName( "Intensity" ), Category( "Skybox" ), Range( 0.0f, 10.0f ) )
        float Intensity = 1.0f;

        // Degrees about the world's up axis. Answers the one thing an author cannot do to a panorama
        // from outside: line the sun that is baked into the image up with the scene's directional light.
        PROPERTY( DisplayName( "Rotation" ), Category( "Skybox" ), Range( 0.0f, 360.0f ) )
        float Rotation = 0.0f;

        // Linear grade over the whole sky. White is the file as authored.
        PROPERTY( DisplayName( "Tint" ), Category( "Skybox" ), Color )
        glm::vec3 Tint = glm::vec3( 1.0f );
    };

    // Scene-outliner grouping node: an otherwise-empty entity that acts as a FOLDER for organizing the
    // hierarchy (drag entities under it). It renders nothing — just parents children via RelationshipComponent
    // — so it's purely an authoring aid. Marker component (no data); serialized so folders persist.
    struct FolderComponent
    {
    };

    // Authoring lock (UE's actor lock): this entity cannot be picked in the viewport and its transform
    // cannot be dragged by the gizmo. Set on a finished floor, a lightmap-baked prop, a background you
    // keep grabbing by accident.
    //
    // MARKER, not a `bool Locked` — presence IS the state, the same bargain FolderComponent makes above.
    // A bool would have two ways to spell "not locked" (absent, or present-and-false) and every reader
    // would have to handle both; a marker has one, so `registry.has<LockComponent>( e )` is the whole
    // question and no site can get it half-right.
    //
    // IT IS SERIALIZED (ComponentRegistry.cpp, "Lock"). That is not decoration: a lock that does not
    // survive a reload protects nothing, because the reload is exactly when you have forgotten which
    // things were finished. Adding the key needs no scene-version bump — ForeignKeys preserves keys a
    // build does not declare, and a file written before this component simply lacks it and loads
    // unlocked, which is the correct default.
    //
    // NOT a render or gameplay flag: nothing in the runtime reads it, and a packaged game has no
    // viewport to pick in. VisibilityComponent is the one that changes what is drawn.
    struct LockComponent
    {
    };

    // WORLD PARTITION: KEEP THIS LOADED EVERYWHERE. The author's one input to partitioning besides the
    // grid (owner decision 2026-09-23): the level and the cell are derived from the footprint, whether a
    // thing is global is derived from its components (Rules::kComponentLoading), and this marker is for
    // what neither can see - a game-manager script, a trigger that must hear the player from anywhere.
    //
    // MARKER for the reason LockComponent above is one: presence is the state, so there is no
    // present-and-false second spelling of "not always-loaded". It reaches the partition through its key
    // on disk, "AlwaysLoaded" (ComponentRegistry.cpp), which Rules::PlanWorldPartition reads; one marked
    // member makes its whole composite always-loaded, because a composite is never divided.
    //
    // In a world with no WorldPartition block it changes nothing, and its toggle in the Details header
    // says so in its tooltip.
    struct AlwaysLoadedComponent
    {
    };

    struct PrefabComponent
    {
        Assets::AssetHandle Prefab;
    };

    // WHICH RECORD OF WHICH PREFAB FILE THIS ENTITY CAME FROM — the link an override is addressed by.
    //
    // RUNTIME ONLY, AND DELIBERATELY NOT SERIALIZED. PrefabFactory stamps it on every entity it creates,
    // at every nesting depth, so it is re-derived in full on every instantiation; writing it into a
    // `.desce` would be a second copy of a fact the prefab file already states, and the two would drift
    // the first time a prefab was edited.
    //
    // WHY THE ENTITY'S OWN UUID COULD NOT BE USED INSTEAD. PrefabFactory mints a FRESH uuid for every
    // entity of every instance on purpose (two instances of one prefab must not collide), so an instance
    // entity has no identity that survives a reload. The record id does: it lives in the `.deprefab` and
    // only changes when the prefab itself is re-authored.
    //
    // The vector is a PATH, not an id: one element for an entity of the instance itself, two for one
    // inside a prefab nested a level down, and so on.
    struct PrefabInstanceComponent
    {
        std::vector<Common::UUID> SourcePath;
    };

    struct RelationshipComponent
    {
        entt::entity              Parent = entt::null;
        std::vector<entt::entity> Children;
    };

    // --- Physics (Jolt) ---------------------------------------------------------------------------------
    // A body's collision SHAPE (reflected -> Details UI + serialization). Paired with RigidBodyComponent
    // to be simulated by PhysicsECSSystem.
    struct ColliderData
    {
        REFLECT()

        PROPERTY( DisplayName( "Shape" ), Category( "Collider" ), Summary )
        Physics::ShapeType Shape = Physics::ShapeType::Box;

        PROPERTY( DisplayName( "Half Extents" ), Category( "Collider" ), Length )
        glm::vec3 HalfExtents = { 50.0f, 50.0f, 50.0f }; // Box

        PROPERTY( DisplayName( "Radius" ), Category( "Collider" ), Range( 1.0f, 5000.0f ), Length )
        float Radius = 50.0f; // Sphere / Capsule

        PROPERTY( DisplayName( "Half Height" ), Category( "Collider" ), Range( 1.0f, 5000.0f ), Length )
        float HalfHeight = 50.0f; // Capsule

        PROPERTY( DisplayName( "Capsule Axis" ), Category( "Collider" ) )
        Physics::CapsuleAxis Axis =
             Physics::CapsuleAxis::Y; // Capsule: the body-local axis its cylinder runs along

        PROPERTY( DisplayName( "Center" ), Category( "Collider" ), Length )
        glm::vec3 Center = { 0.0f, 0.0f,
                             0.0f }; // Box / Sphere / Capsule: body-local offset (UE FKShapeElem Center)
    };

    struct ColliderComponent
    {
        COMPONENT( Key( "Collider" ), Block( Data ), Run( ActorsAndUI ) )
        ColliderData Data;
    };

    // --- Audio (miniaudio) ------------------------------------------------------------------------------
    // A sound emitter (reflected -> Details UI + serialization). AudioECSSystem creates the runtime
    // source in Play mode (AutoPlay), positions spatial sources at the entity's world transform, and
    // stops everything on the Play->Edit transition.
    struct AudioSourceData
    {
        REFLECT()

        // The SOUND asset (`.desound`, UE's USoundWave) by its GUID's handle: the audio file is the asset's to
        // name (SoundAsset::ResolveSourceFile), so moving the pair moves no reference. Unset = silent.
        PROPERTY( DisplayName( "Sound" ), Category( "Audio" ), Summary, Asset<SoundAsset> )
        Assets::AssetHandle Sound;

        PROPERTY( DisplayName( "Volume" ), Category( "Audio" ), Range( 0.0f, 2.0f ) )
        float Volume = 1.0f;

        PROPERTY( DisplayName( "Loop" ), Category( "Audio" ) )
        bool Loop = false;

        PROPERTY( DisplayName( "Auto Play" ), Category( "Audio" ) )
        bool AutoPlay = true; // start when the scene enters Play

        PROPERTY( DisplayName( "3D Spatial" ), Category( "Audio" ), Advanced )
        bool Spatial = true; // attenuate/pan from the entity position vs. the listener (camera)
    };

    struct AudioSourceComponent
    {
        COMPONENT( Key( "AudioSource" ), Block( Data ), Run( ActorsAndUI ) )
        AudioSourceData Data;
    };

    // Body simulation params (reflected -> Details UI + serialization).
    struct RigidBodyData
    {
        REFLECT()

        PROPERTY( DisplayName( "Type" ), Category( "Rigid Body" ), Summary )
        Physics::BodyType Type = Physics::BodyType::Dynamic;

        PROPERTY( DisplayName( "Mass" ), Category( "Rigid Body" ), Range( 0.0f, 1000.0f ), Units( "kg" ), Summary )
        float Mass = 1.0f;

        PROPERTY( DisplayName( "Friction" ), Category( "Rigid Body" ), Range( 0.0f, 2.0f ), Advanced )
        float Friction = 0.5f;

        PROPERTY( DisplayName( "Restitution" ), Category( "Rigid Body" ), Range( 0.0f, 1.0f ), Advanced )
        float Restitution = 0.1f;

        // UE's Collision Presets: a profile NAME of the project's Config/CollisionProfiles.json, resolved when
        // the body is created; a name the register lacks refuses the body by name (no fallback profile).
        PROPERTY( DisplayName( "Collision Profile" ), Category( "Collision" ) )
        std::string CollisionProfile = "PhysicsActor";

        // THE BODY'S EVENTS — UE UPrimitiveComponent::OnComponentHit / OnComponentBeginOverlap /
        // OnComponentEndOverlap (PrimitiveComponent.h). Physics reports the facts (PhysicsEventQueue); the
        // Gameplay Framework delivers them here (ComponentEventSystem) to whoever subscribed to THIS entity
        // (ComponentEvents). The profile decides whether a body reports at all (Generates Hit/Overlap Events).
        EVENT( Category( "Collision" ),
               Tooltip( "This body struck another and the contact began (UE OnComponentHit). Normal points from "
                        "this body towards the other; impulse in kg*cm/s." ) )
        using OnHit = void( entt::entity other, glm::vec3 point, glm::vec3 normal, float impulse );

        EVENT( Category( "Collision" ),
               Tooltip( "Another body began to overlap this one (UE OnComponentBeginOverlap)." ) )
        using OnBeginOverlap = void( entt::entity other );

        EVENT( Category( "Collision" ),
               Tooltip( "Another body stopped overlapping this one, also when it was destroyed while overlapping "
                        "(UE OnComponentEndOverlap)." ) )
        using OnEndOverlap = void( entt::entity other );
    };

    // Marks an entity as a physics body. Static = immovable, Dynamic = simulated, Kinematic = code-driven.
    struct RigidBodyComponent
    {
        COMPONENT( Key( "RigidBody" ), Block( Data ), Run( ActorsAndUI ) )
        RigidBodyData Data;

        // Transient: the live Jolt body (created on Play, cleared on Stop). Not reflected/serialized.
        Physics::BodyHandle RuntimeBody = Physics::kInvalidBody;
    };

    // THE PLAYABLE CHARACTER'S MOVEMENT (GP2a; UE: ACharacter's UCapsuleComponent + UCharacterMovementComponent in
    // MOVE_Walking / MOVE_Falling, crouch included). Drives a Jolt CharacterVirtual capsule (walks slopes/steps,
    // blocked by world geometry); the model itself - acceleration, braking, friction, air control, jump, the
    // crouch shrink and the can-uncrouch overlap - is ECS/System/CharacterMovement.cpp. Every speed is cm/s,
    // every acceleration cm/s^2, every length cm. The INPUT is not here: the pawn's script reads the Enhanced
    // Input actions (IA_Move / IA_Look / IA_Jump / IA_Crouch) and calls self:move / jump / crouch.
    struct CharacterControllerData
    {
        REFLECT()

        PROPERTY( DisplayName( "Radius" ), Category( "Capsule" ), Range( 5.0f, 500.0f ), Length )
        float Radius = 34.0f;

        // Total standing capsule height, caps included (cylinder half-height = (Height - 2*Radius) / 2).
        PROPERTY( DisplayName( "Height" ), Category( "Capsule" ), Range( 20.0f, 1000.0f ), Length )
        float Height = 176.0f;

        PROPERTY( DisplayName( "Max Slope" ), Category( "Capsule" ), Range( 0.0f, 89.0f ), Units( "deg" ) )
        float MaxSlopeDeg = 50.0f;

        // UE MaxWalkSpeed: the ground speed a full stick reaches.
        PROPERTY( DisplayName( "Max Walk Speed" ), Category( "Walking" ), Range( 0.0f, 5000.0f ), Units( "cm/s" ),
                  Summary )
        float MaxWalkSpeed = 600.0f;

        // UE MaxAcceleration: how fast input changes the velocity (ground; times Air Control in the air).
        PROPERTY( DisplayName( "Max Acceleration" ), Category( "Walking" ), Range( 0.0f, 20000.0f ),
                  Units( "cm/s2" ) )
        float MaxAcceleration = 2048.0f;

        // UE BrakingDecelerationWalking: the constant deceleration with no input on the ground.
        PROPERTY( DisplayName( "Braking Deceleration" ), Category( "Walking" ), Range( 0.0f, 20000.0f ),
                  Units( "cm/s2" ) )
        float BrakingDecelerationWalking = 2048.0f;

        // UE GroundFriction: turning friction while accelerating, and (times Braking Friction Factor) the
        // velocity-proportional part of braking.
        PROPERTY( DisplayName( "Ground Friction" ), Category( "Walking" ), Range( 0.0f, 100.0f ), Advanced )
        float GroundFriction = 8.0f;

        // UE BrakingFrictionFactor (default 2): ground friction is multiplied by this while braking.
        PROPERTY( DisplayName( "Braking Friction Factor" ), Category( "Walking" ), Range( 0.0f, 10.0f ), Advanced )
        float BrakingFrictionFactor = 2.0f;

        // UE JumpZVelocity: the upward launch speed; the apex is JumpZVelocity^2 / (2 * gravity).
        PROPERTY( DisplayName( "Jump Z Velocity" ), Category( "Jumping / Falling" ), Range( 0.0f, 5000.0f ),
                  Units( "cm/s" ), Summary )
        float JumpZVelocity = 700.0f;

        // UE AirControl: the fraction of Max Acceleration the input has while falling (0 = none, 1 = as on
        // the ground).
        PROPERTY( DisplayName( "Air Control" ), Category( "Jumping / Falling" ), Range( 0.0f, 1.0f ) )
        float AirControl = 0.35f;

        // UE GravityScale: times the scene's gravity (the physics world's), not a gravity of its own.
        PROPERTY( DisplayName( "Gravity Scale" ), Category( "Jumping / Falling" ), Range( 0.0f, 10.0f ) )
        float GravityScale = 1.0f;

        // UE MaxWalkSpeedCrouched.
        PROPERTY( DisplayName( "Max Walk Speed Crouched" ), Category( "Crouching" ), Range( 0.0f, 5000.0f ),
                  Units( "cm/s" ) )
        float MaxWalkSpeedCrouched = 300.0f;

        // Total crouched capsule height, caps included (UE CrouchedHalfHeight x 2). The radius stays.
        PROPERTY( DisplayName( "Crouched Height" ), Category( "Crouching" ), Range( 20.0f, 1000.0f ), Length )
        float CrouchedHeight = 120.0f;

        // UE MaxSwimSpeed: horizontal and vertical speed while the script says the body is swimming.
        PROPERTY( DisplayName( "Max Swim Speed" ), Category( "Swimming" ), Range( 0.0f, 5000.0f ),
                  Units( "cm/s" ) )
        float MaxSwimSpeed = 300.0f;

        // The capsule's profile in Config/CollisionProfiles.json (UE: the capsule's Collision Presets, "Pawn").
        PROPERTY( DisplayName( "Collision Profile" ), Category( "Collision" ) )
        std::string CollisionProfile = "Pawn";
    };

    // A playable character. The follow camera is NOT here - a child entity with a SpringArmComponent and its
    // camera child (third person), or a camera child at the head (first person).
    struct CharacterControllerComponent
    {
        COMPONENT( Key( "CharacterController" ), Block( Data ), Run( ActorsAndUI ) )
        CharacterControllerData Data;

        // Transient (Play only): the live Jolt character and the movement state.
        Physics::CharacterHandle RuntimeCharacter = Physics::kInvalidCharacter;
        glm::vec3                Velocity         = { 0.0f, 0.0f, 0.0f }; // cm/s, what the model asked of Jolt
        float                    CurrentSpeed     = 0.0f;  // planar |Velocity|, cm/s (drives locomotion)
        bool                     OnGround         = false; // last physics result (self:isOnGround())
        bool                     IsCrouched       = false; // UE bIsCrouched: the capsule IS the crouched one

        // INTENT, written by the pawn's script each frame and executed by PhysicsECSSystem (UE: AddMovementInput,
        // Jump, Crouch / UnCrouch). MoveInput is camera-relative (x = right, y = forward), length clamped to 1.
        glm::vec2 MoveInput       = { 0.0f, 0.0f };
        bool      JumpRequested   = false; // one-shot, consumed by the next physics step
        bool      CrouchRequested = false; // UE bWantsToCrouch: held; standing up waits for head room

        // Swimming (set by the controller SCRIPT when it detects the body is below the water level): gravity is
        // replaced by the swim intent (+1 up, -1 down) at Max Swim Speed.
        bool  Swimming     = false;
        float SwimVertical = 0.0f;

        // ROOT MOTION (UE ACharacter::RootMotionParams, consumed by the CharacterMovementComponent). Written by
        // AnimationECSSystem when this character's skinned child plays with RootMotionFromEverything — this
        // frame's root movement converted to world space and divided by the animation step (UE
        // CalcAnimRootMotionVelocity) — and consumed by every fixed step of this frame's physics, where it
        // REPLACES the planar velocity the input would have produced (gravity and jumps still act). Cleared by
        // PhysicsECSSystem after the frame's step, so a character whose animation stopped is the input's again.
        // RootMotionRotation is the frame's turn about world up; PhysicsECSSystem — the one writer of a
        // character's transform — applies it with the step's position (UE: the CMC rotates the actor).
        bool      HasRootMotion      = false;
        glm::vec3 RootMotionVelocity = { 0.0f, 0.0f, 0.0f };       // cm/s, world
        glm::quat RootMotionRotation = { 1.0f, 0.0f, 0.0f, 0.0f }; // world yaw delta
    };

    // UE USpringArmComponent: on a child entity of the pawn, whose rotation is the arm's (scripts pitch it, the
    // pawn yaws). Each frame PhysicsECSSystem places the arm's CAMERA child TargetArmLength behind the arm's
    // origin (along the arm's +Z, the camera's back) plus SocketOffset in arm space; with Do Collision Test a
    // sphere of Probe Size is swept from the origin to that point and the camera is pulled in to the first hit,
    // and it goes back out when the obstacle is gone. Camera Lag smooths the ORIGIN the arm hangs from (UE
    // bEnableCameraLag), never the collision. The solve is ECS/System/SpringArm.cpp.
    struct SpringArmData
    {
        REFLECT()

        PROPERTY( DisplayName( "Target Arm Length" ), Category( "Camera" ), Range( 0.0f, 5000.0f ), Length,
                  Summary )
        float TargetArmLength = 300.0f;

        // Arm-space offset of the camera at the arm's end (UE SocketOffset): (0, 60, 0) lifts it over the head.
        PROPERTY( DisplayName( "Socket Offset" ), Category( "Camera" ), Length )
        glm::vec3 SocketOffset = { 0.0f, 0.0f, 0.0f };

        PROPERTY( DisplayName( "Do Collision Test" ), Category( "Camera Collision" ) )
        bool DoCollisionTest = true;

        // Radius of the swept probe sphere (UE ProbeSize): how far the camera keeps from the wall it meets.
        PROPERTY( DisplayName( "Probe Size" ), Category( "Camera Collision" ), Range( 0.0f, 200.0f ), Length )
        float ProbeSize = 12.0f;

        PROPERTY( DisplayName( "Enable Camera Lag" ), Category( "Lag" ) )
        bool EnableCameraLag = false;

        // UE CameraLagSpeed: the origin closes this fraction of its distance per second (VInterpTo).
        PROPERTY( DisplayName( "Camera Lag Speed" ), Category( "Lag" ), Range( 0.0f, 1000.0f ) )
        float CameraLagSpeed = 10.0f;
    };

    struct SpringArmComponent
    {
        COMPONENT( Key( "SpringArm" ), Block( Data ), Run( ActorsAndUI ) )
        SpringArmData Data;

        // Transient: the lagged origin (valid once HasLaggedOrigin) and the length the last solve reached.
        glm::vec3 LaggedOrigin     = { 0.0f, 0.0f, 0.0f };
        bool      HasLaggedOrigin  = false;
        float     CurrentArmLength = 0.0f;
    };

    // UE's APlayerStart: where Play puts the player's pawn (SceneSettings::DefaultPawn). Its transform is
    // the spawn transform; the tag lets a level have several named entries (a door, a checkpoint) that a
    // Play request asks for by name. Selection rules: Core::ChoosePlayerStart.
    struct PlayerStartData
    {
        REFLECT()

        PROPERTY( DisplayName( "Player Start Tag" ), Category( "Player Start" ),
                  Tooltip( "Empty = the level's default start. A tagged start is used only when Play asks "
                           "for its tag." ) )
        std::string Tag;
    };

    struct PlayerStartComponent
    {
        COMPONENT( Key( "PlayerStart" ), Block( Data ), Run( ActorsAndUI ) )
        PlayerStartData Data;
    };

    // UE's UWorldPartitionStreamingSourceComponent: what makes an entity a point a partitioned world loads
    // around in Play and in the game. Residency follows the union of every enabled source (Core::WorldStreamer);
    // the camera is not one by itself. The player's pawn gets one when its prefab has none
    // (Core::SpawnDefaultPawn), as UE's player controller is a source. Its entity loads Global
    // (WorldPartitionRules), so a source never streams itself out.
    struct StreamingSourceData
    {
        REFLECT()

        PROPERTY( DisplayName( "Enabled" ), Category( "Streaming Source" ),
                  Tooltip( "Off = the world does not load around this entity." ) )
        bool Enabled = true;

        PROPERTY( DisplayName( "Override Loading Range" ), Category( "Streaming Source" ),
                  Tooltip( "Off = the World Partition grid's Loading Range." ) )
        bool OverrideLoadingRange = false;

        PROPERTY( DisplayName( "Loading Range" ), Category( "Streaming Source" ), Units( "cm" ),
                  Range( 0.0f, 1000000.0f ), EditCondition( "OverrideLoadingRange" ),
                  Tooltip( "Cells within this distance of the entity load (cm)." ) )
        float LoadingRange = 0.0f;

        PROPERTY( DisplayName( "Priority" ), Category( "Streaming Source" ),
                  Tooltip( "Higher loads first when several sources want cells; it never changes which cells "
                           "load." ) )
        int Priority = 0;

        // The shape the world loads within (UE's FStreamingSourceShape): a circle of the range, or a sector of
        // it, placed in the entity's frame (Rules::StreamingSourceShape).
        PROPERTY( DisplayName( "Sector" ), Category( "Streaming Source" ),
                  Tooltip( "On = only a slice of the circle, Sector Angle wide around the entity's forward "
                           "(turned by Shape Yaw), loads." ) )
        bool IsSector = false;

        PROPERTY( DisplayName( "Sector Angle" ), Category( "Streaming Source" ), Units( "deg" ),
                  Range( 0.0f, 360.0f ), EditCondition( "IsSector" ),
                  Tooltip( "How wide the slice is; 360 is the whole circle." ) )
        float SectorAngle = 360.0f;

        // A VIEW AS A SOURCE: a camera that streams what it looks at (a narrow lens reaching far) states the
        // range and priority; the angle follows the lens as gameplay changes the camera's Field of View.
        PROPERTY( DisplayName( "Angle From Camera" ), Category( "Streaming Source" ), EditCondition( "IsSector" ),
                  Tooltip( "On = the slice is as wide as this entity's Camera sees across: its Field of View "
                           "widened by the viewport's width over height, following every change of it. Sector "
                           "Angle is not used. The entity must have a Camera." ) )
        bool SectorAngleFromCamera = false;

        PROPERTY( DisplayName( "Shape Offset" ), Category( "Streaming Source" ), Length,
                  Tooltip( "Where the shape's centre sits from the entity, in its own axes (cm): X right, Z "
                           "backward. Height is ignored." ) )
        glm::vec3 ShapeOffset = glm::vec3( 0.0f );

        PROPERTY( DisplayName( "Shape Yaw" ), Category( "Streaming Source" ), Units( "deg" ),
                  Range( -180.0f, 180.0f ),
                  Tooltip( "How far the shape's axis is turned from the entity's forward, about its up axis." ) )
        float ShapeYaw = 0.0f;

        PROPERTY( DisplayName( "Use Velocity For Cell Sorting" ), Category( "Streaming Source" ),
                  Tooltip( "On = cells the entity is moving toward load first (UE's "
                           "bUseVelocityContributionToCellsSorting); it never changes which cells load." ) )
        bool UseVelocityForSorting = false;

        // UE's FWorldPartitionStreamingSource::TargetGrids: a camera streaming only the near grid's small props,
        // a cinematic only the main grid. A name the world has no grid of streams nothing and is an Issue of the
        // world's plan (Rules::PlanWorldPartition, Entities[i].StreamingSource.TargetGrids). Details draws it as
        // a pick of the world's grid names (ComponentEditorRegistrations.cpp, MakeStreamingSourceEntry).
        PROPERTY( DisplayName( "Target Grids" ), Category( "Streaming Source" ),
                  Tooltip( "The World Partition grids this source streams, by name; empty = every grid." ),
                  Hidden )
        std::vector<std::string> TargetGrids;

        // THE LOOK-AHEAD (Rules::StreamingSource): a plane or a car streams the cells it is about to reach, not
        // only those around it. UE has no such setting; there a game adds a predicted source by hand.
        PROPERTY( DisplayName( "Look Ahead Time" ), Category( "Streaming Source" ), Units( "s" ),
                  Range( 0.0f, 120.0f ),
                  Tooltip( "Cells along the entity's path load ahead of time: the loading shape reaches as far "
                           "as the entity travels in this many seconds at its present speed (on the ground). "
                           "0 = only around where it is. About how long a cell takes to load." ) )
        float LookAheadTime = 0.0f;

        PROPERTY( DisplayName( "Max Look Ahead" ), Category( "Streaming Source" ), Length,
                  Range( 0.0f, 10000000.0f ),
                  Tooltip( "The farthest ahead the loading shape reaches however fast the entity moves (cm), "
                           "so a teleport or a burst of speed does not request half the world." ) )
        float MaxLookAhead = 100000.0f;
    };

    struct StreamingSourceComponent
    {
        COMPONENT( Key( "StreamingSource" ), Block( Data ), Run( ActorsAndUI ) )
        StreamingSourceData Data;
    };

    // WHICH RUNTIME GRID OF A PARTITIONED WORLD THIS ENTITY STREAMS ON - UE's AActor::RuntimeGrid (WP22). Absent
    // = the world's first grid, UE's main grid, so there is no present-and-empty second spelling of "main": an
    // empty or unknown name is refused by the planner with the entity's path (Rules::PlanWorldPartition). A
    // composite streams on its anchor's grid; the grid sets the cell size and the loading range it uses.
    struct RuntimeGridData
    {
        REFLECT()

        PROPERTY( DisplayName( "Runtime Grid" ), Category( "World Partition" ),
                  Tooltip( "The name of the World Partition grid this entity streams on; the grid's cell size "
                           "and loading range apply to it." ) )
        std::string Grid;
    };

    struct RuntimeGridComponent
    {
        COMPONENT( Key( "RuntimeGrid" ), Block( Data ), Run( ActorsAndUI ) )
        RuntimeGridData Data;
    };

    // Attaches a Lua script to an entity. The ScriptSystem loads the file and calls its OnStart()/OnUpdate(dt);
    // the script drives behavior through the bound API (self:move/jump/addYaw..., Input.*). See ScriptEngine.
    // A behavior unit, like a UE ActorComponent: one .lua file + its exposed properties + lifecycle flag.
    struct ScriptSlot
    {
        // The .lua file, as a ROOT-TAGGED KEY — the exact form Common::AssetHandle::StableKeyForPath
        // mints and Common::AssetHandle::PathForStableKey reads back, e.g.
        // "assets:Scripts/Examples/MoveAlongX.lua". NOT a path: never hand it to std::filesystem or to
        // an ifstream, ask ResolvedPath() below.
        //
        // WHY IT IS NOT A PATH ANY MORE (I9, scene schema v16). It used to hold the ROOTED spelling the
        // editor happened to be standing in — "Resources/Assets/Scripts/Examples/MoveAlongX.lua" — and a
        // rooted spelling does not survive packaging: a packaged game remaps ASSETS_PATH to
        // <package>/Assets/, so the stored string named a directory that does not exist there. Measured
        // on a mounted archive by I8: the stored spelling gave Exists=0 while the same file addressed
        // through the scripts root gave Exists=1. Every reference in a scene that goes through
        // MakeAssetResolver is an AssetHandle hashed from a root-tagged relative path and was already
        // immune; this was the only one that did not.
        //
        // IT WAS NOT THE ONLY ROOTED STRING IN A SCENE, and I10 finished the class one merge later.
        // `TextComponent.Font`, `UIText.Font`, `UIIcon.Icon` and `UIPanel.Video` also stored a path and
        // re-hashed it at load — the three service registries (FontService / IconService / VideoService)
        // are path-keyed through AssetHandle::FromCookedPath, so a scene could not store a handle for
        // them. They were safe for every value this repository ships (all 5 fonts and all 8 icons named
        // the ENGINE trees Resources/Fonts and Resources/Icons, which SetProjectRoot never remaps) and
        // broken for anything dropped in from the project's own assets tree, which both scan roots
        // accept. They are root-tagged keys now too, at scene v17, through one pair of functions in
        // Core/Serialize/ComponentRegistry.cpp. Every reference a `.desce` carries is now either an
        // AssetHandle resolved through MakeAssetResolver or a root-tagged key.
        //
        // WHY A KEY AND NOT A HANDLE, which is the other way this could have been fixed. An AssetHandle
        // IS the FNV-1a of exactly this string, so the hash carries no location the key does not — what
        // makes an asset reference survive the remap is the TAG plus the relative path, not the hashing.
        // The hash is one-way, so a handle only becomes a path again through the asset REGISTRY, and a
        // script has no registry entry, no loader and no payload the registry could hold: ScriptEngine
        // opens the file itself and ScriptSystem polls its mtime. Minting a ScriptAsset class purely to
        // invert a hash we would have computed from the string we already have is a table with one
        // reader — the "mirror with only one reader" anti-pattern — so the string is stored as it is.
        std::string ScriptKey;

        // The file ScriptKey names, on THIS host, in THIS project, right now. One place, so no call site
        // can forget the resolution and reintroduce the defect above; PathForStableKey refuses a
        // bare relative string by name (an empty path: no script), so no reader guesses a root for it.
        std::filesystem::path ResolvedPath() const
        {
            return Common::AssetHandle::PathForStableKey( ScriptKey );
        }

        // The values this entity OVERRIDES of the properties its script declares (Scripting::ScriptSchema): set in
        // Details, serialized, written into the script env before it runs and every frame after. A property not
        // listed runs on the script's own default — a new default in the script reaches every entity that never
        // overrode it, the way a UE instance holds only its delta from the class default.
        std::vector<Scripting::ScriptProperty> Properties;

        bool Started = false; // transient: OnStart already called for this instance
        // Transient: the script's SaveGame property names (Scripting::ReadScriptSaveGameProperties), read
        // from the file each time the slot loads; ScriptSystem reads those properties back after the script ran.
        std::vector<std::string> SaveGameProperties;
        // Transient: why this slot is not running as written in the play world — its load error, else its last
        // OnUpdate error (ScriptEngine::SlotError); empty while it runs clean. Details shows it on the slot.
        std::string RuntimeError;
        // Transient: CallInEditor functions (Scripting::ScriptSchema::EditorFunctions) Details asked to run while
        // this slot plays; ScriptSystem calls each once on the running script in its next update.
        std::vector<std::string> PendingEditorCalls;
    };

    // One entity can run MANY scripts (EnTT allows only one component of a type per entity, so multiple
    // behaviors live as a LIST of slots inside this one component — the same composition UE gets from
    // multiple ActorComponents). Each slot is an independent sandbox (its own env + properties + lifecycle);
    // all slots share the same `self` entity. ScriptSystem ticks every slot; entity:call() broadcasts to all.
    /// A level sequence Event key's CallScript action (UE: an event endpoint on the bound actor): Lua function
    /// `Function` is called on every started slot with the key's name, by ScriptSystem in its next update.
    struct SequenceScriptCall
    {
        std::string Function;
        std::string EventName;
    };

    struct ScriptComponent
    {
        std::vector<ScriptSlot> Scripts;

        // Transient, like AnimationComponent::PendingNotifies: queued by LevelSequenceEntityHost::Fire, drained
        // (each call once) by ScriptSystem. Not serialized.
        std::vector<SequenceScriptCall> PendingSequenceCalls;
    };

    // UE-style SOCKET attachment: makes this entity follow a BONE of another (skinned) entity, not just its
    // root. The hand is a bone inside a Skeleton — it has no entity, so RelationshipComponent can't parent to
    // it. AttachmentSystem (after AnimationECSSystem) computes the bone's world transform from the target's
    // animator pose and writes it (plus the local offset) into this entity's TransformComponent each frame.
    // Used for weapons-in-hand, hats, backpacks, scope attachments, etc.
    struct SocketAttachmentComponent
    {
        // The skinned-mesh entity whose bone we follow. Null = detached, and it starts detached: a socket
        // that has not been pointed at anything must not claim to follow a target that does not exist.
        Common::UUID Target = Common::UUID::Null();
        // UE's AttachSocketName: a SOCKET of the target's skeleton (Skeleton::FindSocket - its bone and its local
        // transform come from the rig) or, when the rig has no socket of that name, a bone (UE
        // USkinnedMeshComponent::GetSocketTransform: socket first, then bone). E.g. "hand_r_grip",
        // "mixamorig:Head".
        std::string SocketName;

        // This attachment's own alignment ON TOP of the socket (or of the bone when the name is a bone) - UE's
        // relative transform of an attached component. The grip shared by every weapon belongs on the socket.
        glm::vec3 OffsetTranslation = { 0.0f, 0.0f, 0.0f };
        glm::vec3 OffsetRotation    = { 0.0f, 0.0f, 0.0f }; // euler radians
        glm::vec3 OffsetScale       = { 1.0f, 1.0f, 1.0f };
    };

    // A FLYING PROJECTILE (bullet / grenade / arrow) — UE UProjectileMovementComponent
    // (ProjectileMovementComponent.cpp). ECS/System/ProjectileMovement.cpp moves it in Play, after the physics
    // step: velocity Verlet with the world's gravity times Gravity Scale, swept as a sphere through the physics
    // world in sub-steps of at most kMaxSimulationTimeStep; a blocking hit either bounces it (Should Bounce) or
    // stops it (UE StopSimulating). Every blocking hit is a Hit fact of the frame's PhysicsEventQueue for BOTH
    // sides (UE DispatchBlockingHit -> ReceiveHit), so the scripts of the projectile and of what it struck get
    // OnHit(other, point, normal, impulse). Mechanism here; what a hit MEANS (damage, despawn) is the script's.
    // Speeds are cm/s, lengths cm.
    struct ProjectileData
    {
        REFLECT()

        // UE Velocity: the world-space velocity it is launched with (and, in Play, the current one).
        PROPERTY( DisplayName( "Velocity" ), Category( "Projectile" ), Units( "cm/s" ), Summary )
        glm::vec3 Velocity = { 0.0f, 0.0f, 0.0f };

        // UE MaxSpeed: the speed the velocity is clamped to; 0 = no limit.
        PROPERTY( DisplayName( "Max Speed" ), Category( "Projectile" ), Range( 0.0f, 100000.0f ), Units( "cm/s" ) )
        float MaxSpeed = 0.0f;

        // UE ProjectileGravityScale: times the physics world's gravity (0 = a straight line).
        PROPERTY( DisplayName( "Gravity Scale" ), Category( "Projectile" ), Range( 0.0f, 10.0f ), Summary )
        float GravityScale = 1.0f;

        // The radius of the sphere swept through the world (UE: the UpdatedComponent's collision sphere).
        PROPERTY( DisplayName( "Collision Radius" ), Category( "Projectile" ), Range( 0.1f, 500.0f ), Length )
        float CollisionRadius = 2.0f;

        // UE bRotationFollowsVelocity: the entity faces along its velocity (its -Z, the engine's forward).
        PROPERTY( DisplayName( "Rotation Follows Velocity" ), Category( "Projectile" ) )
        bool RotationFollowsVelocity = false;

        // UE InitialLifeSpan: seconds after Play / spawn before the entity is destroyed; 0 = it lives on.
        PROPERTY( DisplayName( "Life Span" ), Category( "Projectile" ), Range( 0.0f, 600.0f ), Units( "s" ) )
        float LifeSpan = 5.0f;

        // UE bShouldBounce: a blocking hit reflects the velocity instead of stopping the projectile.
        PROPERTY( DisplayName( "Should Bounce" ), Category( "Projectile Bounces" ) )
        bool ShouldBounce = false;

        // UE Bounciness: the fraction of the velocity along the hit normal kept after a bounce.
        PROPERTY( DisplayName( "Bounciness" ), Category( "Projectile Bounces" ), Range( 0.0f, 1.0f ) )
        float Bounciness = 0.6f;

        // UE Friction: the fraction of the velocity along the surface lost in a bounce.
        PROPERTY( DisplayName( "Friction" ), Category( "Projectile Bounces" ), Range( 0.0f, 1.0f ) )
        float Friction = 0.2f;

        // UE BounceVelocityStopSimulatingThreshold: below this speed after a bounce it stops.
        PROPERTY( DisplayName( "Bounce Stop Speed" ), Category( "Projectile Bounces" ), Range( 0.0f, 1000.0f ),
                  Units( "cm/s" ), Advanced )
        float BounceVelocityStopSimulatingThreshold = 5.0f;
    };

    struct ProjectileComponent
    {
        COMPONENT( Key( "Projectile" ), Block( Data ), Run( ActorsAndUI ) )
        ProjectileData Data;

        // Transient (Play only). UE GetInstigator / MoveIgnoreActors: the shooter, whose bodies the sweep goes
        // through; stamped by the firing script (entity:launch). Null = nothing ignored.
        entt::entity Instigator = entt::null;
        float        Age        = 0.0f;  // seconds since Play / spawn, against Life Span
        bool         Stopped    = false; // UE HasStoppedSimulation: a blocking hit ended the flight
    };
} // namespace Desert::ECS