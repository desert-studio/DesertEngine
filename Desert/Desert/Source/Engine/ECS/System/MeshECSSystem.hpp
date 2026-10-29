#pragma once

#include "System.hpp"
#include "SystemRules.hpp"
#include "FracturePieceDraw.hpp"
#include "LandscapeSplineMeshDraw.hpp"
#include "WindField.hpp"

#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/MaterialSlotAdoption.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Animation/Animator.hpp>

#include <Engine/Graphic/Render/Commands/DrawMeshCommand.hpp>
#include <Engine/Graphic/Render/Commands/DrawSkinnedMeshCommand.hpp>
#include <Engine/Graphic/Render/Commands/DrawGenericMeshCommand.hpp>
#include <Engine/Graphic/Render/Commands/DrawSlotMaterialMeshCommand.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Geometry/PrimitiveMeshFactory.hpp>
#include <Engine/Geometry/SkinnedMesh.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Graphic/Materials/Mesh/MeshVertexPath.hpp>

#include <Engine/Runtime/SelectionContext.hpp>

namespace Desert::ECS
{
    class MeshECSSystem : public System
    {
    public:
        explicit MeshECSSystem() : System()
        {
        }

        // Render-data collector (only touches mesh components' runtime caches) — safe to run concurrently with the other collectors.
        bool CanRunParallel() const override
        {
            return true;
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& ts ) override
        {
            // Foliage wind's clock (FO-7): the GAMEPLAY step the scene hands every system - the fixed step
            // under --play, zero while editing - accumulated, never a wall clock, so one time is one pose.
            m_PrevWindSeconds = m_WindSeconds;
            m_WindSeconds += static_cast<double>( ts.GetSeconds() );

            // Frame-constant invalidation stamp: cached instance sets built against an older stamp
            // rebuild below (their parent Material may have been graveyarded by Invalidate()).
            const uint32_t materialsVersion =
                 Runtime::ResourceRegistry::GetMaterialService()->GetInvalidationVersion();

            /* =========================
               FRACTURED ENTITIES (DST-06): their pieces go out as static-mesh draws, through the same
               instanced path below; the entities Record returns do not draw their static mesh.
               ========================= */
            FracturePieceDraw::Services pieceServices;
            pieceServices.SourceSlots = []( const Assets::AssetHandle& meshHandle )
            {
                // The mesh asset's section materials, by the same per-section rule a component's Null slots
                // take them by; the pieces cache no instances of their own here.
                std::vector<Assets::AssetHandle>          slots;
                std::vector<Graphic::MaterialInstancePtr> noInstances;
                AdoptMeshMaterialSlots( slots, noInstances, meshHandle );
                return slots;
            };
            pieceServices.Instance = []( const Assets::AssetHandle& material )
            { return Runtime::ResourceRegistry::GetMaterialService()->CreateRuntimeInstance( material ); };
            pieceServices.DefaultInstance = [this]()
            { return DefaultInstance( Graphic::MeshVertexPath::Static ); };
            pieceServices.MaterialByGuid = []( const Common::Content::AssetGuid& guid )
            {
                return Runtime::ResourceRegistry::GetMaterialService()->GetAssetHandleByExternal(
                     Assets::AssetHandle( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) ) );
            };
            pieceServices.SlotMaterial = []( const Graphic::MaterialInstance* instance )
            { return CustomSlotMaterial( instance ); };
            pieceServices.Fracture = []( const Assets::AssetHandle& fracture )
            {
                auto read = Runtime::ResourceRegistry::GetFractureService()->Get( fracture );
                return read.IsSuccess() ? read.GetValue() : std::shared_ptr<const Destruction::FractureData>();
            };
            const auto drawnAsPieces =
                 m_Pieces.Record( registry, renderCommandBuffer, materialsVersion, pieceServices );

            /* =========================
               LANDSCAPE SPLINE MESHES (L9-MESHES): each segment's meshes, bent along it, as static draws.
               ========================= */
            LandscapeSplineMeshDraw::Services splineServices;
            splineServices.MeshAsset = []( const Common::UUID& id ) -> const Assets::StaticMeshAsset*
            {
                return dynamic_cast<const Assets::StaticMeshAsset*>(
                     Runtime::ResourceRegistry::GetMeshService()->GetAsset(
                          Assets::AssetHandle( static_cast<uint64_t>( id ) ) ) );
            };
            splineServices.Instance = []( const Common::UUID& id )
            {
                auto* materials = Runtime::ResourceRegistry::GetMaterialService();
                return materials->CreateRuntimeInstance( materials->GetAssetHandleByExternal( id ) );
            };
            splineServices.DefaultInstance = [this]()
            { return DefaultInstance( Graphic::MeshVertexPath::Static ); };
            m_SplineMeshes.Record( registry, renderCommandBuffer, materialsVersion, splineServices );

            /* =========================
               STATIC MESHES
               ========================= */
            {
                auto view = registry.view<StaticMeshComponent, TransformComponent>();

                // Frame-constant: read the current selection ONCE, not per entity (256x/frame otherwise).
                const auto& selectedAll = Runtime::SelectionContext::GetAll();

                view.each(
                     [&]( entt::entity entity, StaticMeshComponent& mesh,
                          const TransformComponent& transform )
                     {
                         // Hidden entities (Visible toggle) are skipped.
                         if ( ECS::IsHidden( registry, entity ) )
                             return;

                         if ( !mesh.RuntimeMesh && !mesh.Primitive.has_value() && !mesh.MeshHandle )
                             return;

                         // Its pieces stand in for it (FracturePieceDraw).
                         if ( drawnAsPieces.contains( entity ) )
                             return;

                         Desert::Mesh* targetMesh = nullptr;

                         if ( mesh.RuntimeMesh )
                         {
                             // Use unique modified mesh
                             targetMesh = mesh.RuntimeMesh.get();
                         }
                         else if ( mesh.Primitive.has_value() )
                         {
                             // Use the process-wide SHARED primitive mesh (one Mesh* per type) so identical
                             // primitives batch via instancing. NOT stored in RuntimeMesh — leaving it null
                             // keeps the entity on the shared mesh; the mesh editor forks a per-entity
                             // RuntimeMesh on edit (copy-on-edit), which then takes priority above.
                             targetMesh = Geometry::PrimitiveMeshFactory::GetShared( mesh.Primitive.value() );
                         }
                         else if ( mesh.MeshHandle )
                         {
                             // Asset-based mesh
                             targetMesh = Runtime::ResourceRegistry::GetMeshService()->Get( mesh.MeshHandle );
                         }

                         if ( !targetMesh )
                             return;

                         // --- Auto-Initialize Material Slots --- (the one rule, AdoptMeshMaterialSlots)
                         AdoptMeshMaterialSlots( mesh.MaterialSlots, mesh.RuntimeMaterialInstances,
                                                 mesh.MeshHandle );

                         // A MaterialService::Invalidate() this frame dropped some runtime Material —
                         // rebuild every cached instance set (parents may be graveyarded). One uint
                         // compare per entity; without it a stale RuntimeMaterialInstances would keep
                         // a dangling GetParentMaterial() pointer past the next CollectGarbage().
                         if ( mesh.SeenMaterialsVersion != materialsVersion )
                         {
                             mesh.RuntimeMaterialInstances.clear();
                             mesh.SeenMaterialsVersion = materialsVersion;
                         }

                         // Ensure runtime material instances are initialized and match the slots
                         size_t slotCount = mesh.MaterialSlots.empty() ? 1 : mesh.MaterialSlots.size();

                         if ( mesh.RuntimeMaterialInstances.size() != slotCount )
                         {
                             mesh.RuntimeMaterialInstances.clear();
                             mesh.RuntimeMaterialInstances.reserve( slotCount );

                             if ( mesh.MaterialSlots.empty() )
                             {
                                 // Use persistent system default material template
                                 mesh.RuntimeMaterialInstances.push_back(
                                      DefaultInstance( Graphic::MeshVertexPath::Static ) );
                             }
                             else
                             {
                                 for ( const auto& assetHandle : mesh.MaterialSlots )
                                 {
                                     // Service-owned resolution: base assets give a plain instance,
                                     // material-INSTANCE assets give an instance of their base with
                                     // the child overrides applied.
                                     auto inst = Runtime::ResourceRegistry::GetMaterialService()
                                                      ->CreateRuntimeInstance( assetHandle );
                                     mesh.RuntimeMaterialInstances.push_back(
                                          inst ? std::move( inst )
                                               : DefaultInstance( Graphic::MeshVertexPath::Static ) );
                                 }
                             }

                             // Rebuild the render path's binding ONLY here (when the instance set changes),
                             // not every frame: what the draw commands then carry is a shared_ptr COPY of
                             // it, which is one atomic increment and no allocation, and which keeps both
                             // the slot array and every instance in it alive for as long as any draw is
                             // still in flight. A fresh object rather than a mutation of the old one, for
                             // the reason the whole change exists: a command recorded last frame may still
                             // be holding the previous binding.
                             auto binding   = std::make_shared<Graphic::MaterialSlotBinding>();
                             binding->Owned = mesh.RuntimeMaterialInstances;
                             binding->Slots.reserve( binding->Owned.size() );
                             for ( const auto& inst : binding->Owned )
                                 binding->Slots.push_back( inst.get() );
                             // A displaced mesh (Runtime or Baked, its import settings) draws its displaced
                             // geometry with the displaced siblings of the same slots: base cells, so the
                             // relief is not marched a second time over the triangles that carry it.
                             if ( targetMesh->IsDisplaced() && !mesh.MaterialSlots.empty() )
                             {
                                 auto displaced = std::make_shared<Graphic::MaterialSlotBinding>();
                                 for ( const auto& assetHandle : mesh.MaterialSlots )
                                 {
                                     auto inst = Runtime::ResourceRegistry::GetMaterialService()
                                                      ->CreateRuntimeInstance(
                                                           assetHandle, Graphic::MeshVertexPath::Static, true );
                                     displaced->Owned.push_back(
                                          inst ? std::move( inst )
                                               : DefaultInstance( Graphic::MeshVertexPath::Static ) );
                                 }
                                 if ( displaced->Owned[0] )
                                     for ( const auto& p : mesh.ScriptSlotParams )
                                         displaced->Owned[0]->SetParamFromVec4( p.Name, p.Value );
                                 for ( const auto& inst : displaced->Owned )
                                     displaced->Slots.push_back( inst.get() );
                                 binding->Displaced = std::move( displaced );
                             }
                             mesh.RuntimeSlots = std::move( binding );

                             // The script's slot-0 overrides land on EVERY slot-0 instance built (UE: the
                             // MID a mesh component keeps across material changes). The list is the
                             // script's, not a hand-off buffer: nothing is consumed, so a rebuild after a
                             // material invalidation shows the same values the script last wrote.
                             if ( !mesh.RuntimeMaterialInstances.empty() && mesh.RuntimeMaterialInstances[0] )
                                 for ( const auto& p : mesh.ScriptSlotParams )
                                     mesh.RuntimeMaterialInstances[0]->SetParamFromVec4( p.Name, p.Value );
                         }

                         glm::mat4 worldTransform = transform.GetTransform();

                         entt::entity current = entity;
                         while ( registry.has<RelationshipComponent>( current ) )
                         {
                             const auto& rel = registry.get<RelationshipComponent>( current );
                             if ( rel.Parent == entt::null ) break;

                             current = rel.Parent;
                             if ( registry.has<TransformComponent>( current ) )
                             {
                                 const auto& parentTransform = registry.get<TransformComponent>( current );
                                 worldTransform = parentTransform.GetTransform() * worldTransform;
                             }
                         }

                         bool isSelected = false;
                         if ( !selectedAll.empty() )
                         {
                             // Check this entity itself (multi-selection aware)
                             if ( registry.has<UUIDComponent>( entity ) &&
                                  Runtime::SelectionContext::Contains(
                                       registry.get<UUIDComponent>( entity ).UUID ) )
                             {
                                 isSelected = true;
                             }
                             else
                             {
                                 // Walk ancestor chain — selecting a prefab root outlines all its mesh children
                                 entt::entity ancestor = entity;
                                 while ( registry.has<RelationshipComponent>( ancestor ) )
                                 {
                                     const auto& rel = registry.get<RelationshipComponent>( ancestor );
                                     if ( rel.Parent == entt::null )
                                         break;
                                     ancestor = rel.Parent;
                                     if ( registry.has<UUIDComponent>( ancestor ) &&
                                          Runtime::SelectionContext::Contains(
                                               registry.get<UUIDComponent>( ancestor ).UUID ) )
                                     {
                                         isSelected = true;
                                         break;
                                     }
                                 }
                             }
                         }

                         // Outline: editor selection OR the per-entity "Draw outline" toggle.
                         const bool outlined = isSelected || mesh.OutlineDraw;

                         // A MaterialComponent assigning a NON-Lit shader takes this mesh off the batched
                         // Lit path onto the generic per-object data-driven path.
                         if ( registry.has<MaterialComponent>( entity ) )
                         {
                             const auto& matc = registry.get<MaterialComponent>( entity );
                             if ( !matc.GetShaderName().empty() )
                             {
                                 std::vector<std::pair<std::string, glm::vec4>> overrides;
                                 overrides.reserve( matc.GetParams().size() );
                                 for ( const auto& p : matc.GetParams() )
                                     overrides.emplace_back( p.Name, p.Value );

                                 std::vector<std::pair<std::string, uint64_t>> texOverrides;
                                 texOverrides.reserve( matc.GetTextures().size() );
                                 for ( const auto& t : matc.GetTextures() )
                                     texOverrides.emplace_back( t.Name, t.TextureHandle );

                                 // This draw REPLACES the entity's lit draw (note the return), so it is
                                 // the only draw that could carry the caster — and before it did, a
                                 // Shader Override mesh cast no shadow at all.
                                 const bool overrideCasts = mesh.CastShadows;

                                 renderCommandBuffer.Emplace<Graphic::Render::DrawGenericMeshCommand>(
                                      static_cast<uint32_t>( entity ), targetMesh, worldTransform, matc.GetShaderName(),
                                      Graphic::MaterialOverrides{ std::move( overrides ),
                                                                  std::move( texOverrides ) },
                                      outlined, /*directTexture*/ nullptr, /*directTextureSampler*/ std::string{},
                                      overrideCasts, mesh.MaxDrawDistance, /*visibleInRayTracing*/ true );
                                 return; // skip the lit path for this entity
                             }
                         }

                         // ── Per-section draws (UE: each mesh section draws with its own material slot) ──
                         // Section i takes slot SectionMaterialSlot( i, slots ). A section whose slot material is
                         // a custom-shader material (DataDrivenMaterial without a mesh cell) leaves the batched
                         // passes for a per-slot generic draw; every other section is drawn by the batched passes
                         // with ITS slot, one draw per slot (Rules::SplitMeshSlotDraws, the rule fracture pieces
                         // split by too). Each draw masks out every submesh it does not draw.
                         const size_t submeshCount =
                              std::min<size_t>( targetMesh->GetSubmeshes().size(), 64 );
                         const auto split = Rules::SplitMeshSlotDraws<Graphic::Material*>(
                              submeshCount, mesh.RuntimeMaterialInstances.size(), mesh.HiddenSubmeshes,
                              mesh.CastShadows, [&]( size_t slot ) -> Graphic::Material*
                              { return CustomSlotMaterial( mesh.RuntimeMaterialInstances[slot].get() ); } );

                         for ( const auto& d : split.SlotDraws )
                             renderCommandBuffer.Emplace<Graphic::Render::DrawSlotMaterialMeshCommand>(
                                  static_cast<uint32_t>( entity ), targetMesh, worldTransform, d.Mat,
                                  d.VisibleMask, outlined, d.CastShadows, mesh.MaxDrawDistance );

                         for ( const auto& d : split.SectionDraws )
                             renderCommandBuffer.Emplace<Graphic::Render::DrawStaticMeshCommand>(
                                  static_cast<uint32_t>( entity ), targetMesh, mesh.RuntimeSlots,
                                  static_cast<uint32_t>( d.Slot ), worldTransform, outlined, d.HiddenMask,
                                  mesh.ForcedLOD, mesh.LODBias, d.CastShadows, mesh.ReceiveShadows,
                                  mesh.TranslucencySortPriority, mesh.MaxDrawDistance, /*motionPart*/ 0,
                                  mesh.CastContactShadow );
                     } );
            }

            /* =========================
               INSTANCED STATIC MESHES (UE-style ISM: one entity = N instances, one instanced draw)
               ========================= */
            {
                auto view = registry.view<InstancedStaticMeshComponent>();
                view.each(
                     [&]( entt::entity entity, InstancedStaticMeshComponent& ism )
                     {
                         if ( ECS::IsHidden( registry, entity ) )
                             return;
                         if ( ism.InstanceTransforms.empty() )
                             return;

                         // Resolve the single shared mesh (edited RuntimeMesh > primitive > asset handle).
                         Desert::Mesh* targetMesh = nullptr;
                         if ( ism.RuntimeMesh )
                             targetMesh = ism.RuntimeMesh.get();
                         else if ( ism.Primitive.has_value() )
                             targetMesh = Geometry::PrimitiveMeshFactory::GetShared( ism.Primitive.value() );
                         else if ( ism.MeshHandle )
                             targetMesh = Runtime::ResourceRegistry::GetMeshService()->Get( ism.MeshHandle );
                         if ( !targetMesh )
                             return;

                         // Null slots take the mesh asset's section materials, the static and skinned twins'
                         // one rule (UE: a foliage instance draws its mesh's materials unless overridden). The
                         // painted and procedural foliage spawners set only MeshHandle, so without this every
                         // field drew the default white surface, and a resimulate (which rebuilds the fields)
                         // dropped any slots copied in by hand. An edited RuntimeMesh or a primitive has no
                         // mesh asset to adopt from.
                         if ( !ism.RuntimeMesh && !ism.Primitive.has_value() )
                             AdoptMeshMaterialSlots( ism.MaterialSlots, ism.RuntimeMaterialInstances,
                                                     ism.MeshHandle );

                         // Invalidation stamp (see the static path) — rebuild on any Invalidate().
                         if ( ism.SeenMaterialsVersion != materialsVersion )
                         {
                             ism.RuntimeMaterialInstances.clear();
                             ism.SeenMaterialsVersion = materialsVersion;
                         }

                         // One material instance per slot, rebuilt only when the slot set changes.
                         const size_t slotCount = ism.MaterialSlots.empty() ? 1 : ism.MaterialSlots.size();
                         if ( ism.RuntimeMaterialInstances.size() != slotCount )
                         {
                             ism.RuntimeMaterialInstances.clear();
                             ism.RuntimeDisplacedInstances.clear();
                             // A displaced mesh's twins draw with the displaced siblings (base cells).
                             if ( targetMesh->IsDisplaced() )
                                 for ( const auto& h : ism.MaterialSlots )
                                 {
                                     auto inst = Runtime::ResourceRegistry::GetMaterialService()
                                                      ->CreateRuntimeInstance(
                                                           h, Graphic::MeshVertexPath::Static, true );
                                     ism.RuntimeDisplacedInstances.push_back(
                                          inst ? std::move( inst )
                                               : DefaultInstance( Graphic::MeshVertexPath::Static ) );
                                 }
                             if ( ism.MaterialSlots.empty() )
                                 ism.RuntimeMaterialInstances.push_back(
                                      DefaultInstance( Graphic::MeshVertexPath::Static ) );
                             else
                                 for ( const auto& h : ism.MaterialSlots )
                                 {
                                     auto inst = Runtime::ResourceRegistry::GetMaterialService()
                                                      ->CreateRuntimeInstance( h );
                                     ism.RuntimeMaterialInstances.push_back(
                                          inst ? std::move( inst )
                                               : DefaultInstance( Graphic::MeshVertexPath::Static ) );
                                 }
                         }
                         if ( ism.RuntimeMaterialInstances.empty() )
                             return;

                         // Per section, as a static mesh splits (Rules::SplitMeshSlotDraws): one instanced draw
                         // per surface slot, each masking the sections of the others. A custom-shader slot cannot
                         // drive the batched instancing path, and per-instance generic draws would defeat the
                         // point of an ISM: its sections are not drawn, said once. CO-OWNED instances, for the
                         // same reason the static path's slots are: they are owned by THIS component's
                         // RuntimeMaterialInstances, and the entity can be destroyed by Lua between recording the
                         // draw and executing it.
                         const auto ismSplit = Rules::SplitMeshSlotDraws<Graphic::Material*>(
                              std::min<size_t>( targetMesh->GetSubmeshes().size(), 64 ),
                              ism.RuntimeMaterialInstances.size(), /*hidden*/ 0ull, ism.CastShadows,
                              [&]( size_t slot ) -> Graphic::Material*
                              { return CustomSlotMaterial( ism.RuntimeMaterialInstances[slot].get() ); } );
                         if ( !ismSplit.SlotDraws.empty() )
                         {
                             static bool s_WarnedCustomISM = false;
                             if ( !s_WarnedCustomISM )
                             {
                                 LOG_WARN( "Instanced Static Mesh doesn't support custom-shader materials "
                                           "(instancing is a lit-path optimization) — the sections using one "
                                           "are not drawn. Assign a lit material." );
                                 s_WarnedCustomISM = true;
                             }
                         }
                         if ( ismSplit.SectionDraws.empty() )
                             return;

                         // InstanceTransforms are WORLD-space (the entity is a container). The command
                         // carries a CO-OWNED snapshot, not the component's address: see the member's own
                         // note and Graphic::MaterialSlotBinding (A8-3). The comparison is what makes the
                         // snapshot impossible to leave stale.
                         if ( !ism.RuntimeInstanceSnapshot ||
                              *ism.RuntimeInstanceSnapshot != ism.InstanceTransforms )
                             ism.RuntimeInstanceSnapshot =
                                  std::make_shared<const std::vector<glm::mat4>>( ism.InstanceTransforms );

                         // A foliage field fades by its type's CullDistance, read from the `.defoliage` itself
                         // (one source: the paint panel's edit re-reads into the same asset). A type still being
                         // read culls nothing for those frames; one that failed says why once, in the service.
                         // Its wind response (FO-7) comes from the same file; the wind itself is the scene's
                         // one query, ECS::WindAt, at the field entity's world origin (one push per draw, so one
                         // sample per field), at this frame's gameplay time.
                         Graphic::InstanceCullDistance cullDistance;
                         Graphic::InstanceWind         wind;
                         if ( const auto* foliage = registry.try_get<FoliageComponent>( entity );
                              foliage != nullptr && foliage->FoliageType )
                             if ( const auto* type = Runtime::ResourceRegistry::GetFoliageTypeService()->Get(
                                       foliage->FoliageType ) )
                             {
                                 cullDistance = { type->CullDistance.Min, type->CullDistance.Max };
                                 const glm::vec3 fieldOrigin =
                                      glm::vec3( Entity( entity, registry ).GetWorldTransform()[3] );
                                 const glm::vec3 air = ECS::WindAt( registry, fieldOrigin ).Velocity;
                                 wind = Graphic::MakeInstanceWind( type->Wind.Strength, type->Wind.Speed,
                                                                   type->Wind.Height, glm::vec2( air.x, air.z ),
                                                                   m_WindSeconds, m_PrevWindSeconds );
                             }
                         for ( const auto& d : ismSplit.SectionDraws )
                             renderCommandBuffer.Emplace<Graphic::Render::DrawInstancedStaticMeshCommand>(
                                  static_cast<uint32_t>( entity ), targetMesh,
                                  ism.RuntimeMaterialInstances[d.Slot], d.HiddenMask, ism.RuntimeInstanceSnapshot,
                                  d.CastShadows, ism.CastContactShadow,
                                  Graphic::ClipToMaxDrawDistance( cullDistance, ism.MaxDrawDistance ), wind,
                                  d.Slot < ism.RuntimeDisplacedInstances.size()
                                       ? ism.RuntimeDisplacedInstances[d.Slot]
                                       : Graphic::MaterialInstancePtr{} );
                     } );
            }

            /* =========================
               SKINNED MESHES
               ========================= */
            {
                // AnimationComponent is OPTIONAL: a skinned mesh with no animator renders in its BIND pose
                // (identity bone matrices) — so imported/rigged characters show up in the skeleton editor.
                auto view = registry.view<SkinnedMeshComponent, TransformComponent>();

                view.each(
                     [&]( entt::entity entity, SkinnedMeshComponent& mesh, const TransformComponent& transform )
                     {
                         if ( ECS::IsHidden( registry, entity ) )
                             return;

                         // Editor-built runtime rig (Convert to Skinned) takes priority over the cooked asset.
                         Desert::Mesh* baseMesh = mesh.RuntimeMesh.get();
                         if ( !baseMesh )
                             baseMesh = Runtime::ResourceRegistry::GetMeshService()->Get( mesh.MeshHandle );
                         if ( !baseMesh || !baseMesh->IsSkinned() )
                             return;
                         auto* skinnedMesh = static_cast<Desert::SkinnedMesh*>( baseMesh );

                         // THE COOKED SKINNED MESH NAMES ITS MATERIALS AS THE STATIC ONE DOES (the .skmesh
                         // submeshes' MaterialGuid, SkinnedMeshAsset::GetMaterialHandles): an entity with no slot
                         // of its own takes them, by the same rule. Without it every placed skinned mesh drew the
                         // grey default although its .demat and textures were written (THM1l, live on Fox.glb). A
                         // runtime rig (Convert to Skinned) carries its own slots.
                         if ( !mesh.RuntimeMesh )
                             AdoptMeshMaterialSlots( mesh.MaterialSlots, mesh.RuntimeMaterialInstances,
                                                     mesh.MeshHandle );

                         // One skinned lit material instance (default if no slot assigned), rebuilt only when
                         // the slot set changes.
                         // Invalidation stamp (see the static path) — rebuild on any Invalidate().
                         if ( mesh.SeenMaterialsVersion != materialsVersion )
                         {
                             mesh.RuntimeMaterialInstances.clear();
                             mesh.RuntimeDisplacedInstances.clear();
                             mesh.SeenMaterialsVersion = materialsVersion;
                         }
                         const size_t slotCount = mesh.MaterialSlots.empty() ? 1 : mesh.MaterialSlots.size();
                         if ( mesh.RuntimeMaterialInstances.size() != slotCount )
                         {
                             mesh.RuntimeMaterialInstances.clear();
                             if ( mesh.MaterialSlots.empty() )
                                 mesh.RuntimeMaterialInstances.push_back(
                                      DefaultInstance( Graphic::MeshVertexPath::Skinned ) );
                             else
                                 for ( const auto& h : mesh.MaterialSlots )
                                 {
                                     // THE SKINNED path of the SAME `.demat` the static twin uses. This
                                     // argument is the whole fix: the slot names a surface, this system
                                     // knows the geometry is skinned, and the pair resolves. Without it
                                     // the service answered with a static material and MeshRenderer
                                     // dropped the mesh without drawing anything.
                                     auto inst =
                                          Runtime::ResourceRegistry::GetMaterialService()->CreateRuntimeInstance(
                                               h, Graphic::MeshVertexPath::Skinned );
                                     mesh.RuntimeMaterialInstances.push_back(
                                          inst ? std::move( inst )
                                               : DefaultInstance( Graphic::MeshVertexPath::Skinned ) );
                                 }
                         }
                         if ( mesh.RuntimeMaterialInstances.empty() )
                             return;

                         // Built per frame here, as the raw vector it replaces was — the skinned path has
                         // no cached binding because it has no cached slot view to cache it beside. What
                         // changed is that the command now CO-OWNS the instances instead of copying bare
                         // pointers to instances this component alone keeps alive (A8-3).
                         auto slots   = std::make_shared<Graphic::MaterialSlotBinding>();
                         slots->Owned = mesh.RuntimeMaterialInstances;
                         slots->Slots.reserve( slots->Owned.size() );
                         for ( const auto& inst : slots->Owned )
                             slots->Slots.push_back( inst.get() );
                         // A displaced skinned mesh (Runtime, CONTRACT-DISPLACE §9.8) draws its twin with the
                         // displaced siblings of the same slots: base cells, so the relief is not marched again
                         // over the triangles that carry it (the static rule above).
                         if ( skinnedMesh->IsDisplaced() && !mesh.MaterialSlots.empty() )
                         {
                             if ( mesh.RuntimeDisplacedInstances.size() != mesh.MaterialSlots.size() )
                             {
                                 mesh.RuntimeDisplacedInstances.clear();
                                 for ( const auto& h : mesh.MaterialSlots )
                                 {
                                     auto inst =
                                          Runtime::ResourceRegistry::GetMaterialService()->CreateRuntimeInstance(
                                               h, Graphic::MeshVertexPath::Skinned, true );
                                     mesh.RuntimeDisplacedInstances.push_back(
                                          inst ? std::move( inst )
                                               : DefaultInstance( Graphic::MeshVertexPath::Skinned ) );
                                 }
                             }
                             auto displaced   = std::make_shared<Graphic::MaterialSlotBinding>();
                             displaced->Owned = mesh.RuntimeDisplacedInstances;
                             for ( const auto& inst : displaced->Owned )
                                 displaced->Slots.push_back( inst.get() );
                             slots->Displaced = std::move( displaced );
                         }
                         else
                             mesh.RuntimeDisplacedInstances.clear();

                         // Bone matrices: animated pose if an Animator exists, else bind pose (identity = the
                         // skeleton's rest shape, which stays correct after Phase-2 rest-pose edits since
                         // OffsetMatrix is recomputed alongside LocalBindTransform).
                         // Skeleton Edit (editor) pushes a bind-pose-preview UUID: while set, this entity
                         // renders in its BIND pose so bone-gizmo edits to LocalBindTransform are visible (an
                         // auto-playing clip would otherwise override them with the animated pose).
                         const bool bindPreview = registry.has<UUIDComponent>( entity ) &&
                                                  Runtime::SelectionContext::IsBindPosePreview(
                                                       registry.get<UUIDComponent>( entity ).UUID );

                         std::vector<glm::mat4> boneMatrices;
                         if ( !bindPreview && registry.has<AnimationComponent>( entity ) )
                         {
                             const auto& anim = registry.get<AnimationComponent>( entity );
                             if ( anim.Animator )
                                 boneMatrices = anim.Animator->GetPose().Matrices;
                         }
                         if ( boneMatrices.empty() )
                         {
                             // Proper BIND pose: bind matrix = chainGlobal * OffsetMatrix (NOT identity).
                             // Identity would render the RAW (unscaled, thousands-of-units) vertices; the
                             // chainGlobal*OffsetMatrix bind renders the mesh at its authored size and makes
                             // it line up with the bone overlay (which is drawn at chainGlobal). This used to
                             // be a hand-written copy of the same chain walk that Scene::Raycast and the
                             // bone overlay each had their own copy of.
                             skinnedMesh->GetSkeleton().WriteBindSkinningMatrices( boneMatrices );
                         }

                         bool isSelected =
                              registry.has<UUIDComponent>( entity ) &&
                              Runtime::SelectionContext::Contains(
                                   registry.get<UUIDComponent>( entity ).UUID );

                         // WORLD transform (walk the parent chain) — a skinned mesh parented to e.g. the
                         // character controller must follow it; using the local transform left it behind at
                         // the origin while the (parent-aware) camera moved away. Mirrors the static path.
                         glm::mat4    worldTransform = transform.GetTransform();
                         entt::entity current        = entity;
                         while ( registry.has<RelationshipComponent>( current ) )
                         {
                             const auto& rel = registry.get<RelationshipComponent>( current );
                             if ( rel.Parent == entt::null )
                                 break;
                             current = rel.Parent;
                             if ( registry.has<TransformComponent>( current ) )
                                 worldTransform =
                                      registry.get<TransformComponent>( current ).GetTransform() * worldTransform;
                         }

                         // Morph weights blend before skinning (MeshRenderer::UpdateMorphTargets), only for a
                         // mesh that has targets to blend.
                         std::vector<float> morphWeights;
                         uint64_t           morphRevision = 0;
                         if ( skinnedMesh->GetMorphTargetCount() > 0 && registry.has<MorphComponent>( entity ) )
                         {
                             const auto& morph = registry.get<MorphComponent>( entity );
                             morphWeights      = morph.GetWeights();
                             morphRevision     = morph.GetRevision();
                         }

                         renderCommandBuffer.Emplace<Graphic::Render::DrawSkinnedMeshCommand>(
                              static_cast<uint32_t>( entity ), skinnedMesh, slots, worldTransform, boneMatrices,
                              std::move( morphWeights ), morphRevision, isSelected, mesh.CastShadows );
                     } );
            }
        }

    private:
        // The pieces of fractured entities, drawn as static meshes (DST-06).
        FracturePieceDraw m_Pieces;
        // The landscapes' spline meshes, drawn as static meshes (L9-MESHES).
        LandscapeSplineMeshDraw m_SplineMeshes;

        // The material a slot instance draws with OFF the batched lit path: its parent when that is a
        // custom-shader material (a DataDrivenMaterial of a DSL surface's own cell), null when the batched path
        // draws it (a mesh-table cell's material, or no material). The one test for the static path and the
        // fracture pieces. Materials are MaterialService-owned -> the pointer is stable for the frame.
        static Graphic::Material* CustomSlotMaterial( const Graphic::MaterialInstance* instance )
        {
            auto* parent = instance ? instance->GetParentMaterial() : nullptr;
            if ( const auto* surface = dynamic_cast<const Graphic::DataDrivenMaterial*>( parent );
                 surface == nullptr || Graphic::MeshCellPath( surface->GetShaderName() ) )
                return nullptr;
            return parent;
        }

        // A component's Null slots take its mesh asset's materials, PER SECTION (static and skinned alike;
        // the rule is ECS::AdoptSectionMaterials). A section whose material is not registered yet keeps the
        // default surface and picks the real one up the frame it resolves — the cached instances are dropped
        // so the slot loop rebuilds them. Gated by HasUnadoptedSlot: a fully resolved entity costs one scan.
        static void AdoptMeshMaterialSlots( std::vector<Assets::AssetHandle>&          slots,
                                            std::vector<Graphic::MaterialInstancePtr>& instances,
                                            const Assets::AssetHandle&                 meshHandle )
        {
            if ( meshHandle.IsNull() || !HasUnadoptedSlot( slots ) )
                return;
            auto* meshAsset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( meshHandle );
            if ( meshAsset == nullptr )
                return;
            auto* materials = Runtime::ResourceRegistry::GetMaterialService();
            if ( AdoptSectionMaterials( slots, meshAsset->GetMaterialHandles(),
                                        [materials]( const Common::UUID& id )
                                        { return materials->GetAssetHandleByExternal( id ); } ) )
                instances.clear();
        }

        // The fallback for a mesh with no material slot at all — the DEFAULT SURFACE template's cell per vertex
        // path (found by that role, MaterialService::DefaultSurfaceShader), built like every other surface
        // material (a DataDrivenMaterial of that cell). Two objects and not one because a material owns the
        // descriptor sets of ONE shader, and the two paths are two shaders (MeshVertexPath.hpp). Built on first
        // use: the template registry is the asset manager's, which exists after the systems do.
        Graphic::MaterialInstancePtr DefaultInstance( Graphic::MeshVertexPath path )
        {
            auto& material =
                 path == Graphic::MeshVertexPath::Skinned ? m_DefaultSkinnedMaterial : m_DefaultMaterial;
            if ( !material )
            {
                const auto shader = Runtime::ResourceRegistry::GetMaterialService()->DefaultSurfaceShader(
                     path, Graphic::MeshPass::Forward );
                if ( !shader )
                {
                    if ( !m_DefaultRefusalLogged )
                        LOG_ERROR( "[MeshECSSystem] a mesh with no material draws nothing: {}",
                                   shader.GetError() );
                    m_DefaultRefusalLogged = true;
                    return nullptr;
                }
                material = std::make_shared<Graphic::DataDrivenMaterial>( shader.GetValue() );
            }
            return material->CreateInstance();
        }

        std::shared_ptr<Graphic::DataDrivenMaterial> m_DefaultMaterial;
        std::shared_ptr<Graphic::DataDrivenMaterial> m_DefaultSkinnedMaterial;
        bool                                         m_DefaultRefusalLogged = false;
        // Gameplay seconds since the scene's systems started (FO-7). Double: a float clock loses the sway's
        // sub-frame steps after a few hours of play; MakeInstanceWind wraps it to the sway period.
        double m_WindSeconds = 0.0;
        double m_PrevWindSeconds = 0.0; // the clock one gameplay step ago: the instances' velocity
    };
} // namespace Desert::ECS