#pragma once

#include <entt/entt.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/UUID.hpp>

#include <filesystem>

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
#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBR.hpp>

#include <Engine/Animation/Animator.hpp>

#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Scripting/ScriptProperty.hpp>

#include <Engine/Reflection/ReflectionMacros.hpp>

// Components big enough to own a file. They live in Desert::ECS like everything below, and are included
// here so that "the components" remains one include for every consumer.
#include <Engine/ECS/ExponentialHeightFogComponent.hpp>
#include <Engine/ECS/HeroCloudComponent.hpp>
#include <Engine/ECS/VolumetricCloudComponent.hpp>
#include <Engine/ECS/SkyAtmosphereComponent.hpp>
#include <Engine/World/Landscape/LandscapeLayout.hpp>

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
    // to its GPU representation. This is the general concept — a surface Material (PBRSurfaceParams) is
    // just ONE specialization; camera and lights are others. NOT a material, hence the member is `Data`.
    struct CameraData
    {
        REFLECT()

        PROPERTY( DisplayName( "Main Camera" ), Category( "Camera" ) )
        bool IsMainCamera = true;

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
        std::optional<Geometry::PrimitiveType> Primitive;   // Optional primitive type for dynamic generation
        std::shared_ptr<DynamicMesh>           RuntimeMesh; // Unique mesh instance for modifications
        bool                                   OutlineDraw = false;
        int                                    ForcedLOD   = -1; // -1 = auto (by distance); 0..N pins a LOD
        int  LODBias        = 0;    // shifts the AUTO-picked LOD (+coarser, -finer); ignored when ForcedLOD >= 0
        bool CastShadows    = true; // false = skipped by the shadow (depth) passes
        bool ReceiveShadows = true; // false = sun shadows are not applied to this mesh (forward path)
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

    // A FOLIAGE type (UE5-style). Sits alongside an InstancedStaticMeshComponent (the mesh + per-instance
    // WORLD transforms, drawn instanced). The Foliage paint tool scatters instances of this type onto surfaces
    // (raycast brush). These are the per-type scatter params.
    struct FoliageComponent
    {
        float Density       = 6.0f; // instances scattered per paint dab (in the brush disk)
        float ScaleMin      = 0.8f;
        float ScaleMax      = 1.3f;
        float ZOffsetMin    = 0.0f; // sink(-)/raise(+) along world up, randomized per instance
        float ZOffsetMax    = 0.0f;
        float MaxPitchDeg   = 0.0f; // random tilt off the up/normal axis (0 = upright)
        float SlopeMinDeg   = 0.0f; // only paint where the surface slope is within [min,max] degrees
        float SlopeMaxDeg   = 90.0f;
        bool  AlignToNormal = true; // tilt instances to the surface normal
        bool  RandomYaw     = true; // random rotation about the up axis
    };

    // Per-layer splat mode. Auto = weight from height/slope rules (in-shader); Manual = weight painted
    // into the terrain splat map (brush, Stage 3b); Off = layer disabled. Reflected -> combo in editor.
    enum class TerrainLayerMode
    {
        Auto,
        Manual,
        Off
    };

    // Procedural heightmap terrain params (reflected -> inspector + serialization).
    struct TerrainData
    {
        REFLECT()

        // The terrain's material, a `.demat` like every other material — the surface is drawn by ONE
        // program of domain Terrain, and its three splat layers (u_GrassTex/u_RockTex/u_SnowTex) are
        // TEXTURE PARAMETERS of that one program, blended in-shader by the layer modes below. So this is
        // one handle and not a slot vector: a vector would promise a material per layer, and nothing
        // downstream could consume one. Unset = the shader's own schema defaults.
        //
        // Read by Engine/ECS/System/TerrainECSSystem.hpp, which resolves it through
        // Runtime::MaterialService::ResolveData and forwards the values as named overrides.
        //
        // Hidden from the auto-built Details on the same terms as SkyboxData::SkyboxHandle below: the
        // builder's asset slot is texture-oriented, and the Terrain entry draws a material field with an
        // Edit button that opens the Material Editor window — after Stage 3 that window is the only place
        // a material is authored. Still serialized; Hidden is editor-only.
        PROPERTY( DisplayName( "Material" ), Category( "Terrain" ), Asset<MaterialAsset>, Hidden )
        Assets::AssetHandle Material;

        PROPERTY( DisplayName( "Size" ), Category( "Terrain" ), Range( 100.0f, 50000.0f ), Length )
        float Size = 5000.0f;

        PROPERTY( DisplayName( "Resolution" ), Category( "Terrain" ), Range( 2.0f, 256.0f ) )
        int Resolution = 64;

        PROPERTY( DisplayName( "Height Scale" ), Category( "Terrain" ), Range( 0.0f, 5000.0f ), Length )
        float HeightScale = 500.0f;

        PROPERTY( DisplayName( "Noise Frequency" ), Category( "Terrain" ), Range( 0.001f, 1.0f ) )
        float NoiseFrequency = 0.08f;

        PROPERTY( DisplayName( "Seed" ), Category( "Terrain" ), Range( 0.0f, 9999.0f ) )
        int Seed = 1337;

        PROPERTY( DisplayName( "Grass Layer" ), Category( "Terrain Layers" ) )
        TerrainLayerMode GrassMode = TerrainLayerMode::Auto;

        PROPERTY( DisplayName( "Rock Layer" ), Category( "Terrain Layers" ) )
        TerrainLayerMode RockMode = TerrainLayerMode::Auto;

        PROPERTY( DisplayName( "Snow Layer" ), Category( "Terrain Layers" ) )
        TerrainLayerMode SnowMode = TerrainLayerMode::Auto;
    };

    // TerrainECSSystem generates a grid mesh from Data into the entity's StaticMeshComponent.RuntimeMesh (so
    // the normal mesh render path draws it). Regenerated when any param changes (tracked via BuiltHash).
    struct TerrainComponent
    {
        TerrainData Data;
        // Transient: hash of the params the current RuntimeMesh was built from; a mismatch -> regenerate.
        std::size_t BuiltHash = 0;

        // --- Splat painting (Stage 3b, runtime only; not yet serialized) ---
        // RGBA8 splat map: R=grass, G=rock, B=snow weights. Manual-mode layers sample this. The CPU mirror
        // is the brush's edit target; SplatDirty triggers a safe GPU re-upload (ViewportPanel::OnPreUpdate).
        static constexpr uint32_t         SplatResolution = 256;
        std::shared_ptr<Graphic::Image2D> SplatMap;
        std::vector<unsigned char>        SplatPixels; // size = SplatResolution^2 * 4, lazily allocated
        bool                              SplatDirty = false;
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

        // NO ROOT-MOTION FLAG. `bool EnableRootMotion` sat here, was written to every scene and prefab and
        // drawn as a checkbox in Details, and NOTHING in the engine ever read it: there was no root-delta
        // extraction in Animator, in AnimationECSSystem or in LocomotionSystem. A knob that cannot move a
        // pixel is a TODO wearing a feature's clothes (contract §1.3), and the honest removal is cheaper
        // than a half-implementation. Reopening it is a feature with a design question attached — whether
        // the delta drives the TransformComponent or the character controller, and what a crossfade between
        // two clips with different root motion means — and that question belongs with the locomotion work,
        // not with a checkbox nobody wired.

        // Notify names fired by the Animator THIS frame (crossed clip markers). Filled by AnimationECSSystem,
        // drained + dispatched to the entity's scripts (OnAnimationNotify) by ScriptSystem. Transient.
        std::vector<std::string> PendingNotifies;

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
    // surfaces — what every PBR surface integrates. SkyAtmosphereData::SunColor x SunIntensity is the
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
    };

    struct DirectionLightComponent
    {
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
        ParticleEmitterData Data;

        // One-shot "restart" from the editor's transport, consumed by ParticleRenderer::PrepareFrame:
        // it zeroes the emitter's particle state (every particle dead -> respawned from scratch) without
        // destroying the GPU buffer. Transient — not reflected, so it never reaches a scene file.
        bool RequestRestart = false;
    };

    // ============================================================
    // UI — Godot-Control-style screen-space UI (2D)
    // ============================================================

    // Horizontal text alignment within a UI element's rect. Reflected enum -> editor combo + serialization.
    enum class UITextAlign
    {
        Left,
        Center,
        Right
    };

    // Vertical placement of the (possibly multi-line) text block within the element rect.
    enum class UITextVAlign
    {
        Top,
        Middle,
        Bottom
    };

    // What to do when the text is wider/taller than the element rect (mutually resolved in this order:
    // AutoSize shrinks first, then Wrap breaks lines, then Ellipsis truncates the overflow).
    enum class UITextOverflow
    {
        Overflow, // draw past the rect (legacy behaviour)
        Ellipsis, // truncate the overflowing tail with "…"
        Clip      // hard-clip to the rect (no ellipsis)
    };

    // How the canvas maps to the viewport (Unity CanvasScaler-style):
    //  Stretch        - canvas == the WHOLE viewport, 1:1 pixels. Layout is driven by anchors, so a full-screen
    //                   element (anchors 0,0-1,1) fills any resolution and nothing "zooms" when the window
    //                   resizes. The resolution-independent default.
    //  ScaleWithScreen- canvas == the whole viewport too, but the ENTIRE design is scaled from the reference
    //                   resolution (offsets + font sizes multiplied), so a layout authored at 1280x720 keeps
    //                   its proportions on any screen. MatchWidthHeight blends width- vs height-based scaling.
    //  Letterbox      - the reference resolution scaled to FIT inside the viewport and centred (black bars).
    //                   For fixed-aspect, pixel-perfect designs.
    enum class UICanvasScaleMode
    {
        Stretch,
        ScaleWithScreen,
        Letterbox
    };

    // Where the canvas lives. ScreenSpace = a flat overlay (menus/HUD). WorldSpace = billboarded at the
    // canvas entity's 3D position, projected to the screen + distance-scaled each frame (nameplate over an
    // NPC, a floating panel). WorldSpace needs the camera's view-proj (passed by the renderer's caller).
    enum class UICanvasRenderMode
    {
        ScreenSpace,
        WorldSpace
    };

    // Auto-layout container type. A UILayoutGroup on an element positions + sizes its DIRECT children
    // automatically (overriding their anchors) — the Unity/Godot "layout group" model.
    enum class UILayoutType
    {
        Vertical,   // VBox: children top -> bottom
        Horizontal, // HBox: children left -> right
        Grid        // fixed cells, wrapping into rows
    };

    // Add to an element to auto-arrange its children. Children keep their UILayout for appearance + preferred
    // size (from CustomMinimumSize, else the offset size), but their POSITION/size comes from the group.
    struct UILayoutGroupData
    {
        REFLECT()

        PROPERTY( DisplayName( "Type" ), Category( "UI Layout Group" ) )
        UILayoutType Type = UILayoutType::Vertical;

        PROPERTY( DisplayName( "Padding L/T/R/B" ), Category( "UI Layout Group" ) )
        glm::vec4 Padding = glm::vec4( 8.0f );

        PROPERTY( DisplayName( "Spacing" ), Category( "UI Layout Group" ), Range( 0.0f, 128.0f ) )
        float Spacing = 6.0f;

        PROPERTY( DisplayName( "Stretch Children (cross axis)" ), Category( "UI Layout Group" ) )
        bool StretchCross = true;

        PROPERTY( DisplayName( "Grid Cell Size" ), Category( "UI Layout Group" ) )
        glm::vec2 CellSize = glm::vec2( 100.0f, 100.0f );

        PROPERTY( DisplayName( "Grid Columns (0 = auto)" ), Category( "UI Layout Group" ), Range( 0.0f, 64.0f ) )
        int Columns = 0;
    };
    struct UILayoutGroupComponent
    {
        UILayoutGroupData Data;
    };

    // A horizontal progress/health bar: a background track with a fill spanning Value (0..1) of the width.
    // Display-only (no interaction).
    struct UIProgressBarData
    {
        REFLECT()

        PROPERTY( DisplayName( "Value" ), Category( "UI Progress Bar" ), Range( 0.0f, 1.0f ) )
        float Value = 0.5f;

        PROPERTY( DisplayName( "Background" ), Category( "UI Progress Bar" ), Color )
        glm::vec3 Background = glm::vec3( 0.12f, 0.13f, 0.16f );

        PROPERTY( DisplayName( "Fill" ), Category( "UI Progress Bar" ), Color )
        glm::vec3 Fill = glm::vec3( 0.30f, 0.65f, 0.35f );

        PROPERTY( DisplayName( "Corner Radius" ), Category( "UI Progress Bar" ), Range( 0.0f, 32.0f ) )
        float CornerRadius = 4.0f;
    };
    struct UIProgressBarComponent
    {
        UIProgressBarData Data;
    };

    // A checkbox: a box that fills with the check colour when on. A click (runtime) flips Value.
    struct UIToggleData
    {
        REFLECT()

        PROPERTY( DisplayName( "Value (on)" ), Category( "UI Toggle" ) )
        bool Value = false;

        PROPERTY( DisplayName( "Box Color" ), Category( "UI Toggle" ), Color )
        glm::vec3 BoxColor = glm::vec3( 0.18f, 0.19f, 0.24f );

        PROPERTY( DisplayName( "Check Color" ), Category( "UI Toggle" ), Color )
        glm::vec3 CheckColor = glm::vec3( 0.30f, 0.60f, 0.90f );

        PROPERTY( DisplayName( "Corner Radius" ), Category( "UI Toggle" ), Range( 0.0f, 32.0f ) )
        float CornerRadius = 4.0f;
    };
    struct UIToggleComponent
    {
        UIToggleData Data;
    };

    // A horizontal slider: a track + a fill up to the handle + a draggable handle. Dragging (runtime) sets
    // Value in [MinValue, MaxValue].
    struct UISliderData
    {
        REFLECT()

        PROPERTY( DisplayName( "Value" ), Category( "UI Slider" ) )
        float Value = 0.5f;

        PROPERTY( DisplayName( "Min" ), Category( "UI Slider" ) )
        float MinValue = 0.0f;

        PROPERTY( DisplayName( "Max" ), Category( "UI Slider" ) )
        float MaxValue = 1.0f;

        PROPERTY( DisplayName( "Track Color" ), Category( "UI Slider" ), Color )
        glm::vec3 TrackColor = glm::vec3( 0.12f, 0.13f, 0.16f );

        PROPERTY( DisplayName( "Fill Color" ), Category( "UI Slider" ), Color )
        glm::vec3 FillColor = glm::vec3( 0.30f, 0.52f, 0.82f );

        PROPERTY( DisplayName( "Handle Color" ), Category( "UI Slider" ), Color )
        glm::vec3 HandleColor = glm::vec3( 0.90f, 0.92f, 0.96f );
    };
    struct UISliderComponent
    {
        UISliderData Data;
    };

    // A vertical scroll view: clips its children to its rect and scrolls them by the mouse wheel. Children are
    // positioned relative to the content top (offset up by ScrollY). ContentHeight is the total scrollable
    // height in design px (author-set); scrolling clamps to [0, ContentHeight - viewport height].
    struct UIScrollViewData
    {
        REFLECT()

        PROPERTY( DisplayName( "Scroll Y" ), Category( "UI Scroll View" ) )
        float ScrollY = 0.0f;

        PROPERTY( DisplayName( "Content Height" ), Category( "UI Scroll View" ) )
        float ContentHeight = 600.0f;

        PROPERTY( DisplayName( "Background" ), Category( "UI Scroll View" ), Color )
        glm::vec3 Background = glm::vec3( 0.10f, 0.11f, 0.14f );

        PROPERTY( DisplayName( "Show Scrollbar" ), Category( "UI Scroll View" ) )
        bool ShowScrollbar = true;

        PROPERTY( DisplayName( "Scrollbar Color" ), Category( "UI Scroll View" ), Color )
        glm::vec3 ScrollbarColor = glm::vec3( 0.35f, 0.37f, 0.44f );
    };
    struct UIScrollViewComponent
    {
        UIScrollViewData Data;
    };

    // A VERTICAL LIST WHOSE OFF-SCREEN ROWS DO NOT EXIST FOR THE FRAME (Ю17).
    //
    // WHY IT IS A SECOND CONTAINER AND NOT A FLAG ON UIScrollView. A scroll view holds ARBITRARY content:
    // its children keep their own anchors, it cannot know where any of them ends up without resolving all
    // of them, and resolving all of them is the O(n) it would have to avoid. This one owns its rows'
    // geometry the way UILayoutGroup owns its children's — every row is ItemHeight design px tall and
    // Spacing apart — which is exactly what turns "which rows are on screen" into two divisions. The two
    // are different contracts, not two modes of one, and a `Virtualize` bool on the scroll view would have
    // meant a container that positions its children on Tuesdays.
    //
    // ROW INDEX IS CHILD ORDER, AND VISIBILITY DOES NOT MOVE IT. A Hidden or Collapsed child leaves an
    // empty row rather than closing the gap, which is the one place this container disagrees with
    // UILayoutGroup. Closing the gap needs a running count over every child ahead of the window — the
    // whole-list pass this element exists to delete — so it would cost precisely what it saves. The row is
    // still skipped by the walk's own visibility axis, so the hole is visible and is not a silent draw.
    //
    // CONTENT HEIGHT IS DERIVED, NEVER AUTHORED. UIScrollViewData carries a ContentHeight the author has
    // to keep in step with what is actually in the list; here it is the child count times the pitch, so a
    // row added or removed cannot leave the scroll range lying about it.
    struct UIListViewData
    {
        REFLECT()

        PROPERTY( DisplayName( "Scroll Y" ), Category( "UI List View" ) )
        float ScrollY = 0.0f;

        // The pitch's first half, and the reason the window is arithmetic instead of a search. Clamped to
        // at least 1 design px where it is read: a pitch of zero makes the window unbounded, which turns
        // this element into the whole-list walk it exists to replace.
        PROPERTY( DisplayName( "Item Height" ), Category( "UI List View" ), Range( 1.0f, 512.0f ) )
        float ItemHeight = 40.0f;

        PROPERTY( DisplayName( "Spacing" ), Category( "UI List View" ), Range( 0.0f, 128.0f ) )
        float Spacing = 0.0f;

        // Rows kept in the walk past each edge of the viewport. NOT a safety margin for sloppy geometry —
        // it is what gives a row's per-frame state a life beyond one pixel of scrolling. A UIRenderTexture
        // row holds a renderer slot that comes back by DESTRUCTION (UIRenderTextureSource.hpp), so at zero
        // overscan a one-pixel jitter across the edge tears down a whole SceneRenderer and builds it again
        // next frame. One row of overscan is what that costs to avoid.
        PROPERTY( DisplayName( "Overscan Rows" ), Category( "UI List View" ), Range( 0, 8 ) )
        int Overscan = 1;

        // The two colours are the SCROLLING-CONTAINER slots, shared with UIScrollView on purpose — see
        // UIStyleSlots.hpp. They must stay equal to UIScrollViewData's, or a theme that binds the one slot
        // changes this element's look and not that one's; Desert/Tests/Engine/UIListView pins it.
        PROPERTY( DisplayName( "Background" ), Category( "UI List View" ), Color )
        glm::vec3 Background = glm::vec3( 0.10f, 0.11f, 0.14f );

        PROPERTY( DisplayName( "Show Scrollbar" ), Category( "UI List View" ) )
        bool ShowScrollbar = true;

        PROPERTY( DisplayName( "Scrollbar Color" ), Category( "UI List View" ), Color )
        glm::vec3 ScrollbarColor = glm::vec3( 0.35f, 0.37f, 0.44f );
    };
    struct UIListViewComponent
    {
        UIListViewData Data;
    };

    // A single-line text input. Click to focus (runtime), then typing edits Text; a caret shows at the end.
    // Placeholder shows (dimmed) when empty + unfocused.
    struct UIInputFieldData
    {
        REFLECT()

        PROPERTY( DisplayName( "Text" ), Category( "UI Input Field" ) )
        std::string Text;

        // Localisable on the same terms as UIText::Text — a leading hash is a string-table key. The user's
        // own typed `Text` above is NOT: it is what the player wrote, and translating it would be absurd.
        PROPERTY( DisplayName( "Placeholder" ), Category( "UI Input Field" ),
                  Tooltip( "Shown while empty. A leading hash makes it a string-table key instead" ) )
        std::string Placeholder = "Enter text...";

        PROPERTY( DisplayName( "Font Size" ), Category( "UI Input Field" ), Range( 6.0f, 96.0f ) )
        float FontSize = 20.0f;

        PROPERTY( DisplayName( "Text Color" ), Category( "UI Input Field" ), Color )
        glm::vec3 TextColor = glm::vec3( 0.92f, 0.94f, 0.98f );

        PROPERTY( DisplayName( "Placeholder Color" ), Category( "UI Input Field" ), Color )
        glm::vec3 PlaceholderColor = glm::vec3( 0.45f, 0.47f, 0.52f );

        PROPERTY( DisplayName( "Background" ), Category( "UI Input Field" ), Color )
        glm::vec3 Background = glm::vec3( 0.10f, 0.11f, 0.14f );

        PROPERTY( DisplayName( "Focus Border" ), Category( "UI Input Field" ), Color )
        glm::vec3 FocusColor = glm::vec3( 0.30f, 0.55f, 0.90f );

        PROPERTY( DisplayName( "Corner Radius" ), Category( "UI Input Field" ), Range( 0.0f, 32.0f ) )
        float CornerRadius = 4.0f;
    };
    struct UIInputFieldComponent
    {
        UIInputFieldData Data;
    };

    // A dropdown / combo box. Shows the selected option; a click opens a list of Options (';'-separated) below
    // it, drawn on top of everything. Picking an option sets SelectedIndex and closes.
    struct UIDropdownData
    {
        REFLECT()

        // EACH OPTION is localisable on its own — a leading hash on one entry makes that entry a key, and
        // the separator is not part of any of them. Per option rather than per list because a dropdown
        // mixes translated labels with proper nouns (a server name, a player's own preset) far more often
        // than it is wholly one or the other.
        PROPERTY( DisplayName( "Options (';'-separated)" ), Category( "UI Dropdown" ),
                  Tooltip( "One entry per option. A leading hash on an entry makes that entry a "
                           "string-table key" ) )
        std::string Options = "Option A;Option B;Option C";

        PROPERTY( DisplayName( "Selected Index" ), Category( "UI Dropdown" ) )
        int SelectedIndex = 0;

        PROPERTY( DisplayName( "Open" ), Category( "UI Dropdown" ) )
        bool Open = false;

        PROPERTY( DisplayName( "Font Size" ), Category( "UI Dropdown" ), Range( 6.0f, 96.0f ) )
        float FontSize = 20.0f;

        PROPERTY( DisplayName( "Background" ), Category( "UI Dropdown" ), Color )
        glm::vec3 Background = glm::vec3( 0.16f, 0.17f, 0.21f );

        PROPERTY( DisplayName( "Text Color" ), Category( "UI Dropdown" ), Color )
        glm::vec3 TextColor = glm::vec3( 0.92f, 0.94f, 0.98f );

        PROPERTY( DisplayName( "Highlight" ), Category( "UI Dropdown" ), Color )
        glm::vec3 Highlight = glm::vec3( 0.26f, 0.40f, 0.62f );

        PROPERTY( DisplayName( "Corner Radius" ), Category( "UI Dropdown" ), Range( 0.0f, 32.0f ) )
        float CornerRadius = 4.0f;
    };
    struct UIDropdownComponent
    {
        UIDropdownData Data;
    };

    // ------------------------------------------------------------------------------------------------
    // WHERE AN ELEMENT'S COLOURS, FONTS AND SPACINGS COME FROM (Ю13)
    //
    // The canvas names a theme; this component names which STYLE of that theme this element resolves
    // through. An element with no UIStyleComponent uses the theme's "Default" style — theming must not
    // require an edit of every entity, which is the thing themes exist to avoid.
    //
    // THE PRECEDENCE IS NOT A RULE, IT IS A TABLE. A style binds some slots and not others; a bound slot
    // comes from the theme, an unbound one from the element's own authored field. There is never a moment
    // when both apply, so nothing can quietly beat anything — and the Details "UI Style" block prints the
    // source of every slot, so an author can see which of the two a colour came from without guessing.
    // ------------------------------------------------------------------------------------------------

    // Does this element resolve through the canvas's theme at all?
    //
    // WHY AN ENUM AND NOT A RESERVED STYLE NAME like "None": a magic string is exactly the silent
    // convention this decision is trying to avoid, and it cannot be seen in the Details panel. Both
    // values are read by UICanvasRenderer2D.cpp.
    enum class UIStyleSource
    {
        Theme, // resolve bound slots from the canvas's theme; unbound ones stay local
        Local  // ignore the theme entirely — every slot is this element's own authored field
    };

    struct UIStyleData
    {
        REFLECT()

        PROPERTY( DisplayName( "Source" ), Category( "UI Style" ),
                  Tooltip( "Theme: bound slots come from the canvas's theme, the rest from this element. "
                           "Local: every slot is this element's own value." ) )
        UIStyleSource Source = UIStyleSource::Theme;

        // The style's name inside the theme. A name the theme does not declare is REPORTED with the
        // element's tag and this name (once per canvas), and the element falls back to fully local — the
        // values its author actually typed — rather than to an invented default or to nothing drawn.
        PROPERTY( DisplayName( "Style" ), Category( "UI Style" ),
                  Tooltip( "A style declared by the canvas's theme, e.g. \"Default\" or \"Primary\"." ) )
        std::string Style = "Default";
    };
    struct UIStyleComponent
    {
        UIStyleData Data;
    };

    // Root of a screen-space UI tree. Child entities with a UILayout are laid out against this canvas. Add UI
    // elements as CHILDREN of the canvas entity (the viewport "UI" menu / UI Editor do this for you).
    struct UICanvasData
    {
        REFLECT()

        PROPERTY( DisplayName( "Scale Mode" ), Category( "UI Canvas" ) )
        UICanvasScaleMode ScaleMode = UICanvasScaleMode::Stretch;

        PROPERTY( DisplayName( "Render Mode" ), Category( "UI Canvas" ) )
        UICanvasRenderMode RenderMode = UICanvasRenderMode::ScreenSpace;

        PROPERTY( DisplayName( "World Scale" ), Category( "UI Canvas" ), Range( 1.0f, 4000.0f ) )
        float WorldScale = 400.0f; // WorldSpace: on-screen px per reference-unit at distance 1

        PROPERTY( DisplayName( "Reference Width" ), Category( "UI Canvas" ), Range( 64.0f, 7680.0f ) )
        float ReferenceWidth = 1280.0f;

        PROPERTY( DisplayName( "Reference Height" ), Category( "UI Canvas" ), Range( 64.0f, 4320.0f ) )
        float ReferenceHeight = 720.0f;

        PROPERTY( DisplayName( "Match Width/Height" ), Category( "UI Canvas" ), Range( 0.0f, 1.0f ) )
        float MatchWidthHeight = 0.5f; // ScaleWithScreen only: 0 = match width, 1 = match height

        // Asset<TextureAsset> is what names the asset TYPE to the serializer's resolver. Without it the
        // resolver gets an empty type string, falls through its table to the mesh lookup, and writes the
        // slot out as an EMPTY STRING — so every save cleared it.
        //
        // CORRECTED 2026-09-05: this comment used to say the annotation "is what makes this a HANDLE and
        // not a number", and that the field could not be authored without it. Neither is so.
        // DesertHeaderTool maps `Assets::AssetHandle` onto FieldType::AssetHandle by the TYPE's spelling
        // (main.cpp, MapFieldType), and the Details panel's texture picker is the default arm of that
        // field type — so the slot was always editable in the editor and always discarded on save. The
        // setting was dead in three places, but the third one is "nothing it was set to survived a save",
        // and the cause is the empty asset type, not the field type.
        PROPERTY( DisplayName( "Background Sprite" ), Category( "UI Canvas" ), Asset<TextureAsset> )
        Assets::AssetHandle Sprite; // drag a texture from the Content Browser; unset = transparent

        PROPERTY( DisplayName( "Visible" ), Category( "UI Canvas" ) )
        bool Visible = true;

        // WHICH CANVAS IS ON TOP, authored. A view draws EVERY canvas of its scene (Ю4), so with two of
        // them something has to decide the order — and "whichever entt hands out first" is a property of
        // the component pool, not a decision. Ascending: a higher Sort Order draws later, so it covers the
        // lower ones and takes the pointer from them. Equal values keep the scene file's own order.
        //
        // This is the knob the four overlay features are built on: a tooltip, a context menu, a modal and a
        // toast are each a canvas that must be above the HUD whatever order the level happened to create
        // them in. Read by UI::CanvasesInDrawOrder (UICanvasLayout.cpp).
        PROPERTY( DisplayName( "Sort Order" ), Category( "UI Canvas" ) )
        int SortOrder = 0;

        // Safe area (Phase B): per-edge insets (L/T/R/B, design px) the top-level content stays inside — for
        // mobile notches / rounded corners. On desktop set manually to preview a device; 0 = full canvas.
        PROPERTY( DisplayName( "Safe Area L/T/R/B" ), Category( "UI Canvas" ) )
        glm::vec4 SafeArea = glm::vec4( 0.0f );

        // --- Theme (Ю13) --------------------------------------------------------------------------------
        //
        // WHY THE THEME HANGS OFF THE CANVAS. A canvas is the root of one UI tree, and Ю4 already keyed
        // every piece of walk state by (canvas x view) precisely because a value owned by the PROCESS
        // cannot express two canvases or two viewports. A theme in a global would bring that back in the
        // place an author notices first: a HUD and a menu overlay in one scene could not have two looks,
        // and the UI Editor's preview could not show a theme the viewport is not on.
        //
        // WHY NOT PER ELEMENT. An element picks a STYLE (UIStyleComponent), which is a role INSIDE a
        // theme. Two themes inside one canvas is two palettes fighting on one screen, and the ancestor
        // walk it would cost is paid per element per frame for a capability a second canvas already
        // expresses — UICanvas has a Sort Order for exactly that.
        //
        // Empty is not an error and is the state of every scene authored before themes existed: each
        // element then draws the colours its author typed into it. Read by UICanvasRenderer2D.cpp.
        PROPERTY( DisplayName( "Theme" ), Category( "UI Theme" ), Asset<UIThemeAsset> )
        Assets::AssetHandle Theme;

        // ACCESSIBILITY, AND IT IS LIVE WITH OR WITHOUT A THEME. Multiplies every font size this canvas
        // draws at — the theme's, the element's own, and the auto-size floor — so raising it is one field
        // rather than an edit of every UIText in the scene. It is here and not in the theme because it is
        // the PLAYER's preference applied on top of whatever look the designer authored; a theme that
        // carried its own type scale would make "larger text" mean "a different theme".
        PROPERTY( DisplayName( "Font Scale" ), Category( "UI Theme" ), Range( 0.5f, 3.0f ) )
        float FontScale = 1.0f;

        // The other half of the accessibility pair: while this is on, a colour token the theme declares a
        // high-contrast value for resolves to that value instead. A theme that declares none is simply
        // unaffected, and a canvas with no theme is unaffected too — this switches a PALETTE, it does not
        // apply an algorithm to arbitrary colours, which is why it can never make a designed screen look
        // like something nobody drew. See Engine/Assets/UIThemeData.hpp for why the override table is
        // sparse rather than a second theme file.
        PROPERTY( DisplayName( "High Contrast" ), Category( "UI Theme" ) )
        bool HighContrast = false;
    };
    struct UICanvasComponent
    {
        UICanvasData Data;
    };

    // Aspect Ratio Fitter mode: which axis is derived from the other to hold the ratio (or off).
    enum class UIAspectMode
    {
        Off,
        HeightControlsWidth, // width = height * ratio
        WidthControlsHeight  // height = width / ratio
    };

    // ------------------------------------------------------------------------------------------------
    // ELEMENT VISIBILITY — TWO AXES, NOT ONE FIVE-VALUED WORD
    //
    // UE spells this as ESlateVisibility, one enum with five values. Decomposed, those five answer two
    // independent questions that were glued together, which is where the awkward names come from:
    //
    //   ESlateVisibility       drawn  keeps layout space  self hit-tests  children hit-test
    //   Visible                 yes         yes                yes              yes
    //   Hidden                  no          yes                no               no
    //   Collapsed               no          no                 no               no
    //   HitTestInvisible        yes         yes                no               no
    //   SelfHitTestInvisible    yes         yes                no               yes
    //
    // The first two columns are one question and the last two are another, so the two enums below are
    // that decomposition. Two fields are simpler to author and to serialize, they are strictly more
    // expressive than the five words (UE exposes five of the products; every product here is reachable),
    // and the second one is exactly the shape the walk already had — per-element flags.
    // ------------------------------------------------------------------------------------------------

    // Is the element on screen, and does it still hold its place in the parent's layout?
    //
    // Collapsed vs Hidden is the whole reason this axis exists and is only observable inside a
    // UILayoutGroup (VBox / HBox / Grid): a Collapsed child gets no slot and its siblings close the gap,
    // a Hidden one keeps its slot and leaves a hole. Under plain anchor layout the two look identical,
    // because siblings there are positioned against the parent and have nothing to close up.
    //
    // Neither is hit-testable: an element nobody can see must not eat clicks, which is also UE's rule.
    // Both take their whole sub-tree with them.
    enum class UIVisibility
    {
        Visible,  // drawn, holds its slot, hit-tested per UIHitTest below
        Hidden,   // not drawn, KEEPS its slot in the parent's layout group, hit-tests nothing
        Collapsed // not drawn, DROPS OUT of the parent's layout group, hit-tests nothing
    };

    // What the pointer sees of this element AND of everything under it.
    //
    // THIS AXIS ABSORBED THE TWO BOOLEANS THAT USED TO SIT HERE, and the mapping is:
    //   RaycastTarget = false  ->  ChildrenOnly   (identical behaviour: the element is transparent to the
    //                                              pointer, its children are not — UE SelfHitTestInvisible)
    //   Interactable  = false  ->  Blocking       (the element stops the pointer and responds to nothing;
    //                                              what is NEW is that its sub-tree is inert too, so
    //                                              greying out a form or a modal dialog is one field
    //                                              instead of one field per descendant)
    // `None` is the value neither boolean could express — UE's HitTestInvisible, where the element and its
    // whole sub-tree are transparent, so a decorative overlay lets every click through to what is behind.
    //
    // WHY FOUR VALUES AND NOT THREE. UE needs a second, separate concept (IsEnabled) for "visible, blocks
    // the pointer, responds to nothing", which is precisely what our Interactable was. Folding it in here
    // rather than leaving it beside this field keeps one source of truth for "what does the pointer do
    // with this element", at the price of one extra enumerator.
    enum class UIHitTest
    {
        All,          // the element and its children take the pointer and respond (the default)
        ChildrenOnly, // the element is transparent to the pointer; its children still take it
        Blocking,     // the element stops the pointer; neither it nor its sub-tree responds to anything
        None          // the element and its whole sub-tree are transparent to the pointer
    };

    // Godot Control-like rect: anchors (fraction of the parent rect, 0..1), offsets (pixels from the anchored
    // edges), a custom minimum size and content clipping. The layout solver turns these into a screen
    // rect each frame. AnchorMin==AnchorMax => fixed-size element positioned by offsets; spread anchors =>
    // element stretches with the parent.
    //
    // PIVOT IS BACK, AND IT IS BACK WITH ITS CONSUMER. Д26 deleted it because the rect was resolved from
    // anchors and offsets alone and nothing in this UI rotated or scaled an element about a point, which
    // made it a knob that moved nothing (§1.3). Ю8 is the rotation, so the three fields below arrive
    // TOGETHER: Pivot on its own would be dead again, and Rotation without Pivot could only ever turn an
    // element about its centre.
    struct UILayoutData
    {
        REFLECT()

        PROPERTY( DisplayName( "Anchor Min" ), Category( "UI Layout" ) )
        glm::vec2 AnchorMin = glm::vec2( 0.0f, 0.0f );

        PROPERTY( DisplayName( "Anchor Max" ), Category( "UI Layout" ) )
        glm::vec2 AnchorMax = glm::vec2( 0.0f, 0.0f );

        PROPERTY( DisplayName( "Offset Min" ), Category( "UI Layout" ) )
        glm::vec2 OffsetMin = glm::vec2( 0.0f, 0.0f ); // px from the AnchorMin edges (left/top)

        PROPERTY( DisplayName( "Offset Max" ), Category( "UI Layout" ) )
        glm::vec2 OffsetMax = glm::vec2( 160.0f, 48.0f ); // px from the AnchorMax edges (right/bottom)

        PROPERTY( DisplayName( "Custom Minimum Size" ), Category( "UI Layout" ) )
        glm::vec2 CustomMinimumSize = glm::vec2( 0.0f, 0.0f );

        // --- Render transform (Ю8) ---------------------------------------------------------------------
        // These do NOT take part in layout: the rect is still resolved from anchors and offsets, and a
        // rotated element occupies exactly the slot it would have occupied straight. They are applied
        // afterwards, to the geometry that rect produces and to the pointer that hits it, and they are
        // INHERITED — a rotated panel carries its whole sub-tree with it, because the transform is pushed
        // before the children are walked and popped after.
        //
        // WHY LAYOUT IS LEFT ALONE. The alternative — feeding the rotated bounds back into the parent's
        // auto-layout — makes a slider that spins an element also resize its siblings, and it makes the
        // layout solution depend on its own output for a nested rotation. Unity and Godot both draw this
        // line in the same place.

        // Degrees, POSITIVE = CLOCKWISE on screen (this space has y pointing down — the CSS `rotate()`
        // and Godot Control.rotation convention). About Pivot.
        PROPERTY( DisplayName( "Rotation" ), Category( "UI Transform" ), Range( -360.0f, 360.0f ) )
        float Rotation = 0.0f;

        // Multiplies the element's own size about Pivot. Non-uniform is allowed and is what an author
        // reaches for to flip a sprite (-1 on one axis). 1,1 = untouched, and an untouched element emits
        // byte-identical geometry to one with no transform at all — the walk skips the matrix entirely.
        PROPERTY( DisplayName( "Scale" ), Category( "UI Transform" ) )
        glm::vec2 Scale = glm::vec2( 1.0f, 1.0f );

        // The point Rotation and Scale act about, as a FRACTION of this element's own resolved rect:
        // 0,0 = its top-left corner, 0.5,0.5 = its centre, 1,1 = its bottom-right. A fraction rather
        // than pixels so it keeps its meaning when the element is resized or the canvas is scaled.
        PROPERTY( DisplayName( "Pivot" ), Category( "UI Transform" ) )
        glm::vec2 Pivot = glm::vec2( 0.5f, 0.5f );

        PROPERTY( DisplayName( "Clip Contents" ), Category( "UI Layout" ) )
        bool ClipContents = false;

        // Is this element on screen, and does it keep its place when it is not? Collapsed is the one that
        // changes the LAYOUT: inside a VBox/HBox/Grid it drops the element's slot and the siblings close
        // up, where Hidden leaves the hole. See UIVisibility.
        PROPERTY( DisplayName( "Visibility" ), Category( "UI Layout" ) )
        UIVisibility Visibility = UIVisibility::Visible;

        // What the pointer sees of this element and of its whole sub-tree. Replaces the Interactable /
        // Raycast Target pair, which said the same things per element and could not say them about a
        // sub-tree at all — see UIHitTest for which old flag became which value.
        PROPERTY( DisplayName( "Hit Test" ), Category( "UI Interaction" ) )
        UIHitTest HitTest = UIHitTest::All;

        // Aspect Ratio Fitter (Phase B): keep this width/height ratio, deriving the free axis about the centre.
        PROPERTY( DisplayName( "Aspect Ratio (W/H)" ), Category( "Fitter" ), Range( 0.0f, 8.0f ) )
        float AspectRatio = 0.0f; // 0 = off
        PROPERTY( DisplayName( "Aspect Mode" ), Category( "Fitter" ) )
        UIAspectMode AspectMode = UIAspectMode::HeightControlsWidth;

        // Layout Element (Phase B): inside a parent VBox/HBox, flexible children share the leftover main-axis
        // space by weight — >0 stretches this child to fill (or acts as a spacer). 0 = fixed preferred size.
        PROPERTY( DisplayName( "Flex Grow" ), Category( "Fitter" ), Range( 0.0f, 8.0f ) )
        float FlexGrow = 0.0f;

        // Content Size Fitter (Phase B): a layout-group container sizes itself to its children (hug content),
        // per axis. Keeps the anchored top-left. No effect on non-group elements.
        PROPERTY( DisplayName( "Fit Width To Content" ), Category( "Fitter" ) )
        bool FitWidth = false;
        PROPERTY( DisplayName( "Fit Height To Content" ), Category( "Fitter" ) )
        bool FitHeight = false;
    };
    struct UILayoutComponent
    {
        UILayoutData Data;
    };

    // A filled (rounded) rectangle — the background of a panel / window / button.
    struct UIPanelData
    {
        REFLECT()

        PROPERTY( DisplayName( "Color" ), Category( "UI Panel" ), Color )
        glm::vec3 Color = glm::vec3( 0.15f, 0.16f, 0.2f );

        PROPERTY( DisplayName( "Opacity" ), Category( "UI Panel" ), Range( 0.0f, 1.0f ) )
        float Opacity = 0.92f;

        PROPERTY( DisplayName( "Corner Radius" ), Category( "UI Panel" ), Range( 0.0f, 64.0f ) )
        float CornerRadius = 6.0f;

        // Frosted glass: fill the panel with the BLURRED scene behind it instead of a flat colour, with
        // Color/Opacity acting as the tint over that blur (Opacity 1 = an ordinary opaque panel again).
        // 0 = off; higher = blurrier (the renderer maps it onto its backdrop blur pyramid).
        PROPERTY( DisplayName( "Backdrop Blur" ), Category( "UI Panel" ), Range( 0.0f, 1.0f ),
                  Tooltip( "Fill with the blurred scene behind the panel (frosted glass). 0 = off." ) )
        float BackdropBlur = 0.0f;

        // Asset<TextureAsset> is what tells the SERIALIZER which asset type this handle names. Without it
        // the resolver is handed an empty type string, falls through its table to the mesh lookup, finds
        // no mesh under a texture's handle and writes the field out as an EMPTY STRING — so the slot was
        // silently cleared by every save. (It is not what makes the field a handle: DesertHeaderTool maps
        // `Assets::AssetHandle` to FieldType::AssetHandle by its spelling, which is why the Details panel
        // could always offer a picker for it. The setting could be authored and could not be kept.)
        PROPERTY( DisplayName( "Sprite" ), Category( "UI Panel" ), Asset<TextureAsset>, Preview )
        Assets::AssetHandle Sprite; // optional background image, tinted by Color * Opacity. Unset = flat colour.

        PROPERTY( DisplayName( "Sprite Border L/T/R/B" ), Category( "UI Panel" ) )
        glm::vec4 SpriteBorder = glm::vec4( 0.0f ); // 9-slice: source-px borders kept unstretched (0 = stretch)

        // --- Material (Ю11) -------------------------------------------------------------------------------
        // A `.demat` whose shader declares `Domain UI` becomes this panel's FILL, in place of the colour,
        // the gradient, the sprite, the video and the glass. Its parameters are the shader's own, edited
        // in the Material Editor like every other material's, and they reach the pixel through THE
        // parameter transport (one row of `Materials[]`, named by a push constant) — so an author extends
        // the look with an expression instead of waiting for one more boolean to be added below.
        //
        // WHY IT REPLACES THE FILL RATHER THAN COMPOSING WITH IT. The panel's Color * Opacity still
        // travels, as the vertex colour the material may multiply by (the shipped UIMatRadialWipe does),
        // so nothing is lost; but a fill that were BOTH a sprite and a material would need the batcher to
        // key on two resources at once for one quad. Glow, Shadow and the Ring are separate quads and go
        // on composing around it exactly as they do around a sprite.
        //
        // A handle the UI path cannot execute — deleted asset, unregistered shader, a Surface material
        // dropped in here — draws the magenta hatch of `UIMatError` and is named in the log. It does NOT
        // fall back to the flat colour: a panel that quietly looks unmaterialised is the one failure mode
        // UE shipped and never fixed.
        PROPERTY( DisplayName( "Material" ), Category( "UI Material" ), Asset<MaterialAsset> )
        Assets::AssetHandle Material;

        PROPERTY( DisplayName( "Video" ), Category( "UI Panel" ), Asset<VideoAsset> )
        Assets::AssetHandle Video; // MPEG1 .mpg/.mpeg streamed into this panel (loops, tinted by Color*Opacity).
                                   // Drag a .mpg from the Content Browser. Overrides the sprite/gradient fill
                                   // while set. Unset = no video. (Handle<->path owned by the VideoService.)

        // --- Shape (Phase C) ------------------------------------------------------------------------------
        PROPERTY( DisplayName( "Circle" ), Category( "UI Panel" ) )
        bool Circle = false; // force a perfect circle/ellipse (rounding = half the shorter side) at any size —
                             // for avatars, badges, status dots. Overrides Corner Radius.

        PROPERTY( DisplayName( "Ring Width" ), Category( "Ring" ), Range( 0.0f, 24.0f ) )
        float RingWidth = 0.0f; // >0 draws a gradient ring hugging the (circular or rounded) edge
        PROPERTY( DisplayName( "Ring Color A" ), Category( "Ring" ), Color )
        glm::vec3 RingColorA = glm::vec3( 1.0f, 0.48f, 0.15f );
        PROPERTY( DisplayName( "Ring Color B" ), Category( "Ring" ), Color )
        glm::vec3 RingColorB = glm::vec3( 0.18f, 0.89f, 1.0f ); // sweeps A -> B -> A around the ring

        // --- Animation (Phase F) --------------------------------------------------------------------------
        PROPERTY( DisplayName( "Pulse" ), Category( "Animation" ) )
        bool Pulse = false; // breathe the opacity between Pulse Min and full — a live "online" dot / CTA glow
        PROPERTY( DisplayName( "Pulse Speed" ), Category( "Animation" ), Range( 0.1f, 10.0f ), Units( "rad/s" ),
                  EditCondition( "Pulse" ) )
        float PulseSpeed = 2.5f; // radians/sec of the sine
        PROPERTY( DisplayName( "Pulse Min" ), Category( "Animation" ), Range( 0.0f, 1.0f ),
                  EditCondition( "Pulse" ) )
        float PulseMin = 0.35f; // opacity floor of the breathe

        // Effects (Phase 4). All in design px; scaled by the canvas scale at draw time.
        PROPERTY( DisplayName( "Use Gradient" ), Category( "Effects" ) )
        bool UseGradient = false; // vertical Color (top) -> Gradient Color (bottom); ignored when a sprite is set
        PROPERTY( DisplayName( "Gradient Color" ), Category( "Effects" ), Color, EditCondition( "UseGradient" ) )
        glm::vec3 GradientColor = glm::vec3( 0.10f, 0.11f, 0.14f );
        PROPERTY( DisplayName( "Border Width" ), Category( "Effects" ), Range( 0.0f, 16.0f ) )
        float BorderWidth = 0.0f; // 0 = no border
        PROPERTY( DisplayName( "Border Color" ), Category( "Effects" ), Color )
        glm::vec3 BorderColor = glm::vec3( 0.0f );
        PROPERTY( DisplayName( "Shadow" ), Category( "Effects" ) )
        bool Shadow = false;
        PROPERTY( DisplayName( "Shadow Color" ), Category( "Effects" ), Color )
        glm::vec3 ShadowColor = glm::vec3( 0.0f );
        PROPERTY( DisplayName( "Shadow Offset" ), Category( "Effects" ) )
        glm::vec2 ShadowOffset = glm::vec2( 3.0f, 4.0f );
        PROPERTY( DisplayName( "Glow" ), Category( "Effects" ) )
        bool Glow = false; // cheap ImDrawList glow: layered expanded rects behind, alpha falloff
        PROPERTY( DisplayName( "Glow Color" ), Category( "Effects" ), Color )
        glm::vec3 GlowColor = glm::vec3( 0.3f, 0.6f, 1.0f );
        PROPERTY( DisplayName( "Glow Size" ), Category( "Effects" ), Range( 0.0f, 48.0f ) )
        float GlowSize = 12.0f;
    };
    struct UIPanelComponent
    {
        UIPanelData Data;
    };

    // What a tween drives. From/To are read per property: Offset/Size use xy (design px), Opacity uses x,
    // Color uses rgb — one vec4 keeps the component flat instead of four half-used fields.
    enum class UITweenProperty
    {
        Offset,  // slide: shifts the resolved rect
        Size,    // grow/shrink: adds to the rect's width/height
        Opacity, // fade: multiplies the element's alpha
        Color    // tint: multiplies the element's colour
    };

    enum class UIEasing
    {
        Linear,
        QuadIn,
        QuadOut,
        QuadInOut,
        CubicIn,
        CubicOut,
        CubicInOut,
        BackOut, // overshoots then settles — the "pop" of a modal
        ElasticOut,
        BounceOut
    };

    enum class UITweenLoop
    {
        Once,
        Loop,
        PingPong
    };

    // A generic from->to animation on any UI element, evaluated while the canvas is drawn. It never
    // writes back into the authored fields — the value is applied on the way to the screen — so a tween
    // running in the editor cannot corrupt the scene, and Design mode previews it live.
    struct UITweenData
    {
        REFLECT()

        PROPERTY( DisplayName( "Property" ), Category( "UI Tween" ) )
        UITweenProperty Property = UITweenProperty::Offset;

        PROPERTY( DisplayName( "From" ), Category( "UI Tween" ),
                  Tooltip( "Offset/Size: xy in design px. Opacity: x. Color: rgb." ) )
        glm::vec4 From = glm::vec4( 0.0f );

        PROPERTY( DisplayName( "To" ), Category( "UI Tween" ) )
        glm::vec4 To = glm::vec4( 0.0f );

        PROPERTY( DisplayName( "Duration" ), Category( "UI Tween" ), Range( 0.01f, 20.0f ) )
        float Duration = 0.4f;

        PROPERTY( DisplayName( "Delay" ), Category( "UI Tween" ), Range( 0.0f, 20.0f ) )
        float Delay = 0.0f;

        PROPERTY( DisplayName( "Easing" ), Category( "UI Tween" ) )
        UIEasing Easing = UIEasing::CubicOut;

        PROPERTY( DisplayName( "Loop" ), Category( "UI Tween" ) )
        UITweenLoop Loop = UITweenLoop::Once;

        PROPERTY( DisplayName( "Playing" ), Category( "UI Tween" ) )
        bool Playing = true; // clear to freeze at the current value; set to (re)start from the delay

        PROPERTY( DisplayName( "Rewind On Hide" ), Category( "UI Tween" ) )
        bool RewindOnHide = true; // a canvas that goes invisible replays from the top when it returns
    };
    struct UITweenComponent
    {
        UITweenData Data;
    };

    // What a binding drives on its element.
    enum class UIBindTarget
    {
        Text,    // replaces UIText's string (Format applies)
        Value,   // Slider / ProgressBar value
        Opacity, // multiplies the element (and its children) down
        Color,   // multiplies the element's colour
        Visible  // false hides the element and everything under it
    };

    // MVVM-lite: ties this element to a key in the UI data store, which gameplay (C++ or Lua via
    // ui.set) writes. Nothing is written back into the component — the bound value is applied on the way
    // to the screen — so a binding can never overwrite what the author typed.
    //
    // THERE USED TO BE A `Format` FIELD HERE and it was deleted by Ю15, not deprecated. It held a printf
    // format the author typed in the Details panel and handed straight to `std::snprintf` with a double:
    // typing `%s` in an editor field was undefined behaviour at run time, and every number it printed was
    // formatted in the C locale on every screen in every language. Its job moved into the string table,
    // where a translator can put `{n}` (or `{n:2}`) wherever their language wants it and the number is
    // formatted for the reader's locale. The scene migration to version 19 drops the dead key by name.
    struct UIBindingData
    {
        REFLECT()

        // NOTE: the header tool reads a tooltip up to the first quote, so keep literals out of them.
        PROPERTY( DisplayName( "Key" ), Category( "UI Binding" ),
                  Tooltip( "Data-store key, e.g. player.hp — write it from Lua with ui.set( key, value )" ) )
        std::string Key;

        PROPERTY( DisplayName( "Target" ), Category( "UI Binding" ) )
        UIBindTarget Target = UIBindTarget::Text;
    };
    struct UIBindingComponent
    {
        UIBindingData Data;
    };

    // One keyframe of a UI animation track. Value is read exactly like UITweenData::From/To — xy for
    // Offset/Size, x for Opacity, rgb for Color — and Easing shapes the segment ENDING at this key.
    struct UIAnimKey
    {
        float     Time   = 0.0f;
        glm::vec4 Value  = glm::vec4( 0.0f );
        UIEasing  Easing = UIEasing::CubicOut;
    };

    // One property's lane on the timeline. Keys are kept sorted by time; a lane with a single key just
    // holds that value.
    struct UIAnimTrack
    {
        UITweenProperty        Property = UITweenProperty::Offset;
        std::vector<UIAnimKey> Keys;
    };

    // A multi-key UI animation, authored on the timeline (View -> Sequencer with a UI element selected).
    // UITween is the one-shot from->to; this is the clip: several properties, many keys, one clock.
    // Serialized by hand (ComponentRegistry) because the reflected path has no vector-of-struct support —
    // the Sequencer is its editor, not the Details grid.
    struct UIAnimData
    {
        std::vector<UIAnimTrack> Tracks;
        float                    Duration = 1.0f;
        bool                     Loop     = false;
        bool                     Playing  = true;

        // Playhead. RUNTIME only — never serialized, so scrubbing in the editor cannot dirty the scene.
        // The canvas advances it while Playing; the Sequencer pauses and writes it directly to scrub.
        float Time = 0.0f;
    };
    struct UIAnimComponent
    {
        UIAnimData Data;
    };

    // A screen (page) of a canvas: everything under this element is shown only while it is the current
    // screen. Sibling screens are the states of a small machine — a button with Action = ShowScreen moves
    // between them and BackScreen returns, so a menu with pages needs no scripting.
    struct UIScreenData
    {
        REFLECT()

        PROPERTY( DisplayName( "Screen Name" ), Category( "UI Screen" ) )
        std::string Name; // referenced by a ShowScreen button; empty = never selectable
    };
    struct UIScreenComponent
    {
        UIScreenData Data;
    };

    // How screens hand over. Lives on the canvas; the current screen and the back-stack are RUNTIME state
    // kept outside the component, so navigating in the editor never rewrites the authored scene.
    struct UIScreenStackData
    {
        REFLECT()

        PROPERTY( DisplayName( "Initial Screen" ), Category( "UI Screens" ) )
        std::string InitialScreen; // empty = the first UIScreen found

        PROPERTY( DisplayName( "Transition" ), Category( "UI Screens" ), Range( 0.0f, 3.0f ) )
        float TransitionTime = 0.25f; // 0 = cut

        PROPERTY( DisplayName( "Slide (px)" ), Category( "UI Screens" ), Range( -1200.0f, 1200.0f ) )
        float SlidePx = 60.0f; // the incoming screen slides in from this far right; out goes the other way

        PROPERTY( DisplayName( "Easing" ), Category( "UI Screens" ) )
        UIEasing Easing = UIEasing::CubicOut;
    };
    struct UIScreenStackComponent
    {
        UIScreenStackData Data;
    };

    // WHERE ALONG THE ROUTE a listener fires. A press does not belong to one element: it belongs to the
    // chain from the canvas down to whatever the pointer is over, and every element on that chain is
    // entitled to see it. Until this existed the press was delivered to the elected element ALONE, so
    // "this panel reacts to a click anywhere inside it" had to be spelled as a copy of the component on
    // every leaf, and "this child handled it, the panel behind must not" could not be spelled at all.
    //
    // Bubble is target -> canvas and is what a handler almost always wants: the innermost thing that cares
    // answers first. Tunnel is canvas -> target and is the only order in which an ancestor acts BEFORE its
    // own children, which is what makes Tunnel + StopPropagation express "this panel takes every press
    // inside it and its children never see one". (DOM calls Tunnel the capture phase; Slate calls it
    // tunnelling and routes its Preview* handlers that way. Same thing.)
    enum class UIEventPhase
    {
        Bubble, // fires on the way up: target first, canvas last
        Tunnel  // fires on the way down: canvas first, target last -- before any Bubble listener
    };

    // Pointer callbacks on any UI element. Each message is dispatched exactly like a button's action, so a
    // host that already handles UIButton actions handles these for free. Empty = that edge fires nothing.
    //
    // PRESS AND RELEASE ARE ROUTED along the ancestor chain (see UIEventPhase); ENTER AND EXIT ARE NOT, and
    // that asymmetry is deliberate rather than an omission. Enter/Exit fire on the DIFFERENCE between the
    // chain the pointer was on and the one it is on now, because the naive alternative -- bubble them like
    // a press -- makes a parent that lights up on hover flicker every time the pointer crosses between two
    // of its own children: the shared parent would receive Exit and then Enter although the pointer never
    // left it. DOM draws the same line (mouseenter/mouseleave do not bubble, mouseover/mouseout do) and
    // Slate walks the difference of the two widget paths for the same reason.
    //
    // A listener only fires on an element whose own UIHitTest is All. ChildrenOnly means the pointer does
    // not see THIS element, so it must not be told about a press it is transparent to; Blocking means the
    // element stops the pointer and responds to nothing. Both are the hit-test axis being obeyed by the
    // routing rather than restated in it.
    struct UIPointerEventsData
    {
        REFLECT()

        PROPERTY( DisplayName( "On Enter" ), Category( "UI Pointer Events" ) )
        std::string OnEnterMessage;

        PROPERTY( DisplayName( "On Exit" ), Category( "UI Pointer Events" ) )
        std::string OnExitMessage;

        PROPERTY( DisplayName( "On Press" ), Category( "UI Pointer Events" ) )
        std::string OnDownMessage;

        PROPERTY( DisplayName( "On Release" ), Category( "UI Pointer Events" ) )
        std::string OnUpMessage;

        // Which pass of the press/release route this listener answers on. No effect on Enter/Exit, which
        // are not routed -- see the note above the struct.
        PROPERTY( DisplayName( "Phase" ), Category( "UI Pointer Events" ) )
        UIEventPhase Phase = UIEventPhase::Bubble;

        // End the press/release route here: no further element on the chain hears this event, in either
        // pass. Independent of whether this listener's own message is empty, so a full-screen scrim can
        // swallow every press inside a modal without emitting anything -- and a scrim that DOES emit is
        // click-outside-to-close, which UIHitTest::Blocking cannot be, because Blocking responds to nothing.
        PROPERTY( DisplayName( "Stop Propagation" ), Category( "UI Pointer Events" ) )
        bool StopPropagation = false;
    };
    struct UIPointerEventsComponent
    {
        UIPointerEventsData Data;
    };

    // Makes an element draggable. Pressing and moving past a small threshold starts a drag carrying
    // `Payload`; a ghost of the element follows the cursor until release (see UIDropTargetData).
    struct UIDraggableData
    {
        REFLECT()

        PROPERTY( DisplayName( "Payload" ), Category( "UI Drag" ) )
        std::string Payload; // e.g. "item:sword" — a drop target filters on its prefix

        PROPERTY( DisplayName( "Ghost Opacity" ), Category( "UI Drag" ), Range( 0.0f, 1.0f ) )
        float GhostOpacity = 0.55f;
    };
    struct UIDraggableComponent
    {
        UIDraggableData Data;
    };

    // Receives a dropped payload. While a drag is in flight every target that ACCEPTS it outlines itself,
    // so the valid destinations are obvious; releasing over one dispatches "OnDropMessage|payload".
    struct UIDropTargetData
    {
        REFLECT()

        PROPERTY( DisplayName( "Accepts (prefix)" ), Category( "UI Drop" ) )
        std::string Accepts; // "" = anything; "item:" = only payloads starting with it

        PROPERTY( DisplayName( "On Drop" ), Category( "UI Drop" ) )
        std::string OnDropMessage;

        PROPERTY( DisplayName( "Highlight" ), Category( "UI Drop" ), Color )
        glm::vec3 HighlightColor = glm::vec3( 0.35f, 0.75f, 1.0f );
    };
    struct UIDropTargetComponent
    {
        UIDropTargetData Data;
    };

    // ------------------------------------------------------------------------------------------------
    // OVERLAYS (Ю12) — A TOOLTIP, A CONTEXT MENU, A MODAL AND A TOAST ARE ONE THING FOUR TIMES
    //
    // Each of them is a CANVAS with a higher UICanvasData::SortOrder, drawn after the rest and therefore
    // taking the pointer from whatever it covers — which is exactly what Ю4 built and what
    // `UICanvasContextPair.TheCanvasDrawnLastTakesThePointerFromTheOneBelowIt` already asserts. Nothing
    // here re-implements stacking, hit testing or input capture; this component says WHICH canvas is an
    // overlay, of what kind, and what its policy is.
    //
    // WHO OWNS AN OVERLAY. The author does. An overlay canvas is an ordinary entity of the scene, created
    // by the author (or by the editor's "UI -> Overlay" menu) and destroyed with the scene. NOTHING is
    // created or destroyed at runtime, which is the whole reason this shape was chosen: the alternative —
    // spawning an overlay entity on demand — reproduces the defect class the renderer already paid for
    // once, where a preview kept its slot until somebody remembered to destroy it
    // (Docs/RENDERER_FRAME_STATE.md). Here there is no runtime object to leak, and the overlay survives a
    // scene reload because it IS the scene.
    //
    // WHAT IS RUNTIME, THEN. Only "is it open, where is it, and what does it say", and all of that lives
    // in the (canvas x view) cell (UICanvasContext::Overlay*), never in this component. Two viewports of
    // one scene can therefore have the same context menu open in different places, and opening one in the
    // editor's preview does not dirty the level. That is the same line UIScreen already draws.
    // ------------------------------------------------------------------------------------------------

    // What kind of overlay this canvas is. The kind decides which policy fields below are read, exactly as
    // UICanvasData::RenderMode decides whether WorldScale is read and ScaleMode decides MatchWidthHeight.
    enum class UIOverlayKind
    {
        Tooltip,     // opens after a hover delay on a trigger, follows the pointer, closes when it leaves
        ContextMenu, // opens on a click, closes on Escape / a click outside; a submenu is one of these too
        Modal,       // draws a scrim over the whole view and takes every click that misses its content
        Toast        // a queue of notifications that time out on their own and never take the pointer
    };

    // What makes a trigger fire. Hover is what a tooltip and a submenu use; the two click edges are what a
    // context menu and a modal use.
    enum class UIOverlayTriggerEvent
    {
        Hover,
        LeftClick,
        RightClick
    };

    // Put this on a CANVAS entity. The canvas keeps every field it already had — Sort Order is still what
    // decides which overlay is above which, so a submenu is simply a ContextMenu canvas with a higher one.
    struct UIOverlayData
    {
        REFLECT()

        PROPERTY( DisplayName( "Kind" ), Category( "UI Overlay" ) )
        UIOverlayKind Kind = UIOverlayKind::Tooltip;

        // How a trigger addresses this overlay. Names are looked up across the scene and a duplicate is
        // REFUSED by name rather than resolved to one of them — the same rule as UI::SoleCanvas, and for
        // the same reason: "there are two and I picked one" is a silent wrong answer.
        PROPERTY( DisplayName( "Name" ), Category( "UI Overlay" ) )
        std::string Name;

        // Tooltip / ContextMenu: the gap in design px between the thing the overlay is placed against (the
        // pointer, or the trigger's rect) and the overlay's own box. Read by UI::PlaceOverlay through
        // UICanvasRenderer2D's overlay update. Meaningless for Modal and Toast, which have no origin to be
        // placed against — their authored anchors place them against the view itself.
        PROPERTY( DisplayName( "Gap" ), Category( "UI Overlay" ) )
        glm::vec2 Gap = glm::vec2( 14.0f, 18.0f );

        // Hover triggers only: how long the pointer must rest on the trigger before this opens. 0 opens on
        // the first frame of contact, which is what a submenu usually wants and what a tooltip never does.
        PROPERTY( DisplayName( "Open Delay" ), Category( "UI Overlay" ), Range( 0.0f, 3.0f ) )
        float OpenDelay = 0.4f;

        // Tooltip: does the box follow the pointer while it stays on the trigger, or is it pinned to the
        // trigger's own rect? Pinned is what a long tooltip on a small button wants; following is what a
        // cursor hint wants.
        PROPERTY( DisplayName( "Follow Pointer" ), Category( "UI Overlay" ) )
        bool FollowPointer = true;

        PROPERTY( DisplayName( "Close On Escape" ), Category( "UI Overlay" ) )
        bool CloseOnEscape = true;

        // A press that the overlay's own content did not take closes it. For a Modal this is the scrim
        // being clicked; for a ContextMenu it is a click anywhere else on screen.
        PROPERTY( DisplayName( "Close On Click Outside" ), Category( "UI Overlay" ) )
        bool CloseOnClickOutside = true;

        // Modal: the dim drawn over the WHOLE view, under this canvas's own content and over everything
        // below it. It is drawn by the walk rather than authored as a panel because it is also what takes
        // the pointer: the scrim is elected as the frame's hot element wherever the modal's own content is
        // not, which is what makes "the click does not reach what is underneath" a consequence of Ю4's
        // single election instead of a second, parallel rule.
        PROPERTY( DisplayName( "Scrim Color" ), Category( "UI Overlay" ), Color )
        glm::vec3 ScrimColor = glm::vec3( 0.0f, 0.0f, 0.0f );

        PROPERTY( DisplayName( "Scrim Opacity" ), Category( "UI Overlay" ), Range( 0.0f, 1.0f ) )
        float ScrimOpacity = 0.55f;

        // Toast: how long one notification stays on screen before it leaves on its own.
        PROPERTY( DisplayName( "Toast Lifetime" ), Category( "UI Overlay" ), Range( 0.5f, 30.0f ) )
        float ToastLifetime = 3.0f;

        // Toast: how many notifications are on screen at once, and therefore how many slots the author
        // wired into this canvas. The runtime publishes exactly this many key sets (overlay.toast.N.text /
        // .visible) into the canvas's per-view locals; anything raised beyond them waits in a bounded
        // queue. See UIOverlay.hpp for why the queue is bounded and what it drops.
        PROPERTY( DisplayName( "Toast Slots" ), Category( "UI Overlay" ), Range( 1, 8 ) )
        int ToastSlots = 3;
    };
    struct UIOverlayComponent
    {
        UIOverlayData Data;
    };

    // Put this on an ELEMENT. It says "when the pointer does X to me, open that overlay" — and, for a
    // Toast, "raise a notification saying this".
    //
    // A trigger is not a control: it sits beside a button, a panel or an icon and adds one edge to it. A
    // menu item that opens a submenu is a button with a Hover trigger naming the submenu's canvas, which
    // is why nesting needed no machinery of its own.
    struct UIOverlayTriggerData
    {
        REFLECT()

        PROPERTY( DisplayName( "Overlay" ), Category( "UI Overlay Trigger" ) )
        std::string Overlay; // the UIOverlayData::Name to open

        PROPERTY( DisplayName( "On" ), Category( "UI Overlay Trigger" ) )
        UIOverlayTriggerEvent On = UIOverlayTriggerEvent::Hover;

        // Published into the opened overlay's per-view locals under the key `overlay.text`, so a Text
        // element inside the overlay carrying a UIBinding to that key shows it. For a Toast this is the
        // notification's own line. Empty publishes nothing, and an overlay whose chrome is entirely
        // authored needs none.
        PROPERTY( DisplayName( "Text" ), Category( "UI Overlay Trigger" ) )
        std::string Text;
    };
    struct UIOverlayTriggerComponent
    {
        UIOverlayTriggerData Data;
    };

    // A vector icon. The artwork is an ASSET — an .svg imported once into a signed distance field
    // (Runtime::IconService) — so icons are added by dropping a file in, never by touching C++, and they
    // stay crisp at any size because the same SDF shader that draws text reconstructs the edge.
    // Place as a child of a button/panel, or standalone for status glyphs.
    struct UIIconData
    {
        REFLECT()

        PROPERTY( DisplayName( "Icon" ), Category( "UI Icon" ), Asset<IconAsset> )
        Assets::AssetHandle Icon; // .svg vector icon — drag one from the Content Browser or pick a built-in

        PROPERTY( DisplayName( "Color" ), Category( "UI Icon" ), Color )
        glm::vec3 Color = glm::vec3( 1.0f );

        PROPERTY( DisplayName( "Scale" ), Category( "UI Icon" ), Range( 0.2f, 1.0f ) )
        float Scale = 0.7f; // icon size as a fraction of the element's shorter side
    };
    struct UIIconComponent
    {
        UIIconData Data;
    };

    // A plain image block: draws a sprite (any PNG/JPG/TGA — or an animated GIF) filling the element rect,
    // tinted by Tint*Opacity. The full-colour, raster alternative to the monochrome vector UIIcon —
    // drag any texture onto Sprite and position it freely with the element's anchors. 9-slice supported.
    struct UIImageData
    {
        REFLECT()

        PROPERTY( DisplayName( "Sprite" ), Category( "UI Image" ), Asset<TextureAsset> )
        Assets::AssetHandle Sprite; // drag a texture (PNG/JPG/TGA/GIF) from the Content Browser

        PROPERTY( DisplayName( "Tint" ), Category( "UI Image" ), Color )
        glm::vec3 Tint = glm::vec3( 1.0f );

        PROPERTY( DisplayName( "Opacity" ), Category( "UI Image" ), Range( 0.0f, 1.0f ) )
        float Opacity = 1.0f;

        PROPERTY( DisplayName( "Sprite Border L/T/R/B" ), Category( "UI Image" ) )
        glm::vec4 SpriteBorder = glm::vec4( 0.0f ); // 9-slice: source-px borders kept unstretched (0 = stretch)
    };
    struct UIImageComponent
    {
        UIImageData Data;
    };

    // A LIVE WORLD INSIDE A UI ELEMENT (Ю16). The element's rect is filled with another scene — named by
    // its .desce path — rendered offscreen every frame through that scene's own camera, then sampled as a
    // texture. UE calls the family "render target": a character portrait in a menu, an inventory item you
    // can turn, a security-camera feed, a minimap.
    //
    // WHY A SCENE FILE AND NOT A MESH SLOT. A mesh slot would need this component to carry a camera, a
    // light rig and a background as well, and every one of those is a thing the scene editor already
    // authors better than a Details page can. The author builds the little world as a normal scene, puts
    // its camera where the shot should be, saves, and names it here — so what the element shows is
    // WYSIWYG in the tool that already exists, and this component stays four fields.
    //
    // WHY IT IS NOT FREE, said here because the price is the design. Each of these elements that is
    // actually on screen owns a Graphic::SceneRenderer, and therefore one of the six renderer slots
    // (Engine/Core/RendererSlotPool.hpp). The seventh is REFUSED, by name and with numbers, and draws the
    // magenta error fill rather than nothing — see Engine/UI/UIRenderTextureSource.hpp for who decides
    // and Engine/Graphic/Render2D/UIRenderTextureCache.hpp for the accounting. An element the walk did
    // not draw this frame — scrolled away, or not Visible — is not on screen, and its slot goes back.
    struct UIRenderTextureData
    {
        REFLECT()

        // The scene to render, as a path a host can open ("Resources/Assets/Scenes/UI_Portrait.desce").
        // A PATH and not an AssetHandle because scenes are not assets in this engine: there is no
        // SceneAsset type, no service that hands one out, and the only other place that names a scene
        // from a component — UIButtonData::Action, "scene:<path>" — names it exactly this way. Inventing
        // a handle type for one field would be a second identity for a file the project already
        // identifies by path.
        PROPERTY( DisplayName( "Scene" ), Category( "UI Render Texture" ),
                  Tooltip( "Path to a .desce rendered live into this element, e.g. "
                           "Resources/Assets/Scenes/UI_Portrait.desce" ) )
        std::string ScenePath;

        PROPERTY( DisplayName( "Tint" ), Category( "UI Render Texture" ), Color )
        glm::vec3 Tint = glm::vec3( 1.0f );

        PROPERTY( DisplayName( "Opacity" ), Category( "UI Render Texture" ), Range( 0.0f, 1.0f ) )
        float Opacity = 1.0f;

        // Offscreen pixels per element pixel. 1 renders the world at the size it is shown; below that it
        // is cheaper and softer, above it supersamples. It is a knob on the PICTURE and on the cost at
        // once, which is why it is the only performance control here — the rest of the world's quality is
        // the scene's own business.
        PROPERTY( DisplayName( "Resolution Scale" ), Category( "UI Render Texture" ), Range( 0.25f, 2.0f ) )
        float ResolutionScale = 1.0f;
    };
    struct UIRenderTextureComponent
    {
        UIRenderTextureData Data;
    };

    // Screen-space text label (distinct from the 3D world-space TextComponent).
    struct UITextData
    {
        REFLECT()

        // A LEADING '#' MAKES THIS A KEY into the project's string tables ("#menu.play"); anything else is
        // a literal and is never translated. '##' at the start is an escape for a literal '#'. One field,
        // because two (a literal and a key, with a rule about which wins) is a defect class this project
        // has a name for — see Engine/Localization/LocalizedText.hpp for why this shape was chosen over
        // the other two.
        PROPERTY( DisplayName( "Text" ), Category( "UI Text" ),
                  Tooltip( "Shown as typed. A leading hash makes it a string-table key instead" ) )
        std::string Text = "Label";

        PROPERTY( DisplayName( "Font Size" ), Category( "UI Text" ), Range( 6.0f, 200.0f ) )
        float FontSize = 22.0f;

        PROPERTY( DisplayName( "Font" ), Category( "UI Text" ), Asset<FontAsset> )
        Assets::AssetHandle Font; // SDF font asset — drag a .ttf from the Content Browser or pick a preloaded
                                  // one. Unset = the engine's built-in default (Roboto).

        PROPERTY( DisplayName( "Color" ), Category( "UI Text" ), Color )
        glm::vec3 Color = glm::vec3( 1.0f, 1.0f, 1.0f );

        PROPERTY( DisplayName( "Alignment" ), Category( "UI Text" ) )
        UITextAlign Align = UITextAlign::Center;

        PROPERTY( DisplayName( "Vertical Align" ), Category( "UI Text" ) )
        UITextVAlign VerticalAlign = UITextVAlign::Middle;

        // --- Layout (Phase E) ---------------------------------------------------------------------------
        PROPERTY( DisplayName( "Word Wrap" ), Category( "Layout" ) )
        bool Wrap = false; // break long lines at word boundaries to fit the element width

        PROPERTY( DisplayName( "Line Spacing" ), Category( "Layout" ), Range( 0.5f, 3.0f ) )
        float LineSpacing = 1.0f; // multiplier on the font's natural line height

        PROPERTY( DisplayName( "Auto Size" ), Category( "Layout" ) )
        bool AutoSize = false; // shrink the font (down to Min Font Size) until the block fits the rect

        PROPERTY( DisplayName( "Min Font Size" ), Category( "Layout" ), Range( 4.0f, 200.0f ) )
        float MinFontSize = 8.0f; // floor for Auto Size

        PROPERTY( DisplayName( "Overflow" ), Category( "Layout" ) )
        UITextOverflow Overflow = UITextOverflow::Overflow; // what to do when the text still doesn't fit

        PROPERTY( DisplayName( "Rich Text" ), Category( "Layout" ) )
        bool RichText = false; // parse BBCode tags: [color=#rrggbb]..[/color], [b]..[/b]

        // --- Animation (Phase F) --------------------------------------------------------------------------
        PROPERTY( DisplayName( "Marquee" ), Category( "Animation" ) )
        bool Marquee = false; // horizontally scroll the text (single line, clipped) — a news/ticker banner
        PROPERTY( DisplayName( "Marquee Speed" ), Category( "Animation" ), Range( 5.0f, 400.0f ) )
        float MarqueeSpeed = 60.0f; // design px/sec

        // Effects (Phase 4).
        PROPERTY( DisplayName( "Shadow" ), Category( "Effects" ) )
        bool Shadow = false;
        PROPERTY( DisplayName( "Shadow Color" ), Category( "Effects" ), Color )
        glm::vec3 ShadowColor = glm::vec3( 0.0f );
        PROPERTY( DisplayName( "Shadow Offset" ), Category( "Effects" ) )
        glm::vec2 ShadowOffset = glm::vec2( 1.0f, 1.0f );
        PROPERTY( DisplayName( "Outline" ), Category( "Effects" ) )
        bool Outline = false;
        PROPERTY( DisplayName( "Outline Color" ), Category( "Effects" ), Color )
        glm::vec3 OutlineColor = glm::vec3( 0.0f );
    };
    struct UITextComponent2D
    {
        UITextData Data;
    };

    // Interactive button: tints its panel by pointer state, and on click hands its encoded action to the
    // HOST (RenderCanvas2D writes it to `outClicked`) -- the canvas itself knows nothing about scenes,
    // URLs or scripting. LoadScene/QuitGame/OpenURL are executed by the host; everything else, SendMessage
    // included, goes on UI::UIMessageQueue and reaches every Lua script defining OnUIMessage. The word
    // "dispatches ... to Lua" used to stand here and was FALSE: both hosts logged the message and never
    // queued it, so the one action documented as a "gameplay event name" was the one that arrived nowhere.
    //
    // What a UI Button does when clicked. The target/payload is the button's "Action Target" string:
    //  LoadScene   -> load that scene path        SendMessage -> gameplay event name (Lua/scripts)
    //  QuitGame    -> quit (target ignored)        OpenURL     -> open the URL
    enum class UIButtonAction
    {
        None,
        SendMessage,
        LoadScene,
        QuitGame,
        OpenURL,
        ShowScreen, // switch the canvas to the UIScreen named in On Click Message (pushes onto the stack)
        BackScreen  // return to the screen underneath (does nothing at the bottom of the stack)
    };

    struct UIButtonData
    {
        REFLECT()

        PROPERTY( DisplayName( "Normal" ), Category( "UI Button" ), Color )
        glm::vec3 NormalColor = glm::vec3( 0.20f, 0.40f, 0.70f );

        PROPERTY( DisplayName( "Hover" ), Category( "UI Button" ), Color )
        glm::vec3 HoverColor = glm::vec3( 0.30f, 0.52f, 0.82f );

        PROPERTY( DisplayName( "Pressed" ), Category( "UI Button" ), Color )
        glm::vec3 PressedColor = glm::vec3( 0.15f, 0.30f, 0.55f );

        PROPERTY( DisplayName( "Click Action" ), Category( "UI Button" ) )
        UIButtonAction Action = UIButtonAction::SendMessage;

        PROPERTY( DisplayName( "Action Target" ), Category( "UI Button" ) )
        std::string OnClickMessage = ""; // scene path / message name / URL, depending on Action

        // All three carry Asset<TextureAsset> for the reason UIPanelData::Sprite states in full: the
        // annotation is what names the asset TYPE to the serializer, and without it the resolver wrote
        // each of these out as an empty string. UICanvasRenderer2D reads all three (a button picks
        // Pressed, then Hover, then Sprite), so they were live in the draw and dead in the file.
        PROPERTY( DisplayName( "Sprite" ), Category( "UI Button" ), Asset<TextureAsset> )
        Assets::AssetHandle Sprite; // normal-state image, tinted by the state colour. Unset = flat colour.

        PROPERTY( DisplayName( "Hover Sprite" ), Category( "UI Button" ), Asset<TextureAsset> )
        Assets::AssetHandle HoverSprite; // shown on hover (falls back to Sprite if unset)

        PROPERTY( DisplayName( "Pressed Sprite" ), Category( "UI Button" ), Asset<TextureAsset> )
        Assets::AssetHandle PressedSprite; // shown while pressed (falls back to Sprite if unset)

        PROPERTY( DisplayName( "Sprite Border L/T/R/B" ), Category( "UI Button" ) )
        glm::vec4 SpriteBorder = glm::vec4( 0.0f ); // 9-slice: source-px borders kept unstretched (0 = stretch)

        // --- States (Phase D) -----------------------------------------------------------------------------
        PROPERTY( DisplayName( "Selected" ), Category( "State" ) )
        bool Selected = false; // persistent highlight (active menu item / current tab): rests on SelectedColor
                               // and draws an accent bar, until hover/press temporarily override it
        PROPERTY( DisplayName( "Selected Color" ), Category( "State" ), Color )
        glm::vec3 SelectedColor = glm::vec3( 0.85f, 0.42f, 0.18f );
        PROPERTY( DisplayName( "Selected Accent" ), Category( "State" ), Color )
        glm::vec3 SelectedAccent = glm::vec3( 1.0f, 0.55f, 0.2f ); // the left accent bar / ring colour

        PROPERTY( DisplayName( "Disabled" ), Category( "State" ) )
        bool Disabled = false; // greyed + non-interactive (ignores hover/press/click)
        PROPERTY( DisplayName( "Disabled Color" ), Category( "State" ), Color )
        glm::vec3 DisabledColor = glm::vec3( 0.22f, 0.24f, 0.28f );
    };
    struct UIButtonComponent
    {
        UIButtonData Data;
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
        REFLECT()

        // Hidden from the auto-generated Details (the widget draws a proper SkyboxAsset picker + DnD instead
        // of the builder's texture-oriented asset slot). Still serialized — Hidden is editor-only.
        PROPERTY( DisplayName( "Skybox" ), Category( "Skybox" ), Asset<SkyboxAsset>, Hidden )
        Assets::AssetHandle SkyboxHandle;

        // ── THE AUTHORED LOOK ─────────────────────────────────────────────────────────────────────────
        //
        // All three reach the frame through ONE route: they are baked into the environment cubes
        // (Graphic::SkyLook -> EnvironmentManager::Create), which is what the backdrop is drawn from AND
        // what every lit surface reads its ambient and reflections out of. Intensity used to be applied
        // to the sky pass alone and therefore lit nothing; that spelling is gone rather than kept beside
        // the new one.
        //
        // THE PRICE IS A REBAKE, not a frame — SkyboxRenderer waits for the value to settle and then
        // spends the same ~0.7 s the procedural sky spends when its sun moves. That is why none of these
        // is a per-frame knob and why none of them is animated.

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
    };

    struct ColliderComponent
    {
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
    };

    // Marks an entity as a physics body. Static = immovable, Dynamic = simulated, Kinematic = code-driven.
    struct RigidBodyComponent
    {
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
    };

    // A WASD-driven player. The follow camera is NOT here — parent a child entity with a CameraComponent
    // (offset behind = 3rd person, at the head = 1st person); it tracks the player via the hierarchy.
    struct CharacterControllerComponent
    {
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