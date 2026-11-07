#pragma once

#include "System.hpp"
#include "SystemRules.hpp"
#include "FracturePieceDraw.hpp"
#include "LandscapeSplineMeshDraw.hpp"
#include "SelectionOutlineMask.hpp"
#include "WindField.hpp"

#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/EntityVisibility.hpp>
#include <Engine/ECS/MaterialSlotAdoption.hpp>
#include <Engine/ECS/RenderStateDelta.hpp>
#include <Engine/Geometry/MeshBounds.hpp>
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

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace Desert::ECS
{
    class MeshECSSystem : public System
    {
    public:
        explicit MeshECSSystem() : System()
        {
        }

        // The Shader Override draws' params are named by raw pointer by the draws in flight: parked, never freed.
        ~MeshECSSystem() override
        {
            RetireGenericDraws();
            if ( m_IsmScene )
                RetireRenderCache( m_IsmScene ); // the ISM rows a view in flight may still replay
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
               STATIC MESHES: the retained static scene (RecordStaticScene), only its changes rebuilt
               ========================= */
            // Frame-constant: the selection resolved ONCE into a per-entity flag (selected entities and their
            // subtrees), read in O(1) by the retained static rebuild (BuildStaticPrimitive).
            m_Outline.Refresh( registry );
            RecordStaticScene( registry, renderCommandBuffer, materialsVersion, drawnAsPieces );

            /* =========================
               INSTANCED STATIC MESHES (UE-style ISM: one entity = N instances, one instanced draw)
               ========================= */
            {
                // THE RETAINED ISM LIST (CPU-TAILS F10): rows staged in walk order, one snapshot published only
                // when a row changed, one command per update; the frame's values go to a pooled array.
                Graphic::Render::InstancedSceneFrame& ismFrame   = AcquireInstancedFrame();
                size_t                                ismRows    = 0;
                bool                                  ismChanged = false;
                auto view = registry.view<InstancedStaticMeshComponent>();
                view.each(
                     [&]( entt::entity entity, InstancedStaticMeshComponent& ism )
                     {
                         if ( ECS::IsHidden( registry, entity ) )
                             return;
                         if ( ism.GetInstanceTransforms().empty() )
                             return;

                         // Resolve the single shared mesh (edited RuntimeMesh > primitive > asset handle).
                         Desert::Mesh* targetMesh = nullptr;
                         if ( ism.RuntimeMesh() )
                             targetMesh = ism.RuntimeMesh().get();
                         else if ( ism.GetPrimitive().has_value() )
                             targetMesh = Geometry::PrimitiveMeshFactory::GetShared( ism.GetPrimitive().value() );
                         else if ( ism.GetMeshHandle() )
                             targetMesh = Runtime::ResourceRegistry::GetMeshService()->Get( ism.GetMeshHandle() );
                         if ( !targetMesh )
                             return;

                         // Null slots take the mesh asset's section materials, the static and skinned twins'
                         // one rule (UE: a foliage instance draws its mesh's materials unless overridden). The
                         // painted and procedural foliage spawners set only MeshHandle, so without this every
                         // field drew the default white surface, and a resimulate (which rebuilds the fields)
                         // dropped any slots copied in by hand. An edited RuntimeMesh or a primitive has no
                         // mesh asset to adopt from.
                         if ( !ism.RuntimeMesh() && !ism.GetPrimitive().has_value() )
                             AdoptComponentMaterialSlots( registry, entity, ism, &ECS::SetInstancedStaticMesh );

                         // Invalidation stamp (see the static path) — rebuild on any Invalidate().
                         if ( ism.SeenMaterialsVersion() != materialsVersion )
                         {
                             ism.RuntimeMaterialInstances().clear();
                             ism.SeenMaterialsVersion() = materialsVersion;
                         }

                         // One material instance per slot, rebuilt only when the slot set changes.
                         const size_t slotCount = ism.GetMaterialSlots().empty() ? 1 : ism.GetMaterialSlots().size();
                         if ( ism.RuntimeMaterialInstances().size() != slotCount )
                         {
                             ism.RuntimeMaterialInstances().clear();
                             ism.RuntimeDisplacedInstances().clear();
                             // A displaced mesh's twins draw with the displaced siblings (base cells).
                             if ( targetMesh->IsDisplaced() )
                                 for ( const auto& h : ism.GetMaterialSlots() )
                                 {
                                     auto inst = Runtime::ResourceRegistry::GetMaterialService()
                                                      ->CreateRuntimeInstance(
                                                           h, Graphic::MeshVertexPath::Static, true );
                                     ism.RuntimeDisplacedInstances().push_back(
                                          inst ? std::move( inst )
                                               : DefaultInstance( Graphic::MeshVertexPath::Static ) );
                                 }
                             if ( ism.GetMaterialSlots().empty() )
                                 ism.RuntimeMaterialInstances().push_back(
                                      DefaultInstance( Graphic::MeshVertexPath::Static ) );
                             else
                                 for ( const auto& h : ism.GetMaterialSlots() )
                                 {
                                     auto inst = Runtime::ResourceRegistry::GetMaterialService()
                                                      ->CreateRuntimeInstance( h );
                                     ism.RuntimeMaterialInstances().push_back(
                                          inst ? std::move( inst )
                                               : DefaultInstance( Graphic::MeshVertexPath::Static ) );
                                 }
                         }
                         if ( ism.RuntimeMaterialInstances().empty() )
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
                              ism.RuntimeMaterialInstances().size(), /*hidden*/ 0ull, ism.GetCastShadows(),
                              [&]( size_t slot ) -> Graphic::Material*
                              { return CustomSlotMaterial( ism.RuntimeMaterialInstances()[slot].get() ); } );
                         if ( !ismSplit.SlotDraws.empty() )
                         {
                             // An atomic: this tick runs on a worker (CanRunParallel), and every scene's
                             // MeshECSSystem shares the function's static.
                             static std::atomic<bool> s_WarnedCustomISM{ false };
                             if ( !s_WarnedCustomISM.exchange( true ) )
                             {
                                 LOG_WARN( "Instanced Static Mesh doesn't support custom-shader materials "
                                           "(instancing is a lit-path optimization) — the sections using one "
                                           "are not drawn. Assign a lit material." );
                             }
                         }
                         if ( ismSplit.SectionDraws.empty() )
                             return;

                         // InstanceTransforms are WORLD-space (the entity is a container). The command
                         // carries a CO-OWNED snapshot, not the component's address: see the member's own
                         // note and Graphic::MaterialSlotBinding (A8-3). The component's instance revision
                         // is what makes the snapshot impossible to leave stale: it moves on every byte
                         // change at the one writer, so a steady frame is one integer compare, not a memcmp
                         // of every instance (UE: the ISM proxy follows the component's instance change).
                         if ( !ism.RuntimeInstanceSnapshot() ||
                              ism.RuntimeInstanceSnapshotRevision() != ism.GetInstanceRevision() )
                         {
                             ism.RuntimeInstanceSnapshot() =
                                  std::make_shared<const std::vector<glm::mat4>>( ism.GetInstanceTransforms() );
                             ism.RuntimeInstanceSnapshotRevision() = ism.GetInstanceRevision();
                         }

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
                         // Each section group is a row of the retained ISM list (StageInstancedRow: compared by
                         // handle identity, assigned only on a change) and an entry of this frame's values.
                         const auto fade =
                              Graphic::ClipToMaxDrawDistance( cullDistance, ism.GetMaxDrawDistance() );
                         const auto& displaced = ism.RuntimeDisplacedInstances();
                         for ( const auto& d : ismSplit.SectionDraws )
                         {
                             const Graphic::MaterialInstancePtr& displacedSlot =
                                  d.Slot < displaced.size() ? displaced[d.Slot] : m_NoInstance;
                             const bool changed = StageInstancedRow(
                                  ismRows, static_cast<uint32_t>( entity ), targetMesh,
                                  ism.RuntimeMaterialInstances()[d.Slot], displacedSlot, d.HiddenMask,
                                  ism.RuntimeInstanceSnapshot(), d.CastShadows, ism.GetCastContactShadow() );
                             ismChanged = ismChanged || changed;
                             ++ismRows;
                             ismFrame.Rows.push_back( Graphic::Render::InstancedSceneRowFrame{ fade, wind } );
                         }
                     } );
                PublishInstancedScene( ismRows, ismChanged, renderCommandBuffer );
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
                         Desert::Mesh* baseMesh = mesh.GetRuntimeMesh().get();
                         if ( !baseMesh )
                             baseMesh = Runtime::ResourceRegistry::GetMeshService()->Get( mesh.GetMeshHandle() );
                         if ( !baseMesh || !baseMesh->IsSkinned() )
                             return;
                         auto* skinnedMesh = static_cast<Desert::SkinnedMesh*>( baseMesh );

                         // THE COOKED SKINNED MESH NAMES ITS MATERIALS AS THE STATIC ONE DOES (the .skmesh
                         // submeshes' MaterialGuid, SkinnedMeshAsset::GetMaterialHandles): an entity with no slot
                         // of its own takes them, by the same rule. Without it every placed skinned mesh drew the
                         // grey default although its .demat and textures were written (THM1l, live on Fox.glb). A
                         // runtime rig (Convert to Skinned) carries its own slots.
                         if ( !mesh.GetRuntimeMesh() )
                             AdoptComponentMaterialSlots( registry, entity, mesh, &ECS::SetSkinnedMesh );

                         // One skinned lit material instance (default if no slot assigned), rebuilt only when
                         // the slot set changes.
                         // Invalidation stamp (see the static path) — rebuild on any Invalidate().
                         if ( mesh.SeenMaterialsVersion() != materialsVersion )
                         {
                             mesh.RuntimeMaterialInstances().clear();
                             mesh.RuntimeDisplacedInstances().clear();
                             mesh.SeenMaterialsVersion() = materialsVersion;
                         }
                         const size_t slotCount = mesh.GetMaterialSlots().empty() ? 1 : mesh.GetMaterialSlots().size();
                         if ( mesh.RuntimeMaterialInstances().size() != slotCount )
                         {
                             mesh.RuntimeMaterialInstances().clear();
                             if ( mesh.GetMaterialSlots().empty() )
                                 mesh.RuntimeMaterialInstances().push_back(
                                      DefaultInstance( Graphic::MeshVertexPath::Skinned ) );
                             else
                                 for ( const auto& h : mesh.GetMaterialSlots() )
                                 {
                                     // THE SKINNED path of the SAME `.demat` the static twin uses. This
                                     // argument is the whole fix: the slot names a surface, this system
                                     // knows the geometry is skinned, and the pair resolves. Without it
                                     // the service answered with a static material and MeshRenderer
                                     // dropped the mesh without drawing anything.
                                     auto inst =
                                          Runtime::ResourceRegistry::GetMaterialService()->CreateRuntimeInstance(
                                               h, Graphic::MeshVertexPath::Skinned );
                                     mesh.RuntimeMaterialInstances().push_back(
                                          inst ? std::move( inst )
                                               : DefaultInstance( Graphic::MeshVertexPath::Skinned ) );
                                 }
                         }
                         if ( mesh.RuntimeMaterialInstances().empty() )
                             return;

                         // Built per frame here, as the raw vector it replaces was — the skinned path has
                         // no cached binding because it has no cached slot view to cache it beside. What
                         // changed is that the command now CO-OWNS the instances instead of copying bare
                         // pointers to instances this component alone keeps alive (A8-3).
                         auto slots   = std::make_shared<Graphic::MaterialSlotBinding>();
                         slots->Owned = mesh.RuntimeMaterialInstances();
                         slots->Slots.reserve( slots->Owned.size() );
                         for ( const auto& inst : slots->Owned )
                             slots->Slots.push_back( inst.get() );
                         // A displaced skinned mesh (Runtime, CONTRACT-DISPLACE §9.8) draws its twin with the
                         // displaced siblings of the same slots: base cells, so the relief is not marched again
                         // over the triangles that carry it (the static rule above).
                         if ( skinnedMesh->IsDisplaced() && !mesh.GetMaterialSlots().empty() )
                         {
                             if ( mesh.RuntimeDisplacedInstances().size() != mesh.GetMaterialSlots().size() )
                             {
                                 mesh.RuntimeDisplacedInstances().clear();
                                 for ( const auto& h : mesh.GetMaterialSlots() )
                                 {
                                     auto inst =
                                          Runtime::ResourceRegistry::GetMaterialService()->CreateRuntimeInstance(
                                               h, Graphic::MeshVertexPath::Skinned, true );
                                     mesh.RuntimeDisplacedInstances().push_back(
                                          inst ? std::move( inst )
                                               : DefaultInstance( Graphic::MeshVertexPath::Skinned ) );
                                 }
                             }
                             auto displaced   = std::make_shared<Graphic::MaterialSlotBinding>();
                             displaced->Owned = mesh.RuntimeDisplacedInstances();
                             for ( const auto& inst : displaced->Owned )
                                 displaced->Slots.push_back( inst.get() );
                             slots->Displaced = std::move( displaced );
                         }
                         else
                             mesh.RuntimeDisplacedInstances().clear();

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

                         // The entity itself only (a skinned mesh under a selected root stays unoutlined,
                         // as before): the subtree flag would widen it.
                         const bool isSelected =
                              registry.has<UUIDComponent>( entity ) &&
                              Runtime::SelectionContext::Contains( registry.get<UUIDComponent>( entity ).UUID );

                         // WORLD transform (the cached one) — a skinned mesh parented to e.g. the character
                         // controller must follow it; using the local transform left it behind at the origin
                         // while the (parent-aware) camera moved away. Mirrors the static path.
                         const glm::mat4& worldTransform = transform.GetWorldTransform();

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
                              std::move( morphWeights ), morphRevision, isSelected, mesh.GetCastShadows() );
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
            const Graphic::DataDrivenMaterial* surface = parent != nullptr ? parent->AsDataDriven() : nullptr;
            if ( surface == nullptr || surface->GetVertexPath() )
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

        // A component's slots go through its one writer (SetStaticMesh / SetSkinnedMesh / SetInstancedStaticMesh):
        // the adopted set is computed on a copy and written only when it differs; the writer drops the runtime
        // instances on a slot change, so the slot loop rebuilds them.
        template <typename Component, typename Desc>
        static void AdoptComponentMaterialSlots( entt::registry& registry, entt::entity entity, const Component& component,
                                                 bool ( *write )( entt::registry&, entt::entity, const Desc& ) )
        {
            if ( !HasUnadoptedSlot( component.GetMaterialSlots() ) )
                return;
            Desc                                      desc = component.GetDesc();
            std::vector<Graphic::MaterialInstancePtr> unused;
            AdoptMeshMaterialSlots( desc.MaterialSlots, unused, component.GetMeshHandle() );
            if ( desc.MaterialSlots != component.GetMaterialSlots() )
                write( registry, entity, desc );
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

        // ── THE RETAINED STATIC SCENE (CPU-SCENE-DELTA; UE FScene: AddPrimitive once, then changes) ──
        //
        // Every StaticMeshComponent entity owns a PERSISTENT PRIMITIVE SLOT (a dense index, recycled through a
        // free
        // list) holding its lit sections exactly as the immediate path used to record them each update. A slot is
        // rebuilt only when the entity is in the scene's render-state delta (ECS::RenderStateDelta: its transform,
        // an ancestor's, its mesh / material / visibility component, an attach or detach), when it is unresolved
        // (a pending mesh, an unadopted slot, an upload not yet made: it polls), or when something no component
        // signal reports changed for everyone (a material invalidation, a dropped mesh: a full rebuild; the
        // editor selection, the fractured set, an in-place mesh edit: the entities concerned). The slots reach
        // the views in immutable chunks (Render::StaticSceneChunk, kStaticSceneChunkSlots slots each): a change
        // rebuilds only the chunks it touched, and an update with no change records the previous snapshot again.
        struct StaticSlotDraw
        {
            Graphic::Material* Mat         = nullptr;
            uint64_t           VisibleMask = 0;
            bool               CastShadows = false;
        };
        struct StaticPrimitive
        {
            entt::entity  Entity          = entt::null;
            uint32_t      Rebuilt         = 0; // m_StaticUpdate of its last rebuild (the delta repeats entities)
            bool          Generic         = false; // a MaterialComponent shader: the generic path, every update
            bool          Outlined        = false;
            bool          CastShadows     = true;
            float         MaxDrawDistance = 0.0f;
            Desert::Mesh* Mesh            = nullptr;
            glm::mat4     World           = glm::mat4( 1.0f );
            Graphic::MaterialSlotBindingPtr                  Slots;
            std::vector<Graphic::Render::StaticSceneSection> Sections;
            std::vector<StaticSlotDraw>                      SlotDraws;
            // A Generic slot's draw values (UE: a proxy's cached mesh draw command, rebuilt on a material change),
            // built by BuildStaticPrimitive when GenericRevision differs from the MaterialComponent's revision and
            // only named by the per-update record. Kept across a rebuild (ClearStaticPrimitive leaves it), so a
            // moved mesh keeps it; dropped through RetireGenericDraw, since the draws in flight name it.
            std::shared_ptr<const Graphic::Render::GenericMeshDrawParams> GenericDraw;
            uint64_t                                                      GenericRevision = 0;
        };

        static void RetireGenericDraw( StaticPrimitive& primitive )
        {
            if ( !primitive.GenericDraw )
                return;
            RetireRenderCache( primitive.GenericDraw );
            primitive.GenericDraw.reset();
            primitive.GenericRevision = 0;
        }

        void RetireGenericDraws()
        {
            for ( StaticPrimitive& primitive : m_StaticPrimitives )
                RetireGenericDraw( primitive );
        }

        static void SetSlotMembership( std::vector<uint32_t>& list, const uint32_t slot, const bool member )
        {
            const auto at = std::find( list.begin(), list.end(), slot );
            if ( member && at == list.end() )
                list.push_back( slot );
            else if ( !member && at != list.end() )
            {
                *at = list.back();
                list.pop_back();
            }
        }

        static bool SameSections( const std::vector<Graphic::Render::StaticSceneSection>& a,
                                  const std::vector<Graphic::Render::StaticSceneSection>& b )
        {
            if ( a.size() != b.size() )
                return false;
            for ( size_t i = 0; i < a.size(); ++i )
            {
                const auto& x = a[i];
                const auto& y = b[i];
                if ( x.Mesh != y.Mesh || x.Transform != y.Transform || x.BoundsRevision != y.BoundsRevision ||
                     x.HiddenSubmeshes != y.HiddenSubmeshes || x.SectionSlot != y.SectionSlot ||
                     x.ForcedLOD != y.ForcedLOD || x.LODBias != y.LODBias ||
                     x.TranslucencySortPriority != y.TranslucencySortPriority ||
                     x.MaxDrawDistance != y.MaxDrawDistance || x.Outlined != y.Outlined ||
                     x.CastShadows != y.CastShadows || x.ReceiveShadows != y.ReceiveShadows ||
                     x.CastContactShadow != y.CastContactShadow )
                    return false;
            }
            return true;
        }

        void MarkStaticChunk( const uint32_t slot )
        {
            const size_t chunk = slot / Graphic::Render::kStaticSceneChunkSlots;
            if ( m_StaticChunkDirty.size() <= chunk )
                m_StaticChunkDirty.resize( chunk + 1, 1 );
            m_StaticChunkDirty[chunk] = 1;
            m_StaticChanged           = true;
        }

        static void ClearStaticPrimitive( StaticPrimitive& primitive )
        {
            primitive.Generic  = false;
            primitive.Outlined = false;
            primitive.Mesh     = nullptr;
            primitive.Slots.reset();
            primitive.Sections.clear();
            primitive.SlotDraws.clear();
        }

        void RemoveStaticPrimitive( const uint32_t slot )
        {
            StaticPrimitive& primitive = m_StaticPrimitives[slot];
            ClearStaticPrimitive( primitive );
            RetireGenericDraw( primitive );
            primitive.Entity = entt::null;
            SetSlotMembership( m_StaticPolls, slot, false );
            SetSlotMembership( m_StaticImmediate, slot, false );
            m_StaticFree.push_back( slot );
            MarkStaticChunk( slot );
        }

        // One entity of the delta: its slot rebuilt from the components as they now stand, or freed when the
        // entity no longer draws a static mesh.
        void RebuildStaticPrimitive( entt::registry& registry, const entt::entity entity,
                                     const uint32_t                          materialsVersion,
                                     const std::unordered_set<entt::entity>& drawnAsPieces )
        {
            const bool  alive     = entity != entt::null && registry.valid( entity );
            auto*       mesh      = alive ? registry.try_get<StaticMeshComponent>( entity ) : nullptr;
            const auto* transform = alive ? registry.try_get<TransformComponent>( entity ) : nullptr;
            const auto  found     = m_StaticSlotOf.find( entity );
            if ( mesh == nullptr || transform == nullptr )
            {
                if ( found != m_StaticSlotOf.end() )
                {
                    RemoveStaticPrimitive( found->second );
                    m_StaticSlotOf.erase( found );
                }
                return;
            }
            uint32_t slot = 0;
            if ( found != m_StaticSlotOf.end() )
                slot = found->second;
            else
            {
                if ( !m_StaticFree.empty() )
                {
                    slot = m_StaticFree.back();
                    m_StaticFree.pop_back();
                }
                else
                {
                    slot = static_cast<uint32_t>( m_StaticPrimitives.size() );
                    m_StaticPrimitives.emplace_back();
                }
                m_StaticSlotOf.emplace( entity, slot );
            }
            StaticPrimitive& primitive = m_StaticPrimitives[slot];
            if ( primitive.Rebuilt == m_StaticUpdate && primitive.Entity == entity )
                return;
            primitive.Rebuilt = m_StaticUpdate;
            primitive.Entity  = entity;
            // The previous sections are kept aside: a rebuild that comes out the same (a polled slot still
            // waiting)
            // touches no chunk, so an unresolved entity does not republish the scene every update.
            m_StaticPrevious.swap( primitive.Sections );
            const Graphic::MaterialSlotBindingPtr previousSlots = std::move( primitive.Slots );
            ClearStaticPrimitive( primitive );
            const bool polls = BuildStaticPrimitive( registry, entity, *mesh, *transform, materialsVersion,
                                                     drawnAsPieces, primitive );
            if ( !primitive.Generic )
                RetireGenericDraw( primitive );
            if ( primitive.Slots != previousSlots || !SameSections( m_StaticPrevious, primitive.Sections ) )
                MarkStaticChunk( slot );
            m_StaticPrevious.clear();
            SetSlotMembership( m_StaticPolls, slot, polls );
            SetSlotMembership( m_StaticImmediate, slot, primitive.Generic || !primitive.SlotDraws.empty() );
        }

        // The chunk's sections from its slots, into a chunk no snapshot names (a spare whose last holder is gone,
        // or a new one).
        void RebuildStaticChunk( const size_t index )
        {
            std::shared_ptr<Graphic::Render::StaticSceneChunk> chunk;
            for ( size_t i = 0; i < m_StaticSpareChunks.size(); ++i )
                if ( m_StaticSpareChunks[i].use_count() == 1 )
                {
                    chunk                  = std::move( m_StaticSpareChunks[i] );
                    m_StaticSpareChunks[i] = std::move( m_StaticSpareChunks.back() );
                    m_StaticSpareChunks.pop_back();
                    break;
                }
            if ( !chunk )
                chunk = std::make_shared<Graphic::Render::StaticSceneChunk>();
            chunk->Sections.clear();
            chunk->Owners.clear();
            const size_t first = index * Graphic::Render::kStaticSceneChunkSlots;
            const size_t last =
                 std::min<size_t>( first + Graphic::Render::kStaticSceneChunkSlots, m_StaticPrimitives.size() );
            for ( size_t slot = first; slot < last; ++slot )
            {
                const StaticPrimitive& primitive = m_StaticPrimitives[slot];
                if ( primitive.Sections.empty() )
                    continue;
                const auto owner = static_cast<uint32_t>( chunk->Owners.size() );
                chunk->Owners.push_back( primitive.Slots );
                for ( const Graphic::Render::StaticSceneSection& section : primitive.Sections )
                {
                    chunk->Sections.push_back( section );
                    chunk->Sections.back().Owner     = owner;
                    chunk->Sections.back().Primitive = static_cast<uint32_t>( slot );
                }
            }
            if ( m_StaticChunks[index] )
                m_StaticSpareChunks.push_back( std::move( m_StaticChunks[index] ) );
            m_StaticChunks[index] = std::move( chunk );
        }

        // A new snapshot of the chunks (a spare no view holds any more, or a new one); the previous one becomes a
        // spare once the views that replay it are done.
        void PublishStaticScene( const size_t rebuiltChunks )
        {
            std::shared_ptr<Graphic::Render::StaticSceneSnapshot> snapshot;
            for ( auto& spare : m_StaticSpareSnapshots )
                if ( spare.use_count() == 1 )
                {
                    spare->Chunks.clear(); // a free snapshot must not keep its chunks from being reused
                    if ( !snapshot )
                        snapshot = spare;
                }
            std::erase_if( m_StaticSpareSnapshots, [&]( const auto& spare )
                           { return spare == snapshot || ( spare.use_count() == 1 && spare->Chunks.empty() ); } );
            if ( !snapshot )
                snapshot = std::make_shared<Graphic::Render::StaticSceneSnapshot>();
            snapshot->Chunks.assign( m_StaticChunks.begin(), m_StaticChunks.end() );
            snapshot->Revision           = ++m_StaticRevision;
            snapshot->PreparedStaleEpoch = ~0ull; // a new version: its meshes are prepared afresh once
            if ( m_StaticScene )
                m_StaticSpareSnapshots.push_back( std::move( m_StaticScene ) );
            m_StaticScene = std::move( snapshot );
            // Free spare chunks beyond what this change needed are dropped (in-flight ones stay until free).
            size_t freeKept = 0;
            std::erase_if( m_StaticSpareChunks, [&]( const auto& spare )
                           { return spare.use_count() == 1 && freeKept++ >= rebuiltChunks; } );
        }

        // THE RETAINED ISM LIST (CPU-TAILS F10; UE: an ISM proxy's cached mesh draw commands). The row at
        // @p index takes this section group's values; returns whether it changed. Compared by handle identity:
        // the component's material instances are rebuilt (new handles) on a slot, mesh or invalidation change, and
        // its transform snapshot is new on every instance revision (the one writer, SetInstancedStaticMesh), so
        // an equal row is the same draw and nothing is copied.
        bool StageInstancedRow( const size_t index, const uint32_t entity, Desert::Mesh* const mesh,
                                const Graphic::MaterialInstancePtr& material,
                                const Graphic::MaterialInstancePtr& displaced, const uint64_t hiddenSubmeshes,
                                const std::shared_ptr<const std::vector<glm::mat4>>& transforms,
                                const bool castShadows, const bool castContactShadow )
        {
            if ( index == m_IsmRows.size() )
                m_IsmRows.emplace_back();
            Graphic::Render::InstancedSceneRow& row = m_IsmRows[index];
            if ( row.Entity == entity && row.Mesh == mesh && row.Material == material &&
                 row.DisplacedMaterial == displaced && row.HiddenSubmeshes == hiddenSubmeshes &&
                 row.Transforms == transforms && row.CastShadows == castShadows &&
                 row.CastContactShadow == castContactShadow )
                return false;
            row.Entity            = entity;
            row.Mesh              = mesh;
            row.Material          = material;
            row.DisplacedMaterial = displaced;
            row.HiddenSubmeshes   = hiddenSubmeshes;
            row.Transforms        = transforms;
            row.CastShadows       = castShadows;
            row.CastContactShadow = castContactShadow;
            return true;
        }

        // This update's frame values: a pooled array no recorded command holds any more (or a new one, only
        // while every pooled one is in flight), emptied with its capacity kept.
        Graphic::Render::InstancedSceneFrame& AcquireInstancedFrame()
        {
            size_t free = m_IsmFrames.size();
            for ( size_t i = 0; i < m_IsmFrames.size(); ++i )
                if ( m_IsmFrames[i].use_count() == 1 )
                {
                    free = i;
                    break;
                }
            if ( free == m_IsmFrames.size() )
                m_IsmFrames.push_back( std::make_shared<Graphic::Render::InstancedSceneFrame>() );
            m_IsmFrameIndex = free;
            m_IsmFrames[free]->Rows.clear();
            return *m_IsmFrames[free];
        }

        // The rows past @p rows left the list; any change publishes a new snapshot (the replaced one is parked
        // through RetireRenderCache: a view may still replay it). One command names the snapshot and the frame.
        void PublishInstancedScene( const size_t rows, bool changed,
                                    Graphic::Render::RenderCommandBuffer& renderCommandBuffer )
        {
            if ( rows != m_IsmRows.size() )
            {
                m_IsmRows.resize( rows );
                changed = true;
            }
            if ( changed || !m_IsmScene )
            {
                auto snapshot      = std::make_shared<Graphic::Render::InstancedSceneSnapshot>();
                snapshot->Rows     = m_IsmRows;
                snapshot->Revision = ++m_IsmRevision;
                if ( m_IsmScene )
                    RetireRenderCache( m_IsmScene );
                m_IsmScene = std::move( snapshot );
            }
            if ( !m_IsmScene->Rows.empty() )
                renderCommandBuffer.Emplace<Graphic::Render::DrawInstancedSceneCommand>(
                     m_IsmScene, m_IsmFrames[m_IsmFrameIndex] );
        }

        // THE STATIC PATH, per update: the delta applied to the slots, the touched chunks rebuilt, the snapshot
        // recorded once, and the few entities that draw off the batched path (a slot of a custom-shader material,
        // a MaterialComponent shader) recorded from their slot's cached state.
        void RecordStaticScene( entt::registry&                         registry,
                                Graphic::Render::RenderCommandBuffer&   renderCommandBuffer,
                                const uint32_t                          materialsVersion,
                                const std::unordered_set<entt::entity>& drawnAsPieces )
        {
            ++m_StaticUpdate;
            bool full        = m_StaticRegistry != &registry;
            m_StaticRegistry = &registry;
            m_StaticDirty.clear();
            const auto* link = registry.try_ctx<RenderStateDeltaLink>();
            if ( link != nullptr && link->Delta != nullptr )
                full = link->Delta->Drain( m_StaticDirty ) || full;
            else
                full = true; // no feed: nothing tells this cache what changed, so it trusts none of it
            // A dropped or rebuilt mesh, a material invalidation (the instances are rebuilt): no component signal
            // carries them, and each may concern any entity.
            const uint64_t meshRevision = Runtime::ResourceRegistry::GetMeshService()->GetBuiltRevision();
            if ( meshRevision != m_StaticMeshRevision || materialsVersion != m_StaticMaterialsVersion )
                full = true;
            m_StaticMeshRevision     = meshRevision;
            m_StaticMaterialsVersion = materialsVersion;
            // The outlines: only the entities whose selection flag moved since the last update (the old and the
            // new selection's subtrees, SelectionOutlineMask::Changed), never a full rebuild on an editor click.
            for ( const entt::entity entity : m_Outline.Changed() )
                m_StaticDirty.push_back( entity );
            // The fractured set: an entity entering or leaving it stops, or starts, drawing its own mesh.
            for ( const entt::entity entity : drawnAsPieces )
                if ( !std::binary_search( m_StaticPieces.begin(), m_StaticPieces.end(), entity ) )
                    m_StaticDirty.push_back( entity );
            for ( const entt::entity entity : m_StaticPieces )
                if ( !drawnAsPieces.contains( entity ) )
                    m_StaticDirty.push_back( entity );
            m_StaticPieces.assign( drawnAsPieces.begin(), drawnAsPieces.end() );
            std::sort( m_StaticPieces.begin(), m_StaticPieces.end() );

            if ( full )
            {
                RetireGenericDraws();
                m_StaticPrimitives.clear();
                m_StaticFree.clear();
                m_StaticSlotOf.clear();
                m_StaticPolls.clear();
                m_StaticImmediate.clear();
                m_StaticChunkDirty.clear();
                m_StaticChanged    = true;
                m_StaticStaleEpoch = Desert::Mesh::GetStaleEpoch(); // read before the rebuild reads the meshes
                registry.view<StaticMeshComponent, TransformComponent>().each(
                     [&]( const entt::entity entity, const StaticMeshComponent&, const TransformComponent& )
                     { RebuildStaticPrimitive( registry, entity, materialsVersion, drawnAsPieces ); } );
            }
            else
            {
                for ( const uint32_t slot : m_StaticPolls )
                    m_StaticDirty.push_back( m_StaticPrimitives[slot].Entity );
                for ( const entt::entity entity : m_StaticDirty )
                    RebuildStaticPrimitive( registry, entity, materialsVersion, drawnAsPieces );
                // AN IN-PLACE MESH EDIT (DynamicMesh::Update, an editor tool) raises no component signal: when any
                // mesh was marked stale since the last scan (Mesh::GetStaleEpoch), the slots whose mesh is pending
                // or was re-uploaded under their cached bounds are rebuilt (a pending one polls until its upload).
                // A still scene skips the scan in O(1); after the dirty rebuilds, so no freed slot is read.
                const uint64_t staleEpoch = Desert::Mesh::GetStaleEpoch();
                if ( staleEpoch != m_StaticStaleEpoch )
                {
                    m_StaticStaleEpoch = staleEpoch;
                    for ( const StaticPrimitive& primitive : m_StaticPrimitives )
                        if ( primitive.Mesh != nullptr && !primitive.Sections.empty() &&
                             ( primitive.Mesh->IsGpuStale() ||
                               primitive.Sections.front().BoundsRevision != primitive.Mesh->GetRenderRevision() ) )
                            m_StaticEdited.push_back( primitive.Entity );
                    for ( const entt::entity entity : m_StaticEdited )
                        RebuildStaticPrimitive( registry, entity, materialsVersion, drawnAsPieces );
                    m_StaticEdited.clear();
                }
            }

            if ( m_StaticChanged )
            {
                const size_t chunks = ( m_StaticPrimitives.size() + Graphic::Render::kStaticSceneChunkSlots - 1 ) /
                                      Graphic::Render::kStaticSceneChunkSlots;
                m_StaticChunkDirty.resize( chunks, 1 );
                if ( full )
                    std::fill( m_StaticChunkDirty.begin(), m_StaticChunkDirty.end(), uint8_t{ 1 } );
                m_StaticChunks.resize( chunks );
                size_t rebuilt = 0;
                for ( size_t chunk = 0; chunk < chunks; ++chunk )
                    if ( m_StaticChunkDirty[chunk] != 0 )
                    {
                        RebuildStaticChunk( chunk );
                        m_StaticChunkDirty[chunk] = 0;
                        ++rebuilt;
                    }
                PublishStaticScene( rebuilt );
                m_StaticChanged = false;
            }

            if ( m_StaticScene && !m_StaticScene->Chunks.empty() )
                renderCommandBuffer.Emplace<Graphic::Render::DrawStaticSceneCommand>( m_StaticScene );

            for ( const uint32_t slot : m_StaticImmediate )
            {
                const StaticPrimitive& primitive = m_StaticPrimitives[slot];
                if ( primitive.Generic )
                {
                    RecordStaticGeneric( renderCommandBuffer, primitive );
                    continue;
                }
                for ( const StaticSlotDraw& d : primitive.SlotDraws )
                    renderCommandBuffer.Emplace<Graphic::Render::DrawSlotMaterialMeshCommand>(
                         static_cast<uint32_t>( primitive.Entity ), primitive.Mesh, primitive.World, d.Mat,
                         d.VisibleMask, primitive.Outlined, d.CastShadows, primitive.MaxDrawDistance );
            }
        }

        // A MaterialComponent assigning a NON-Lit shader takes the mesh off the batched Lit path onto the generic
        // per-object data-driven path. Its shader and overrides are the slot's GenericDraw, built per material
        // revision (BuildGenericDraw): a steady update copies no string and no vector into the command.
        static void RecordStaticGeneric( Graphic::Render::RenderCommandBuffer& renderCommandBuffer,
                                         const StaticPrimitive&                primitive )
        {
            if ( !primitive.GenericDraw )
                return;
            // This draw REPLACES the entity's lit draw, so it is the only draw that could carry the caster — and
            // before it did, a Shader Override mesh cast no shadow at all.
            renderCommandBuffer.Emplace<Graphic::Render::DrawGenericMeshCommand>(
                 static_cast<uint32_t>( primitive.Entity ), primitive.Mesh, primitive.World,
                 /*shaderName: GenericDraw's*/ std::string{}, *primitive.GenericDraw, primitive.Outlined,
                 /*directTexture*/ nullptr, /*directTextureSampler*/ std::string{}, primitive.CastShadows,
                 primitive.MaxDrawDistance, /*visibleInRayTracing*/ true );
        }

        // The slot's draw values from the component as it stands, when its revision moved (or none is built).
        static void BuildGenericDraw( const MaterialComponent& matc, StaticPrimitive& primitive )
        {
            if ( primitive.GenericDraw && primitive.GenericRevision == matc.GetRevision() )
                return;
            auto draw        = std::make_shared<Graphic::Render::GenericMeshDrawParams>();
            draw->ShaderName = matc.GetShaderName();
            std::vector<std::pair<std::string, glm::vec4>> overrides;
            overrides.reserve( matc.GetParams().size() );
            for ( const auto& p : matc.GetParams() )
                overrides.emplace_back( p.Name, p.Value );

            std::vector<std::pair<std::string, uint64_t>> texOverrides;
            texOverrides.reserve( matc.GetTextures().size() );
            for ( const auto& t : matc.GetTextures() )
                texOverrides.emplace_back( t.Name, t.TextureHandle );
            draw->Overrides = Graphic::MaterialOverrides{ std::move( overrides ), std::move( texOverrides ) };
            RetireGenericDraw( primitive );
            primitive.GenericDraw     = std::move( draw );
            primitive.GenericRevision = matc.GetRevision();
        }

        // One entity's sections from its components (the immediate path's per-entity body, run only when the
        // entity changed). Returns whether the slot must be rebuilt again next update (unresolved state).
        bool BuildStaticPrimitive( entt::registry& registry, const entt::entity entity, StaticMeshComponent& mesh,
                                   const TransformComponent& transform, const uint32_t materialsVersion,
                                   const std::unordered_set<entt::entity>& drawnAsPieces,
                                   StaticPrimitive&                        primitive )
        {
            // Hidden entities (Visible toggle) are skipped.
            if ( ECS::IsHidden( registry, entity ) )
                return false;

            if ( !mesh.GetRuntimeMesh() && !mesh.GetPrimitive().has_value() && !mesh.GetMeshHandle() )
                return false;

            // Its pieces stand in for it (FracturePieceDraw).
            if ( drawnAsPieces.contains( entity ) )
                return false;

            Desert::Mesh* targetMesh = nullptr;
            if ( mesh.GetRuntimeMesh() )
            {
                // Use unique modified mesh
                targetMesh = mesh.GetRuntimeMesh().get();
            }
            else if ( mesh.GetPrimitive().has_value() )
            {
                // Use the process-wide SHARED primitive mesh (one Mesh* per type) so identical primitives batch
                // via
                // instancing. NOT stored in RuntimeMesh — leaving it null keeps the entity on the shared mesh;
                // the mesh editor forks a per-entity RuntimeMesh on edit (copy-on-edit), which then takes
                // priority.
                targetMesh = Geometry::PrimitiveMeshFactory::GetShared( mesh.GetPrimitive().value() );
            }
            else if ( mesh.GetMeshHandle() )
            {
                // Asset-based mesh
                targetMesh = Runtime::ResourceRegistry::GetMeshService()->Get( mesh.GetMeshHandle() );
            }

            // A pending mesh (MeshService::Get answers nullptr until it lands): asked again next update.
            if ( !targetMesh )
                return true;

            // --- Auto-Initialize Material Slots --- (the one rule, AdoptMeshMaterialSlots)
            AdoptComponentMaterialSlots( registry, entity, mesh, &ECS::SetStaticMesh );

            // A MaterialService::Invalidate() dropped some runtime Material — rebuild every cached instance set
            // (parents may be graveyarded). Without it a stale RuntimeMaterialInstances would keep a dangling
            // GetParentMaterial() pointer past the next CollectGarbage().
            if ( mesh.SeenMaterialsVersion() != materialsVersion )
            {
                mesh.RuntimeMaterialInstances().clear();
                mesh.SeenMaterialsVersion() = materialsVersion;
            }

            // Ensure runtime material instances are initialized and match the slots
            size_t slotCount = mesh.GetMaterialSlots().empty() ? 1 : mesh.GetMaterialSlots().size();

            if ( mesh.RuntimeMaterialInstances().size() != slotCount )
            {
                mesh.RuntimeMaterialInstances().clear();
                mesh.RuntimeMaterialInstances().reserve( slotCount );

                if ( mesh.GetMaterialSlots().empty() )
                {
                    // Use persistent system default material template
                    mesh.RuntimeMaterialInstances().push_back(
                         DefaultInstance( Graphic::MeshVertexPath::Static ) );
                }
                else
                {
                    for ( const auto& assetHandle : mesh.GetMaterialSlots() )
                    {
                        // Service-owned resolution: base assets give a plain instance, material-INSTANCE assets
                        // give an instance of their base with the child overrides applied.
                        auto inst =
                             Runtime::ResourceRegistry::GetMaterialService()->CreateRuntimeInstance( assetHandle );
                        mesh.RuntimeMaterialInstances().push_back(
                             inst ? std::move( inst ) : DefaultInstance( Graphic::MeshVertexPath::Static ) );
                    }
                }

                // Rebuild the render path's binding ONLY here (when the instance set changes): what the snapshot
                // then holds is a shared_ptr COPY of it, which keeps both the slot array and every instance in it
                // alive for as long as any draw is still in flight. A fresh object rather than a mutation of the
                // old one: a snapshot published earlier may still be holding the previous binding.
                auto binding   = std::make_shared<Graphic::MaterialSlotBinding>();
                binding->Owned = mesh.RuntimeMaterialInstances();
                binding->Slots.reserve( binding->Owned.size() );
                for ( const auto& inst : binding->Owned )
                    binding->Slots.push_back( inst.get() );
                // A displaced mesh (Runtime or Baked, its import settings) draws its displaced geometry with the
                // displaced siblings of the same slots: base cells, so the relief is not marched a second time
                // over the triangles that carry it.
                if ( targetMesh->IsDisplaced() && !mesh.GetMaterialSlots().empty() )
                {
                    auto displaced = std::make_shared<Graphic::MaterialSlotBinding>();
                    for ( const auto& assetHandle : mesh.GetMaterialSlots() )
                    {
                        auto inst = Runtime::ResourceRegistry::GetMaterialService()->CreateRuntimeInstance(
                             assetHandle, Graphic::MeshVertexPath::Static, true );
                        displaced->Owned.push_back( inst ? std::move( inst )
                                                         : DefaultInstance( Graphic::MeshVertexPath::Static ) );
                    }
                    if ( displaced->Owned[0] )
                        for ( const auto& p : mesh.GetScriptSlotParams() )
                            displaced->Owned[0]->SetParamFromVec4( p.Name, p.Value );
                    for ( const auto& inst : displaced->Owned )
                        displaced->Slots.push_back( inst.get() );
                    binding->Displaced = std::move( displaced );
                }
                mesh.RuntimeSlots() = std::move( binding );

                // The script's slot-0 overrides land on EVERY slot-0 instance built (UE: the MID a mesh component
                // keeps across material changes). The list is the script's, not a hand-off buffer: nothing is
                // consumed, so a rebuild after a material invalidation shows the same values the script last
                // wrote.
                if ( !mesh.RuntimeMaterialInstances().empty() && mesh.RuntimeMaterialInstances()[0] )
                    for ( const auto& p : mesh.GetScriptSlotParams() )
                        mesh.RuntimeMaterialInstances()[0]->SetParamFromVec4( p.Name, p.Value );
            }

            // The world transform: the cached one (TransformComponent::GetWorldTransform, kept current by every
            // hierarchy write), a load rather than a parent walk with a TRS recompose per ancestor.
            const glm::mat4& worldTransform = transform.GetWorldTransform();

            // Outline: editor selection (this entity or any ancestor — selecting a prefab root outlines all its
            // mesh children) OR the per-entity "Draw outline" toggle. A selection change rebuilds the slots whose
            // flag moved (m_Outline.Changed); the flag is m_Outline's, refreshed once per update before
            // RecordStaticScene (O(1) here).
            const bool outlined = m_Outline.Contains( entity ) || mesh.GetOutlineDraw();

            primitive.Mesh            = targetMesh;
            primitive.World           = worldTransform;
            primitive.Outlined        = outlined;
            primitive.CastShadows     = mesh.GetCastShadows();
            primitive.MaxDrawDistance = mesh.GetMaxDrawDistance();

            // A MaterialComponent assigning a NON-Lit shader takes this mesh off the batched Lit path onto the
            // generic per-object data-driven path (RecordStaticGeneric), and replaces its lit draws.
            if ( const auto* matc = registry.try_get<MaterialComponent>( entity );
                 matc != nullptr && !matc->GetShaderName().empty() )
            {
                primitive.Generic = true;
                BuildGenericDraw( *matc, primitive );
                return targetMesh->IsGpuStale();
            }

            // ── Per-section draws (UE: each mesh section draws with its own material slot) ──
            // Section i takes slot SectionMaterialSlot( i, slots ). A section whose slot material is a
            // custom-shader material (DataDrivenMaterial without a mesh cell) leaves the batched passes for a
            // per-slot generic
            // draw; every other section is drawn by the batched passes with ITS slot, one draw per slot
            // (Rules::SplitMeshSlotDraws, the rule fracture pieces split by too). Each draw masks out every
            // submesh
            // it does not draw.
            const size_t submeshCount = std::min<size_t>( targetMesh->GetSubmeshes().size(), 64 );
            const auto   split        = Rules::SplitMeshSlotDraws<Graphic::Material*>(
                 submeshCount, mesh.RuntimeMaterialInstances().size(), mesh.GetHiddenSubmeshes(),
                 mesh.GetCastShadows(), [&]( size_t slot ) -> Graphic::Material*
                 { return CustomSlotMaterial( mesh.RuntimeMaterialInstances()[slot].get() ); } );

            for ( const auto& d : split.SlotDraws )
                primitive.SlotDraws.push_back(
                     { .Mat = d.Mat, .VisibleMask = d.VisibleMask, .CastShadows = d.CastShadows } );

            primitive.Slots                   = mesh.RuntimeSlots();
            const Common::Math::AABB local    = Geometry::LocalBounds( targetMesh->GetSubmeshes() );
            const uint64_t           revision = targetMesh->GetRenderRevision();
            for ( const auto& d : split.SectionDraws )
                primitive.Sections.push_back( { .Mesh                     = targetMesh,
                                                .Transform                = worldTransform,
                                                .LocalBounds              = local,
                                                .BoundsRevision           = revision,
                                                .HiddenSubmeshes          = d.HiddenMask,
                                                .Entity                   = static_cast<uint32_t>( entity ),
                                                .SectionSlot              = static_cast<uint32_t>( d.Slot ),
                                                .Owner                    = 0,
                                                .ForcedLOD                = mesh.GetForcedLOD(),
                                                .LODBias                  = mesh.GetLODBias(),
                                                .TranslucencySortPriority = mesh.GetTranslucencySortPriority(),
                                                .MaxDrawDistance          = mesh.GetMaxDrawDistance(),
                                                .Outlined                 = outlined,
                                                .CastShadows              = d.CastShadows,
                                                .ReceiveShadows           = mesh.GetReceiveShadows(),
                                                .CastContactShadow        = mesh.GetCastContactShadow() } );

            // An upload not made yet (a new or edited mesh: its revision and bounds change at the fence), or a
            // slot
            // still waiting for its section material: taken again next update.
            return targetMesh->IsGpuStale() || HasUnadoptedSlot( mesh.GetMaterialSlots() );
        }

        SelectionOutlineMask                         m_Outline; // the selection as a per-entity flag, per frame
        std::shared_ptr<Graphic::DataDrivenMaterial> m_DefaultMaterial;
        std::shared_ptr<Graphic::DataDrivenMaterial> m_DefaultSkinnedMaterial;
        bool                                         m_DefaultRefusalLogged = false;
        // Gameplay seconds since the scene's systems started (FO-7). Double: a float clock loses the sway's
        // sub-frame steps after a few hours of play; MakeInstanceWind wraps it to the sway period.
        double m_WindSeconds = 0.0;
        double m_PrevWindSeconds = 0.0; // the clock one gameplay step ago: the instances' velocity

        // The retained static scene's state (RecordStaticScene).
        const entt::registry*                                              m_StaticRegistry         = nullptr;
        uint32_t                                                           m_StaticUpdate           = 0;
        uint64_t                                                           m_StaticRevision         = 0;
        uint64_t                                                           m_StaticMeshRevision     = 0;
        uint32_t                                                           m_StaticMaterialsVersion = 0;
        bool                                                               m_StaticChanged          = false;
        std::vector<StaticPrimitive>                                       m_StaticPrimitives; // by slot
        std::vector<uint32_t>                                              m_StaticFree;       // freed slots
        std::unordered_map<entt::entity, uint32_t>                         m_StaticSlotOf;     // entity -> slot
        std::vector<uint32_t>                                              m_StaticPolls;      // unresolved slots
        std::vector<uint32_t>                                              m_StaticImmediate;  // off-batch slots
        std::vector<uint8_t>                                               m_StaticChunkDirty; // by chunk
        std::vector<std::shared_ptr<Graphic::Render::StaticSceneChunk>>    m_StaticChunks;     // published
        std::vector<std::shared_ptr<Graphic::Render::StaticSceneChunk>>    m_StaticSpareChunks;
        std::shared_ptr<Graphic::Render::StaticSceneSnapshot>              m_StaticScene;
        std::vector<std::shared_ptr<Graphic::Render::StaticSceneSnapshot>> m_StaticSpareSnapshots;
        std::vector<entt::entity>                                          m_StaticDirty;  // this update's delta
        std::vector<entt::entity>                                          m_StaticPieces; // last fractured set
        std::vector<entt::entity> m_StaticEdited;             // the in-place edit scan's slots (scratch)
        uint64_t                  m_StaticStaleEpoch = ~0ull; // Mesh::GetStaleEpoch as of the last scan
        std::vector<Graphic::Render::StaticSceneSection>                   m_StaticPrevious; // scratch

        // The retained ISM list's state (StageInstancedRow / PublishInstancedScene, CPU-TAILS F10).
        std::vector<Graphic::Render::InstancedSceneRow>                    m_IsmRows;  // staged, walk order
        std::shared_ptr<Graphic::Render::InstancedSceneSnapshot>           m_IsmScene; // published
        uint64_t                                                           m_IsmRevision = 0;
        std::vector<std::shared_ptr<Graphic::Render::InstancedSceneFrame>> m_IsmFrames; // pooled frame values
        size_t                                                             m_IsmFrameIndex = 0;
        const Graphic::MaterialInstancePtr m_NoInstance; // a section with no displaced sibling names this
    };
} // namespace Desert::ECS