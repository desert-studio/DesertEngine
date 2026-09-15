#pragma once

#include "System.hpp"
#include "SystemRules.hpp"

#include <Engine/ECS/Components.hpp>
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
#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBR.hpp>

#include <Engine/Runtime/SelectionContext.hpp>

namespace Desert::ECS
{
    class MeshECSSystem : public System
    {
    public:
        explicit MeshECSSystem() : System()
        {
            // The fallback for a mesh with no material slot at all — one PBR surface per vertex path,
            // the same surface on both. They are two objects and not one because a material owns the
            // descriptor sets of ONE shader, and the two paths are two shaders (MeshVertexPath.hpp).
            m_DefaultMaterial        = Graphic::MaterialPBR::Create( Graphic::MeshVertexPath::Static );
            m_DefaultSkinnedMaterial = Graphic::MaterialPBR::Create( Graphic::MeshVertexPath::Skinned );
        }

        // Render-data collector (only touches mesh components' runtime caches) — safe to run concurrently with the other collectors.
        bool CanRunParallel() const override
        {
            return true;
        }

        void Update( entt::registry& registry, Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                     const Common::Timestep& /*ts*/ ) override
        {
            // Frame-constant invalidation stamp: cached instance sets built against an older stamp
            // rebuild below (their parent Material may have been graveyarded by Invalidate()).
            const uint32_t materialsVersion =
                 Runtime::ResourceRegistry::GetMaterialService()->GetInvalidationVersion();

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
                         if ( registry.has<VisibilityComponent>( entity ) &&
                              !registry.get<VisibilityComponent>( entity ).Visible )
                             return;

                         if ( !mesh.RuntimeMesh && !mesh.Primitive.has_value() && !mesh.MeshHandle )
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

                         // --- Auto-Initialize Material Slots ---
                         // If the component has no materials assigned, try to fetch defaults from the asset.
                         // ALL-OR-NOTHING: an external id that doesn't resolve yet (material registered
                         // later than the mesh) leaves the slots EMPTY so this retries next frame —
                         // pushing Null() handles would pass the empty() gate forever and freeze the
                         // mesh on the fallback material.
                         if ( mesh.MaterialSlots.empty() && mesh.MeshHandle )
                         {
                             auto* meshAsset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( mesh.MeshHandle );
                             if ( meshAsset )
                             {
                                 const auto& defaultHandles = meshAsset->GetMaterialHandles();
                                 std::vector<Assets::AssetHandle> resolved;
                                 resolved.reserve( defaultHandles.size() );
                                 for ( const auto& h : defaultHandles )
                                 {
                                     const auto internal =
                                          Runtime::ResourceRegistry::GetMaterialService()->GetAssetHandleByExternal( h );
                                     if ( internal.IsNull() )
                                     {
                                         resolved.clear();
                                         break;
                                     }
                                     resolved.push_back( internal );
                                 }
                                 if ( !resolved.empty() )
                                     mesh.MaterialSlots = std::move( resolved );
                             }
                         }

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
                                 mesh.RuntimeMaterialInstances.push_back( m_DefaultMaterial->CreateInstance() );
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
                                          inst ? std::move( inst ) : m_DefaultMaterial->CreateInstance() );
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
                             mesh.RuntimeSlots = std::move( binding );

                             // One-shot seed, CONSUMED here: PBR-channel MaterialComponent params exist only
                             // as a hand-off buffer (scripts that ran before this build + legacy scenes).
                             // They land as slot-0 instance overrides once and the buffer is cleared — the
                             // authored slots stay the single source of truth, nothing re-applies per frame,
                             // and slot edits in the editor can never be silently shadowed. Live script
                             // writes go straight to the instance (ScriptEntity::SetMaterialParam).
                             if ( registry.has<MaterialComponent>( entity ) &&
                                  !mesh.RuntimeMaterialInstances.empty() )
                             {
                                 auto& matc = registry.get<MaterialComponent>( entity );
                                 if ( ( matc.ShaderName.empty() || matc.ShaderName == "StaticMeshPBR" ) &&
                                      !matc.Params.empty() )
                                 {
                                     auto& inst = mesh.RuntimeMaterialInstances[0];
                                     for ( const auto& p : matc.Params )
                                         inst->SetParamFromVec4( p.Name, p.Value );
                                     matc.Params.clear();
                                     matc.Textures.clear();
                                 }
                             }
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

                         // A MaterialComponent assigning a NON-PBR shader takes this mesh off the batched
                         // PBR path onto the generic per-object data-driven path.
                         if ( registry.has<MaterialComponent>( entity ) )
                         {
                             const auto& matc = registry.get<MaterialComponent>( entity );
                             if ( !matc.ShaderName.empty() && matc.ShaderName != "StaticMeshPBR" &&
                                  matc.ShaderName != "SkinnedMeshPBR" )
                             {
                                 std::vector<std::pair<std::string, glm::vec4>> overrides;
                                 overrides.reserve( matc.Params.size() );
                                 for ( const auto& p : matc.Params )
                                     overrides.emplace_back( p.Name, p.Value );

                                 std::vector<std::pair<std::string, uint64_t>> texOverrides;
                                 texOverrides.reserve( matc.Textures.size() );
                                 for ( const auto& t : matc.Textures )
                                     texOverrides.emplace_back( t.Name, t.TextureHandle );

                                 // This draw REPLACES the entity's PBR draw (note the return), so it is
                                 // the only draw that could carry the caster — and before it did, a
                                 // Shader Override mesh cast no shadow at all.
                                 const bool overrideCasts =
                                      Rules::RouteMeshShadowCaster( mesh.CastShadows, /*shaderOverride*/ true,
                                                                    /*slotDrawCount*/ 0,
                                                                    /*pbrDrawEmitted*/ false ) ==
                                      Rules::MeshShadowCaster::ShaderOverride;

                                 renderCommandBuffer.Emplace<Graphic::Render::DrawGenericMeshCommand>(
                                      targetMesh, worldTransform, matc.ShaderName,
                                      Graphic::MaterialOverrides{ std::move( overrides ),
                                                                  std::move( texOverrides ) },
                                      outlined, /*directTexture*/ nullptr, /*directTextureSampler*/ std::string{},
                                      overrideCasts );
                                 return; // skip the PBR path for this entity
                             }
                         }

                         // ── v3 per-slot shader routing ──────────────────────────────────────
                         // Submesh i uses slot min(i, slots-1). Submeshes whose slot material is
                         // a custom-shader material (DataDrivenMaterial) leave the batched PBR
                         // path and are drawn per-slot through the generic path; the PBR draw
                         // masks them out. Materials are MaterialService-owned -> pointers are
                         // stable for the frame.
                         uint64_t customMask = 0;
                         struct SlotDraw
                         {
                             Graphic::Material* Mat;
                             uint64_t           Mask;
                         };
                         std::vector<SlotDraw> slotDraws;

                         const size_t submeshCount =
                              std::min<size_t>( targetMesh->GetSubmeshes().size(), 64 );
                         const size_t materialSlotCount = mesh.RuntimeMaterialInstances.size();
                         for ( size_t si = 0; si < submeshCount && materialSlotCount > 0; ++si )
                         {
                             const size_t slot = std::min( si, materialSlotCount - 1 );
                             auto* inst = mesh.RuntimeMaterialInstances[slot].get();
                             auto* parent = inst ? inst->GetParentMaterial() : nullptr;
                             if ( !dynamic_cast<Graphic::DataDrivenMaterial*>( parent ) )
                                 continue;

                             customMask |= ( 1ull << si );
                             bool merged = false;
                             for ( auto& d : slotDraws )
                                 if ( d.Mat == parent )
                                 {
                                     d.Mask |= ( 1ull << si );
                                     merged = true;
                                     break;
                                 }
                             if ( !merged )
                                 slotDraws.push_back( { parent, 1ull << si } );
                         }

                         // Decided BEFORE anything is emitted, because the caster belongs to the ENTITY:
                         // the shadow pass draws a mesh whole, so the PBR draw and the slot draws are
                         // candidates for the same silhouette and only one of them may record it.
                         const uint64_t allMask = submeshCount >= 64 ? ~0ull : ( ( 1ull << submeshCount ) - 1ull );
                         const uint64_t pbrHidden      = mesh.HiddenSubmeshes | customMask;
                         const bool     pbrDrawEmitted = submeshCount == 0 || ( ~pbrHidden & allMask ) != 0;

                         const auto shadowRoute = Rules::RouteMeshShadowCaster(
                              mesh.CastShadows, /*shaderOverride*/ false, slotDraws.size(), pbrDrawEmitted );

                         bool slotCasterPlaced = false;
                         for ( const auto& d : slotDraws )
                         {
                             const uint64_t visible = d.Mask & ~mesh.HiddenSubmeshes;
                             if ( !visible )
                                 continue;

                             // "First slot draw" means the first one actually EMITTED — a leading slot
                             // whose submeshes are all hidden emits nothing, and routing the caster to it
                             // would drop the entity's shadow instead of moving it.
                             const bool casts =
                                  !slotCasterPlaced && shadowRoute == Rules::MeshShadowCaster::FirstSlotDraw;
                             slotCasterPlaced = slotCasterPlaced || casts;

                             renderCommandBuffer.Emplace<Graphic::Render::DrawSlotMaterialMeshCommand>(
                                  targetMesh, worldTransform, d.Mat, visible, outlined, casts );
                         }

                         // PBR path draws the remaining submeshes (skip entirely when every
                         // submesh went custom).
                         if ( pbrDrawEmitted )
                             renderCommandBuffer.Emplace<Graphic::Render::DrawStaticMeshCommand>(
                                  targetMesh, mesh.RuntimeSlots, worldTransform, outlined, pbrHidden,
                                  mesh.ForcedLOD, mesh.LODBias, shadowRoute == Rules::MeshShadowCaster::PbrDraw,
                                  mesh.ReceiveShadows );
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
                         if ( registry.has<VisibilityComponent>( entity ) &&
                              !registry.get<VisibilityComponent>( entity ).Visible )
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

                         // Invalidation stamp (see the static path) — rebuild on any Invalidate().
                         if ( ism.SeenMaterialsVersion != materialsVersion )
                         {
                             ism.RuntimeMaterialInstances.clear();
                             ism.SeenMaterialsVersion = materialsVersion;
                         }

                         // One PBR material instance (slot 0), rebuilt only when the slot set changes.
                         const size_t slotCount = ism.MaterialSlots.empty() ? 1 : ism.MaterialSlots.size();
                         if ( ism.RuntimeMaterialInstances.size() != slotCount )
                         {
                             ism.RuntimeMaterialInstances.clear();
                             if ( ism.MaterialSlots.empty() )
                                 ism.RuntimeMaterialInstances.push_back( m_DefaultMaterial->CreateInstance() );
                             else
                                 for ( const auto& h : ism.MaterialSlots )
                                 {
                                     auto inst = Runtime::ResourceRegistry::GetMaterialService()
                                                      ->CreateRuntimeInstance( h );
                                     ism.RuntimeMaterialInstances.push_back(
                                          inst ? std::move( inst ) : m_DefaultMaterial->CreateInstance() );
                                 }
                         }
                         if ( ism.RuntimeMaterialInstances.empty() )
                             return;

                         // ISM draws through the batched PBR instancing path — a custom-shader slot
                         // material can't drive it. Use the first PBR slot; if none, warn once and
                         // skip (per-instance generic draws would defeat the point of an ISM).
                         // CO-OWNED, for the same reason the static path's slots are: this instance is
                         // owned by THIS component's RuntimeMaterialInstances, and the entity can be
                         // destroyed by Lua between recording the draw and executing it.
                         Graphic::MaterialInstancePtr ismInstancePtr;
                         for ( const auto& inst : ism.RuntimeMaterialInstances )
                         {
                             if ( inst && !dynamic_cast<Graphic::DataDrivenMaterial*>(
                                               inst->GetParentMaterial() ) )
                             {
                                 ismInstancePtr = inst;
                                 break;
                             }
                         }
                         if ( !ismInstancePtr )
                         {
                             static bool s_WarnedCustomISM = false;
                             if ( !s_WarnedCustomISM )
                             {
                                 LOG_WARN( "Instanced Static Mesh doesn't support custom-shader materials "
                                           "(instancing is a PBR-path optimization) — entity skipped. "
                                           "Assign a PBR material." );
                                 s_WarnedCustomISM = true;
                             }
                             return;
                         }

                         // InstanceTransforms are WORLD-space (the entity is a container). The command
                         // carries a CO-OWNED snapshot, not the component's address: see the member's own
                         // note and Graphic::MaterialSlotBinding (A8-3). The comparison is what makes the
                         // snapshot impossible to leave stale.
                         if ( !ism.RuntimeInstanceSnapshot ||
                              *ism.RuntimeInstanceSnapshot != ism.InstanceTransforms )
                             ism.RuntimeInstanceSnapshot =
                                  std::make_shared<const std::vector<glm::mat4>>( ism.InstanceTransforms );

                         renderCommandBuffer.Emplace<Graphic::Render::DrawInstancedStaticMeshCommand>(
                              targetMesh, ismInstancePtr, ism.RuntimeInstanceSnapshot );
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
                         if ( registry.has<VisibilityComponent>( entity ) &&
                              !registry.get<VisibilityComponent>( entity ).Visible )
                             return;

                         // Editor-built runtime rig (Convert to Skinned) takes priority over the cooked asset.
                         Desert::Mesh* baseMesh = mesh.RuntimeMesh.get();
                         if ( !baseMesh )
                             baseMesh = Runtime::ResourceRegistry::GetMeshService()->Get( mesh.MeshHandle );
                         if ( !baseMesh || !baseMesh->IsSkinned() )
                             return;
                         auto* skinnedMesh = static_cast<Desert::SkinnedMesh*>( baseMesh );

                         // One skinned PBR material instance (default if no slot assigned), rebuilt only when
                         // the slot set changes.
                         // Invalidation stamp (see the static path) — rebuild on any Invalidate().
                         if ( mesh.SeenMaterialsVersion != materialsVersion )
                         {
                             mesh.RuntimeMaterialInstances.clear();
                             mesh.SeenMaterialsVersion = materialsVersion;
                         }
                         const size_t slotCount = mesh.MaterialSlots.empty() ? 1 : mesh.MaterialSlots.size();
                         if ( mesh.RuntimeMaterialInstances.size() != slotCount )
                         {
                             mesh.RuntimeMaterialInstances.clear();
                             if ( mesh.MaterialSlots.empty() )
                                 mesh.RuntimeMaterialInstances.push_back(
                                      m_DefaultSkinnedMaterial->CreateInstance() );
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
                                               : m_DefaultSkinnedMaterial->CreateInstance() );
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

                         renderCommandBuffer.Emplace<Graphic::Render::DrawSkinnedMeshCommand>(
                              skinnedMesh, slots, worldTransform, boneMatrices, isSelected, mesh.CastShadows );
                     } );
            }
        }

    private:
        std::shared_ptr<Graphic::MaterialPBR> m_DefaultMaterial;
        std::shared_ptr<Graphic::MaterialPBR> m_DefaultSkinnedMaterial;
    };
} // namespace Desert::ECS