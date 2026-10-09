#pragma once

#include <entt/entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <filesystem>
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
#include <Engine/Animation/Timeline/Binding.hpp>
#include <Engine/Animation/Timeline/Player.hpp>
#include <Engine/Animation/Timeline/Sequence.hpp>

#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>

#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/UIArgs.hpp>

// Components big enough to own a file. They live in Desert::ECS like everything below, and are included
// here so that "the components" remains one include for every consumer.
#include <Engine/ECS/ExponentialHeightFogComponent.hpp>
#include <Engine/ECS/HeroCloudComponent.hpp>
#include <Engine/ECS/ProceduralFoliageComponent.hpp>
#include <Engine/ECS/DestructibleComponent.hpp>
#include <Engine/ECS/DestructionFieldComponents.hpp>
#include <Engine/ECS/PostProcessVolumeComponent.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/ECS/SkyAtmosphereComponent.hpp>
#include <Engine/World/Landscape/LandscapeEditLayers.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>

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

    struct VisibilityComponent
    {
        bool Visible = true;
    };

    struct StaticMeshComponent
    {
        Assets::AssetHandle              MeshHandle;
        std::vector<Assets::AssetHandle> MaterialSlots;
        std::vector<Graphic::MaterialInstancePtr>
             RuntimeMaterialInstances; // Cache to keep instances alive and avoid per-frame allocations
        // The render path's CO-OWNED handle on those instances, rebuilt with them and never separately.
        // It replaced a `std::vector<MaterialInstance*> RuntimeSlotPtrs` whose ADDRESS the draw commands
        // took: entt moves components when the pool changes and Lua runs between recording a draw and
        // executing it, so both the vector and the instances in it could be gone by the time the renderer
        // read them. See Graphic::MaterialSlotBinding for the full account (A8-3).
        Graphic::MaterialSlotBindingPtr        RuntimeSlots;
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
        int                                    ForcedLOD   = -1; // -1 = auto (by distance); 0..N pins a LOD
        int  LODBias        = 0;    // shifts the AUTO-picked LOD (+coarser, -finer); ignored when ForcedLOD >= 0
        bool CastShadows    = true; // false = skipped by the shadow (depth) passes
        bool ReceiveShadows = true; // false = sun shadows are not applied to this mesh (forward path)
        // Translucency pass order override (UE's TranslucencySortPriority): a LOWER value draws first, i.e.
        // behind a higher one whatever their distances; within one value the pass sorts back to front
        // (Graphic::System::TranslucentSortOrder). Only meaningful for a Translucent-blend material.
        int TranslucencySortPriority = 0;
        // Per-submesh visibility: bit i set = submesh i is HIDDEN (skipped at draw). 0 = all visible. Up to
        // 64 submeshes; edited per Element in the Materials panel.
        uint64_t HiddenSubmeshes = 0;
        // Transient: MaterialService invalidation stamp the runtime instances were built against;
        // a mismatch forces a rebuild (see MeshECSSystem) so parent Material* can never dangle.
        uint32_t SeenMaterialsVersion = 0;
    };

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

    struct SkinnedMeshComponent
    {
        Assets::AssetHandle              MeshHandle;
        std::vector<Assets::AssetHandle> MaterialSlots;
        std::vector<Graphic::MaterialInstancePtr>
                 RuntimeMaterialInstances; // Cache to keep instances alive and avoid per-frame allocations
        uint32_t SeenMaterialsVersion = 0; // see StaticMeshComponent
        bool     CastShadows          = true; // false = skipped by the shadow (depth) passes, like the static twin

        // In-editor rig: a skinned mesh built at runtime by "Convert to Skinned" (from a static mesh + placed
        // bones, auto-weighted), NOT yet a cooked asset. When set, RuntimeMesh overrides MeshHandle in the
        // render path. RuntimeSkeleton owns the Skeleton that RuntimeMesh references by raw pointer, keeping it
        // alive for the mesh's lifetime (destroyed together with the component).
        std::shared_ptr<SkinnedMesh>         RuntimeMesh;
        std::shared_ptr<Animation::Skeleton> RuntimeSkeleton;
    };

    // UE-style Instanced Static Mesh: ONE mesh + ONE material drawn N times (per-instance world transforms)
    // as a single instanced draw call. The per-instance transforms live in a GPU storage buffer the
    // instanced shader reads by gl_InstanceIndex, so 1000 instances cost ~1 draw + 1 material setup instead
    // of N. Use for repeated static props / NPCs / buildings in the city.
    struct InstancedStaticMeshComponent
    {
        Assets::AssetHandle              MeshHandle;
        std::vector<Assets::AssetHandle> MaterialSlots;
        std::vector<glm::mat4>           InstanceTransforms; // per-instance world matrices

        // false = skipped by the shadow (depth) passes, exactly like the static and skinned twins.
        // Until this existed an ISM was the ONE mesh kind whose shadow could not be turned off: the
        // cascade pass read StaticMeshComponent::CastShadows and SkinnedMeshComponent::CastShadows and
        // appended every ISM unconditionally. A UE-style foliage field of grass is the first thing a
        // scene wants to take out of the cascades, and it was the only thing that could not be.
        bool CastShadows = true;

        // Transient runtime state (not serialized): generated mesh for primitives, the material instance,
        // and a dirty flag so the renderer re-uploads the instance SSBO only when the transforms change.
        std::optional<Geometry::PrimitiveType>    Primitive;
        std::shared_ptr<DynamicMesh>              RuntimeMesh;
        std::vector<Graphic::MaterialInstancePtr> RuntimeMaterialInstances;
        uint32_t                                  SeenMaterialsVersion = 0; // see StaticMeshComponent

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
        // REBUILT BY COMPARING, NOT BY A FLAG — see the note above on the flag that used to sit beside
        // it. Comparing the snapshot with the authored vector cannot drift: an edit that changes the
        // contents rebuilds it, an unchanged frame costs one comparison and no allocation.
        std::shared_ptr<const std::vector<glm::mat4>> RuntimeInstanceSnapshot;
    };

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
    // entities of this scene by UUID. Saved as {SequenceGuid, SequencePath, Loop, AutoPlay, BindingOverrides}
    // (ComponentRegistry.cpp); a sequence the project does not have refuses the load with both.
    struct LevelSequenceComponent
    {
        Assets::AssetHandle                       Sequence;
        Animation::Timeline::LoopMode             Loop     = Animation::Timeline::LoopMode::Once;
        bool                                      AutoPlay = true;
        std::vector<LevelSequenceBindingOverride> BindingOverrides;
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

    // One overridden material parameter (keyed by the shader's #pragma param name). vec4 stores any
    // scalar/vector value (float uses .x, color uses rgba). Data-driven: NOT a fixed C++ field per param.
    struct MaterialParamOverride
    {
        std::string Name;
        glm::vec4   Value = glm::vec4( 0.0f );
    };

    // One overridden texture param (keyed by the shader's #pragma param texture2D name) -> texture asset.
    struct MaterialTextureOverride
    {
        std::string Name;
        uint64_t    TextureHandle = 0; // Assets::AssetHandle as uint64 (0 = unset -> shader fallback)
    };

    // Assigns an arbitrary shader (by program name) to whatever renderer draws this entity, with its
    // parameters edited generically in Details (built from the shader's #pragma param schema). The
    // renderer builds a DataDrivenMaterial from ShaderName and applies these overrides.
    // ShaderName is the ShaderService COMPILE KEY of a template that is NOT `Role StandardSurface`, resolved from
    // the template's handle by whoever sets it (scene load, a role lookup); empty = no override, the mesh
    // draws its lit material slots and Params are only the slot-0 hand-off buffer. No decision compares it
    // to a template's name.
    struct MaterialComponent
    {
        std::string                          ShaderName;
        std::vector<MaterialParamOverride>   Params;   // scalar/vector overrides (on top of #pragma defaults)
        std::vector<MaterialTextureOverride> Textures; // texture2D overrides (unset -> backend fallback)
    };

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

        // NO ROOT-MOTION FLAG. `bool EnableRootMotion` sat here, was written to every scene and prefab and
        // drawn as a checkbox in Details, and NOTHING in the engine ever read it: there was no root-delta
        // extraction in Animator, in AnimationECSSystem or in LocomotionSystem. A knob that cannot move a
        // pixel is a TODO wearing a feature's clothes (contract §1.3), and the honest removal is cheaper
        // than a half-implementation. Reopening it is a feature with a design question attached — whether
        // the delta drives the TransformComponent or the character controller, and what a crossfade between
        // two clips with different root motion means — and that question belongs with the locomotion work,
        // not with a checkbox nobody wired.

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

    // Data-driven state -> clip mapping for LocomotionSystem, so the SYSTEM holds NO clip knowledge (no clip
    // names or instances baked in). The system maps planar speed / on-ground to one of these clip NAMES and
    // hands it to AnimationComponent.CurrentClip; the clips themselves come from the AnimationLibrary (imported
    // assets or the procedural humanoid's built-in clips). LocomotionSystem falls back to a default-constructed
    // instance of THIS struct when the component is absent, so the defaults live in data, not in the system.
    struct LocomotionComponent
    {
        std::string IdleClip  = "Idle";
        std::string WalkClip  = "Walk";
        std::string RunClip   = "Run";
        std::string JumpClip  = "Jump";
        float       WalkSpeed = 0.2f; // planar speed above which -> walk
        float       RunSpeed  = 6.5f; // planar speed above which -> run
    };

    // Blendshape / morph-target weights for the entity's mesh. Weights[k] (0..1, though over/undershoot is
    // allowed) scales morph target k of the mesh asset; the CPU blend is base + Σ(weight·delta) (see
    // Geometry::ApplyMorphTargets). TargetNames mirrors the mesh's target names for the Details UI and stays
    // index-aligned with Weights. Non-reflected (like AnimationComponent) — edited via the Morph widget.
    struct MorphComponent
    {
        std::vector<float>       Weights;
        std::vector<std::string> TargetNames;

        // Last weights the runtime blended into the entity's geometry — lets a per-frame apply skip work
        // when nothing changed. Transient (not serialized).
        std::vector<float> AppliedWeights;
    };

    struct TransformComponent
    {
        glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Rotation    = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Scale       = { 1.0f, 1.0f, 1.0f };

        glm::mat4 GetTransform() const
        {
            return glm::translate( glm::mat4( 1.0f ), Translation ) * glm::toMat4( glm::quat( Rotation ) ) *
                   glm::scale( glm::mat4( 1.0f ), Scale );
        }
    };

    // ON THE TWO PAIRS OF SUN NUMBERS. Colour x Intensity here is the ILLUMINANCE arriving at scene
    // surfaces — what every lit surface integrates. SkyAtmosphereData::SunColor x SunIntensity is the
    // RADIANCE of the sky and of the solar disk — what the camera sees when it looks up. Two different
    // quantities with different consumers, not one value stored twice: neither is derived from the other,
    // and no code path reads one where it means the other. See SkyAtmosphereComponent.hpp.
    struct DirectionalLightData
    {
        REFLECT()

        PROPERTY( DisplayName( "Color" ), Category( "Light" ), Color, Temperature,
                  Tooltip( "Tint of the illumination arriving at scene surfaces. The sun you SEE in the sky "
                           "is the Sky Atmosphere component's Sun Color / Sun Intensity." ) )
        glm::vec3 Color = glm::vec3( 1.0f );

        PROPERTY( DisplayName( "Intensity" ), Category( "Light" ), Range( 0.0f, 10.0f ), Summary, Units( "x" ),
                  Tooltip( "Brightness of the illumination arriving at scene surfaces. NOT a photometric "
                           "unit (lux/candela): the renderer multiplies radiance by this number directly. "
                           "The sun you SEE in the sky is the Sky Atmosphere component's Sun Color / Sun "
                           "Intensity." ) )
        float Intensity = 1.0f;

        // Defaults to true so that every scene authored before this field existed keeps working: a field
        // missing from the file keeps its C++ default, so the one directional light such a scene has
        // becomes its atmosphere sun with no migration.
        PROPERTY( DisplayName( "Atmosphere Sun Light" ), Category( "Atmosphere" ),
                  Tooltip( "This light drives the sky and the sky's IBL bake." ) )
        bool AtmosphereSunLight = true;

        PROPERTY( DisplayName( "Atmosphere Sun Light Index" ), Category( "Atmosphere" ), Range( 0, 0 ),
                  EditCondition( "AtmosphereSunLight" ),
                  Tooltip( "The engine renders exactly one directional light; index 1 is reserved for a "
                           "future second sun." ) )
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

        PROPERTY( DisplayName( "Shadow Bias" ), Category( "Cascaded Shadow Maps" ), Range( 0.0f, 0.05f ) )
        float ShadowBias = 0.005f;

        PROPERTY( DisplayName( "Cascade Split Lambda" ), Category( "Cascaded Shadow Maps" ), Range( 0.0f, 1.0f ),
                  Tooltip( "0 = uniform cascade splits, 1 = logarithmic; UE's Distribution Exponent plays "
                           "the same part." ) )
        float CascadeSplitLambda = 0.6f;
    };

    struct DirectionLightComponent
    {
        COMPONENT( Key( "DirectionLight" ), Block( Data ), Run( ActorsAndUI ) )
        DirectionalLightData Data;
    };

    // Distance falloff model for a point light. Reflected enum — drives a combo in the editor and
    // round-trips through reflected serialization. (Consumed by the lighting upload path later.)
    enum class LightFalloff
    {
        Linear,
        Quadratic,
        InverseSquare
    };

    struct PointLightData
    {
        REFLECT()

        PROPERTY( DisplayName( "Color" ), Category( "Light" ), Color, Temperature )
        glm::vec3 Color = glm::vec3( 1.0f );

        PROPERTY( DisplayName( "Intensity" ), Category( "Light" ), Range( 0.0f, 10.0f ), Summary, Units( "x" ),
                  Tooltip( "Linear multiplier on the light colour. NOT a photometric unit (lux/candela): "
                           "the renderer multiplies radiance by this number directly." ) )
        float Intensity = 1.0f;

        PROPERTY( DisplayName( "Radius" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length, Summary )
        float Radius = 1000.0f;

        // Inner radius where attenuation == 1 (a small emitter "source size"); falloff runs from here to
        // Radius. 0 = point source.
        PROPERTY( DisplayName( "Min Radius" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length, Advanced )
        float MinRadius = 0.0f;

        PROPERTY( DisplayName( "Falloff" ), Category( "Light" ), Advanced )
        LightFalloff Falloff = LightFalloff::Quadratic;

        PROPERTY( DisplayName( "Show Radius" ), Category( "Light" ) )
        bool ShowRadius = false;
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

        PROPERTY( DisplayName( "Intensity" ), Category( "Light" ), Range( 0.0f, 10.0f ), Summary, Units( "x" ),
                  Tooltip( "Linear multiplier on the light colour. NOT a photometric unit (lux/candela): "
                           "the renderer multiplies radiance by this number directly." ) )
        float Intensity = 1.0f;

        PROPERTY( DisplayName( "Range" ), Category( "Light" ), Range( 0.0f, 10000.0f ), Length, Summary )
        float Range = 1500.0f;

        PROPERTY( DisplayName( "Inner Cone" ), Category( "Light" ), Range( 0.0f, 89.0f ), Units( "deg" ) )
        float InnerConeAngle = 20.0f; // degrees — full intensity inside this half-angle

        PROPERTY( DisplayName( "Outer Cone" ), Category( "Light" ), Range( 0.0f, 90.0f ), Units( "deg" ), Summary )
        float OuterConeAngle = 30.0f; // degrees — zero intensity outside this half-angle

        PROPERTY( DisplayName( "Falloff" ), Category( "Light" ), Advanced )
        LightFalloff Falloff = LightFalloff::Quadratic;

        PROPERTY( DisplayName( "Show Cone" ), Category( "Light" ) )
        bool ShowCone = false;
    };

    struct SpotLightComponent
    {
        COMPONENT( Key( "SpotLight" ), Block( Data ), Run( ActorsAndUI ) )
        SpotLightData Data;
    };

    // How particle billboards composite into the scene. Additive = glowing FX (fire/sparks/magic);
    // AlphaBlend = soft opaque puffs (smoke/dust). Reflected enum -> editor combo + serialization.
    enum class ParticleBlendMode
    {
        Additive,
        AlphaBlend
    };

    // GPU-simulated billboard particle emitter. The reflected fields below are the AUTHORING parameters
    // (Details UI + scene serialization are generated from them); the actual simulation runs in a compute
    // shader and the quads are drawn camera-facing in the Transparency phase (ParticleRenderer). Emits from
    // the entity's transform. Runtime GPU buffers live on the render system, keyed by the entity — not here.
    struct ParticleEmitterData
    {
        REFLECT()

        PROPERTY( DisplayName( "Enabled" ), Category( "Emitter" ) )
        bool Enabled = true;

        PROPERTY( DisplayName( "Max Particles" ), Category( "Emitter" ), Range( 1.0f, 100000.0f ) )
        int MaxParticles = 2000;

        PROPERTY( DisplayName( "Spawn Rate" ), Category( "Emitter" ), Range( 0.0f, 10000.0f ), Units( "/s" ),
                  Summary )
        float SpawnRate = 200.0f; // particles per second

        PROPERTY( DisplayName( "Looping" ), Category( "Emitter" ) )
        bool Looping = true;

        PROPERTY( DisplayName( "Simulate In World" ), Category( "Emitter" ) )
        bool WorldSpace = true; // world = particles trail behind a moving emitter; local = ride with it

        PROPERTY( DisplayName( "Lifetime" ), Category( "Particle" ), Range( 0.01f, 60.0f ), Units( "s" ), Summary )
        float Lifetime = 3.0f; // seconds

        PROPERTY( DisplayName( "Lifetime Variance" ), Category( "Particle" ), Range( 0.0f, 1.0f ) )
        float LifetimeVariance = 0.2f;

        PROPERTY( DisplayName( "Start Speed" ), Category( "Motion" ), Range( 0.0f, 100.0f ) )
        float StartSpeed = 200.0f;

        PROPERTY( DisplayName( "Speed Variance" ), Category( "Motion" ), Range( 0.0f, 1.0f ) )
        float SpeedVariance = 0.3f;

        PROPERTY( DisplayName( "Emit Direction" ), Category( "Motion" ) )
        glm::vec3 Direction = glm::vec3( 0.0f, 1.0f, 0.0f ); // normalized emit axis

        PROPERTY( DisplayName( "Cone Angle" ), Category( "Motion" ), Range( 0.0f, 180.0f ), Units( "deg" ) )
        float ConeAngle = 45.0f; // degrees of spread around Direction (wide enough to read from any angle)

        PROPERTY( DisplayName( "Gravity" ), Category( "Motion" ) )
        glm::vec3 Gravity = glm::vec3( 0.0f, -200.0f, 0.0f );

        PROPERTY( DisplayName( "Start Size" ), Category( "Look" ), Range( 0.0f, 1000.0f ), Length )
        float StartSize = 25.0f;

        // Size-over-life ease: the compute shader raises the normalized age t to this power before lerping
        // Start->End size. 1 = linear; <1 = fast then slow (puffs); >1 = slow then fast (shrinking sparks).
        // Authored as a curve in the Particle Editor.
        PROPERTY( DisplayName( "Size Curve Power" ), Category( "Look" ), Range( 0.1f, 8.0f ) )
        float SizeCurvePower = 1.0f;

        PROPERTY( DisplayName( "End Size" ), Category( "Look" ), Range( 0.0f, 1000.0f ), Length )
        float EndSize = 6.0f; // keep a sliver of size so particles stay readable instead of vanishing mid-life

        PROPERTY( DisplayName( "Start Color" ), Category( "Look" ), Color )
        glm::vec3 StartColor = glm::vec3( 1.0f, 0.6f, 0.15f );

        PROPERTY( DisplayName( "End Color" ), Category( "Look" ), Color )
        glm::vec3 EndColor = glm::vec3( 0.6f, 0.1f, 0.0f );

        PROPERTY( DisplayName( "Start Alpha" ), Category( "Look" ), Range( 0.0f, 1.0f ) )
        float StartAlpha = 1.0f;

        PROPERTY( DisplayName( "End Alpha" ), Category( "Look" ), Range( 0.0f, 1.0f ) )
        float EndAlpha = 0.0f;

        // AlphaBlend (over) by default: it shows the particle colour against ANY background. Additive glow
        // washes out against bright/lit surfaces — looked down at a sunlit floor the fountain "disappeared"
        // even though it was drawn, while it popped against the dark sky from below. Fire/sparks presets in
        // the Particle Editor still switch this to Additive where the scene behind them is dark.
        PROPERTY( DisplayName( "Blend" ), Category( "Look" ) )
        ParticleBlendMode Blend = ParticleBlendMode::AlphaBlend;
    };

    struct ParticleEmitterComponent
    {
        COMPONENT( Key( "ParticleEmitter" ), Block( Data ), Run( ActorsAndUI ) )
        ParticleEmitterData Data;

        // One-shot "restart" from the editor's transport, consumed by ParticleRenderer::PrepareFrame:
        // it zeroes the emitter's particle state (every particle dead -> respawned from scratch) without
        // destroying the GPU buffer. Transient — not reflected, so it never reaches a scene file.
        bool RequestRestart = false;
    };

    // ============================================================
    // UI — Godot-Control-style screen-space UI (2D). The authored data lives in the framework
    // (Engine/UI/Args, namespace Desert::UI); the ECS stores each one in the component that wraps it.
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

        PROPERTY( DisplayName( "Clip" ), Category( "Audio" ), Summary )
        std::string Clip; // audio file (wav/mp3/flac), absolute or Assets-relative

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
    };

    // Marks an entity as a physics body. Static = immovable, Dynamic = simulated, Kinematic = code-driven.
    struct RigidBodyComponent
    {
        COMPONENT( Key( "RigidBody" ), Block( Data ), Run( ActorsAndUI ) )
        RigidBodyData Data;

        // Transient: the live Jolt body (created on Play, cleared on Stop). Not reflected/serialized.
        Physics::BodyHandle RuntimeBody = Physics::kInvalidBody;
    };

    // Playable character controller params (reflected -> Details UI + serialization). Drives a Jolt
    // CharacterVirtual capsule (walks slopes/steps, blocked by world geometry) via WASD + jump.
    struct CharacterControllerData
    {
        REFLECT()

        // PHYSICS / capsule only. The control FEEL (move/sprint/look/jump speed) lives in the controller
        // SCRIPT's Properties — not here — so there's a single source of truth for behavior. See ScriptComponent.
        PROPERTY( DisplayName( "Radius" ), Category( "Character" ), Range( 5.0f, 500.0f ), Length )
        float Radius = 30.0f;

        PROPERTY( DisplayName( "Height" ), Category( "Character" ), Range( 20.0f, 1000.0f ), Length )
        float Height = 180.0f; // total capsule height (HalfHeight = (Height - 2*Radius) / 2)

        PROPERTY( DisplayName( "Max Slope" ), Category( "Character" ), Range( 0.0f, 89.0f ), Units( "deg" ) )
        float MaxSlopeDeg = 50.0f;

        // Fall acceleration (m/s^2). Default ~2x real gravity so the jump arc feels SNAPPY (real 9.81 reads as
        // floaty). Authorable per-character instead of a baked engine constant — a moon level just lowers it.
        PROPERTY( DisplayName( "Gravity" ), Category( "Character" ), Range( 0.0f, 6000.0f ), Units( "cm/s2" ),
                  Advanced )
        float Gravity = 2000.0f;

        // The capsule's profile in Config/CollisionProfiles.json (UE: the capsule's Collision Presets, "Pawn").
        PROPERTY( DisplayName( "Collision Profile" ), Category( "Collision" ) )
        std::string CollisionProfile = "Pawn";
    };

    // A WASD-driven player. The follow camera is NOT here — parent a child entity with a CameraComponent
    // (offset behind = 3rd person, at the head = 1st person); it tracks the player via the hierarchy.
    struct CharacterControllerComponent
    {
        COMPONENT( Key( "CharacterController" ), Block( Data ), Run( ActorsAndUI ) )
        CharacterControllerData Data;

        // Transient (Play only): the live Jolt character + the integrated vertical velocity (gravity/jump).
        Physics::CharacterHandle RuntimeCharacter = Physics::kInvalidCharacter;
        float                    VerticalVelocity = 0.0f;
        float                    CurrentSpeed     = 0.0f; // planar move speed this frame (drives locomotion anim)

        // Move INTENT, set by the controller SCRIPT each frame (the engine only executes the physics). This is
        // the mechanism/behavior split: the script reads input + decides where to go; PhysicsECSSystem turns
        // this into a camera-relative velocity and steps Jolt.
        glm::vec2 MoveInput     = { 0.0f, 0.0f }; // x = strafe (right), y = forward; each -1..1
        float     DesiredSpeed  = 0.0f;           // m/s the script asked for (sprint etc. is script policy)
        bool      JumpRequested = false;          // set by script:jump(strength), consumed + cleared by physics
        float     JumpStrength  = 5.0f;           // launch velocity the script passed to self:jump()
        bool      OnGround      = false;          // last physics result, exposed to scripts (self:isOnGround())
        glm::vec2 AirVelocity   = { 0.0f, 0.0f }; // horizontal velocity locked at takeoff (no air control)

        // Swimming (set by the controller SCRIPT when it detects the body is below the water level). While
        // swimming, PhysicsECSSystem replaces gravity with buoyancy, gives full 3D control, and drives the
        // vertical from SwimVertical (+1 = up, -1 = down) instead of jump/gravity.
        bool  Swimming     = false;
        float SwimVertical = 0.0f; // -1..1 swim up/down intent (script)
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
    };

    struct StreamingSourceComponent
    {
        COMPONENT( Key( "StreamingSource" ), Block( Data ), Run( ActorsAndUI ) )
        StreamingSourceData Data;
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
        // can forget the resolution and reintroduce the defect above; PathForStableKey returns an
        // untagged string unchanged, so a key the tag table does not know still behaves exactly as the
        // old rooted spelling did rather than silently becoming something else.
        std::filesystem::path ResolvedPath() const
        {
            return Common::AssetHandle::PathForStableKey( ScriptKey );
        }

        // Editor-exposed properties (from the script's `Properties` table): per-entity values, edited in
        // Details and serialized. Written into the script env before it runs (so the script reads them).
        std::vector<Scripting::ScriptProperty> Properties;

        bool Started = false; // transient: OnStart already called for this instance
    };

    // One entity can run MANY scripts (EnTT allows only one component of a type per entity, so multiple
    // behaviors live as a LIST of slots inside this one component — the same composition UE gets from
    // multiple ActorComponents). Each slot is an independent sandbox (its own env + properties + lifecycle);
    // all slots share the same `self` entity. ScriptSystem ticks every slot; entity:call() broadcasts to all.
    struct ScriptComponent
    {
        std::vector<ScriptSlot> Scripts;
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
        std::string  BoneName; // bone/socket name on the target's skeleton (e.g. "mixamorig:RightHand")

        // Grip alignment relative to the bone (the weapon almost never sits exactly on the bone origin).
        glm::vec3 OffsetTranslation = { 0.0f, 0.0f, 0.0f };
        glm::vec3 OffsetRotation    = { 0.0f, 0.0f, 0.0f }; // euler radians
        glm::vec3 OffsetScale       = { 1.0f, 1.0f, 1.0f };
    };

    // A flying projectile (bullet/grenade/arrow): integrated each frame by ProjectileSystem (Play only). On a
    // swept hit it delivers "OnHit" to the struck entity's script and is destroyed; it also dies on lifetime.
    // Mechanism (movement + collision) is C++; the DECISION to fire + what a hit MEANS stay in Lua.
    struct ProjectileComponent
    {
        glm::vec3    Velocity      = { 0.0f, 0.0f, 0.0f }; // world units/s
        float        GravityScale  = 0.0f;                 // 0 = straight line, 1 = full gravity (arc)
        float        LifeRemaining = 5.0f;                 // seconds before auto-despawn
        float        Damage        = 10.0f;
        Common::UUID Owner         = Common::UUID::Null(); // shooter (so we can skip self-hits); null = unowned
    };
} // namespace Desert::ECS