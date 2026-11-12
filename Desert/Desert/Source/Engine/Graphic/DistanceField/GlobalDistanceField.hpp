#pragma once

// THE GLOBAL DISTANCE FIELD (UE GlobalDistanceField.cpp / GlobalDistanceField.h): the scene's mesh distance fields
// composited into four camera-centred clipmaps of 128^3 voxels (Lumen's extents: 25 m half-size for the first,
// doubling per level), each the distance in centimetres to the nearest surface clamped to four of its voxels, and
// its COVERAGE (UE's coverage atlas): 1 where a solid surface lies within a voxel, 0 where only foliage (a mostly
// two-sided mesh) or nothing does - a GI ray passes foliage stochastically (GlobalDistanceField.glslh).
// What a software trace marches when it needs "the scene" rather than one mesh: Shaders/Common/
// GlobalDistanceField.glslh is the one reading of it (the debug view today; GI, reflections and DF shadows next).
//
// INCREMENTAL, as UE's. A clipmap's window snaps to its 8^3 update bricks and is addressed toroidally (texel =
// world voxel mod 128), so a camera move rewrites only the brick slabs that entered the window; an object that
// appears, disappears, moves or changes its field dirties its old and new boxes; a first frame or a jump farther
// than the window rewrites the level whole. A frame where nothing moved dispatches nothing.
//
// Architecture differs from UE5 in two places, both reported in the GLOBAL-SDF REMAINDER: the clipmaps are dense
// (UE5 keeps a sparse page atlas plus a coarse mip, UE4 kept dense clipmaps like these), and objects are culled to
// update bricks on the CPU (UE culls on the GPU into a grid).

#include <Common/Core/Math/AABB.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/MeshDistanceField.hpp>
#include <Engine/Graphic/DistanceField/DistanceFieldSceneData.hpp>
#include <Engine/Graphic/DistanceField/MeshDistanceFieldReads.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert::ShaderResources
{
    class StorageBuffer;
}

namespace Desert::Graphic
{
    class Image;
    class Image2D;
    class Image3D;
    class Shader;
    class ComputePipeline;
    class GraphicsPipeline;
} // namespace Desert::Graphic

namespace Desert::Graphic::DistanceField
{
    inline constexpr int kGlobalDistanceFieldClipmaps = 4;
    // The clipmaps' slots, one named texture per level (Common/GITracer.glslh,
    // Debug/GlobalDistanceFieldView.shader declare GlobalDistanceField<level>Texture): every reader binds level i
    // through element i.
    inline constexpr std::array<RDG::TextureParameter, kGlobalDistanceFieldClipmaps>
         kGlobalDistanceFieldClipmapTextures{ RDG::TextureParameter( "GlobalDistanceField0Texture" ),
                                              RDG::TextureParameter( "GlobalDistanceField1Texture" ),
                                              RDG::TextureParameter( "GlobalDistanceField2Texture" ),
                                              RDG::TextureParameter( "GlobalDistanceField3Texture" ) };

    // THE OBJECT GRID (UE GlobalDistanceFieldPageObjectGridBuffer, GlobalDistanceFieldObjectGrid.ush): per clipmap
    // a toroidal grid of cells of two voxels, each listing up to four instance-table rows whose mesh distance
    // field surface lies within the card interpolation range of the cell, nearest first - what a software hit
    // reads to find its cards (Common/GITraceDistanceField.glslh GIHitInstanceRows). Written by the composite with
    // the distances, brick by brick. One R32_UINT 3D image: x = cell x * 4 + slot, y = cell y, z = level * cells +
    // cell z; a slot packs the row (24 bits) under the quantised distance (8 bits) so that sorting the words sorts
    // the distances, kNoObjectGridRow ends the list. The constants are DistanceFieldShared.glslh's
    // GDF_OBJECT_GRID_*.
    inline constexpr uint32_t kNoObjectGridRow = 0xFFFFFFFFu;

    // One object the field is composited from: one volume of a mesh's field placed in the world (a mesh whose
    // field has several volumes - Assets::MeshDistanceFieldData::Volumes - is that many objects).
    struct GlobalDistanceFieldObject
    {
        std::shared_ptr<const Assets::MeshDistanceFieldVolume> Field;
        glm::mat4                                            World = glm::mat4( 1.0f );
        // Where the object was last frame (its GpuScene row's PrevLocalToWorld; World when it has no GpuScene
        // primitive): a near-field hit's previous position (DF_OBJECT_ROW_TO_PREVIOUS_WORLD). Not part of the
        // object's identity - the clipmaps hold this frame's surface only.
        glm::mat4 PrevWorld = glm::mat4( 1.0f );
        // The instance-table row (SurfaceCache.hpp, contract B3) of the mesh instance this volume belongs to, or
        // kNoObjectGridRow when the surface cache holds none (no cards): what the object grid lists.
        uint32_t Row = kNoObjectGridRow;
        // The mesh is foliage (Assets::MeshDistanceFieldSlot::MostlyTwoSided, UE bMostlyTwoSided): its surface
        // marks no voxel of the field covered.
        bool MostlyTwoSided = false;
    };

    // THE SCENE'S DISTANCE-FIELD OBJECTS AS A KEPT SET (UE FDistanceFieldSceneData: the object buffer lives across
    // frames and changes only by the primitive add / update / remove operations FScene::AddPrimitive /
    // UpdatePrimitiveTransform queue). Its owner rebuilds it only when something it was made of changed
    // (SceneRenderer::AddFrameGlobalDistanceField: the GpuScene's change stream, a field landing, the surface
    // cache's placements) and then seals it; the field's update reads the seal instead of re-deriving the
    // list's identity and the objects' boxes every frame.
    // ONE LANDSCAPE TILE AS A HEIGHTFIELD OBJECT OF THE GLOBAL DISTANCE FIELD (UE
    // r.AOGlobalDistanceFieldRepresentHeightfields: a landscape component is a surface of the global field): its
    // distance is analytic from the R16 heights the raster draws (Shaders/Common/HeightfieldDistanceField.glslh),
    // so the ground is in the software trace scene without a baked volume, and a sculpt (a new HeightRevision)
    // recomposites the bricks the tile touches. The per-object near field is for meshes: a tile is never a
    // near-field cull candidate (MeshDistanceFieldReads::NumMeshObjects). Skinned meshes stay out of the software
    // scene (UE parity).
    struct GlobalDistanceFieldHeightfield
    {
        // The tile's R16_UNORM heights with the one-sample ring of its neighbours (LandscapeBorderedSamples), read
        // through the bindless table, and the revision of their pixels (LandscapeTileDraw::HeightRevision).
        std::shared_ptr<Image2D> Heightmap;
        uint64_t                 HeightRevision = 0;
        // World x, z of the tile's sample (0, 0) (root origin + first global sample x spacing), cm.
        glm::vec2 SampleOrigin{ 0.0f };
        float     SpacingCm    = 0.0f;
        float     BaseY        = 0.0f; // the root's base height, world cm
        float     ZScale       = 0.0f; // the landscape's vertical scale
        uint32_t  QuadsPerTile = 0;
        // The world box: xz the tile, y its lowest and highest sample (LandscapeTileDraw::MinY / MaxY).
        Common::Math::AABB Bounds;
        // The instance-table row of the tile's heightfield cards (kNoObjectGridRow while it has none).
        uint32_t Row = kNoObjectGridRow;
    };

    struct GlobalDistanceFieldObjectSet
    {
        std::vector<GlobalDistanceFieldObject> Objects;
        // Each object's world box (its field's LocalSpaceMeshBounds through World), index for index with Objects.
        std::vector<Common::Math::AABB> Bounds;
        // The set's distinct fields, first use first, and each object's index into them, index for index with
        // Objects (sealed): the scene's residency and the frame's lookups run once per field (UE
        // FDistanceFieldAssetState: per asset, not per instance), and an object reads its field's answer.
        std::vector<std::shared_ptr<const Assets::MeshDistanceFieldVolume>> Fields;
        std::vector<uint32_t>                                               FieldOfObject;
        // The landscape tiles, placed after the objects in the object records (DF_OBJECT_FLAG_HEIGHTFIELD): the
        // global field's composite reads them, the near field's cull stops before them.
        std::vector<GlobalDistanceFieldHeightfield> Heightfields;
        // The list's identity (field, transform, foliage flag and row of each, in order, then every heightfield's
        // heights, revision, frame and row): what the once-per-frame rule compares and what the field's kept
        // placement is keyed on. Not PrevWorld: the clipmaps hold this frame's surface only.
        uint64_t Identity = 0;
    };
    // Fills @p set's Bounds, Fields, FieldOfObject and Identity from its Objects; call after every rebuild of the
    // Objects.
    void SealGlobalDistanceFieldObjectSet( GlobalDistanceFieldObjectSet& set );

    // The parameters of a clipmap level (pure, shared by the update and every reader): its voxel in centimetres,
    // and the centre of its window in update bricks of that level.
    [[nodiscard]] float      ClipmapVoxelSize( int level );
    [[nodiscard]] glm::ivec3 ClipmapCentreBrick( const glm::dvec3& camera, float voxelSize );

    // THE INCREMENTAL UPDATE OF ONE LEVEL (pure): the bricks of the window around @p centre that must be
    // recomposited - all of them without a previous window or after a jump of a window or more, otherwise the
    // ones that entered it plus the ones @p dirty (world boxes, already grown by the influence radius) touch.
    [[nodiscard]] std::vector<glm::ivec3> PlanClipmapUpdate( const std::optional<glm::ivec3>& previousCentre,
                                                             const glm::ivec3& centre, float voxelSize,
                                                             const std::vector<Common::Math::AABB>& dirty );

    // THE FIELD: the clipmaps, the object grid and what they were composited from - one per camera that owns a
    // field (the scene view's own; the six face views of the scene's reflection captures share ONE,
    // ReflectionCaptureRenderer's: they stand at one capture point). Updated AT MOST ONCE PER ENGINE FRAME: the
    // first AddUpdatePasses of a frame plans, uploads and records the composites into its graph; a later one in the
    // same frame for the same camera and objects records nothing and hands the same field to its own graph; a
    // later one for another camera or object set is refused - the object, brick and brick-list buffers are
    // per-frame copies written by the CPU, and a second write in the frame would change what the frame's earlier
    // graph reads. What a view does with the field (its near-field grid, its debug view) is GlobalDistanceFieldView.
    // The Composite shader's parameters: the one spelling the pass binds and Initialize checks.
    struct GlobalDistanceFieldCompositeShaderParameters
    {
        DESERT_SHADER_PARAMETER( Texture, DistanceFieldBrickAtlasTexture );
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        DESERT_SHADER_PARAMETER( StorageTexture, ClipmapTexture );
        DESERT_SHADER_PARAMETER( StorageTexture, ObjectGridTexture );
        static constexpr std::array kAll{ RDG::Erase( DistanceFieldBrickAtlasTexture ), RDG::Erase( LinearRepeatSampler ), RDG::Erase( ClipmapTexture ), RDG::Erase( ObjectGridTexture ) };
    };

    // The View shader's parameters: the one spelling the pass binds and Initialize checks.
    struct GlobalDistanceFieldViewShaderParameters
    {
        DESERT_SHADER_PARAMETER( Sampler, LinearRepeatSampler );
        static constexpr std::array kAll{ RDG::Erase( LinearRepeatSampler ) };
    };

    class GlobalDistanceField
    {
    public:
        // @p owner: the ledger instance whose rows the field's GPU objects are - the view's own (SceneRenderer's
        // m_LedgerView) or, for a reflection capture's shared field, the scene's (Scene::GetLedgerInstance).
        // Initialize and AddUpdatePasses open its scope: they run inside whichever view's scope asked first
        // (VIEW-CLOSE-LEAK).
        explicit GlobalDistanceField( ResourceOwnerInstance owner );
        ~GlobalDistanceField();
        GlobalDistanceField( const GlobalDistanceField& )            = delete;
        GlobalDistanceField& operator=( const GlobalDistanceField& ) = delete;

        // The composite's shader and pipeline, the clipmap volumes and the per-frame buffers. An error names what
        // is missing.
        [[nodiscard]] Common::BoolResultStr Initialize();
        [[nodiscard]] bool                  IsInitialized() const
        {
            return m_CompositePipeline != nullptr;
        }

        using ImportImage = std::function<RDG::TextureRef( const std::shared_ptr<Image>&, std::string_view )>;

        // What a reader of this frame's field binds: the clipmaps as graph textures and their parameters,
        // (centre.xyz in cm, voxel size in cm) per level - GlobalDistanceField.glslh's GDFLevel - and the mesh
        // fields it was composited from (AddUpdatePasses fills the brick atlas and the object count; the view's
        // GlobalDistanceFieldView::AddFramePasses registers the buffers in its graph and adds its near-field froxel
        // grid, a GI ray's first metres).
        struct FrameField
        {
            std::array<RDG::TextureRef, kGlobalDistanceFieldClipmaps> Clipmaps;
            std::array<glm::vec4, kGlobalDistanceFieldClipmaps>       Levels{};
            // THE OCCUPIED BOX (FPS-PROBES-FAR): (min.xyz, 0), (max.xyz, 0) cm - the union of every placed
            // record's box (the meshes and the landscape tiles: all the clipmaps were composited from), grown by
            // kGdfOccupiedMarginVoxels of the coarsest clipmap. Outside it no clipmap sample can be in a march's
            // band, so a far-field march ends where its ray leaves it (Common/GITraceDistanceField.glslh
            // GIFarFieldOccupiedExit). Min > Max: nothing is placed, no ray can hit.
            std::array<glm::vec4, 2>                                  OccupiedBox{};
            RDG::TextureRef                                           ObjectGrid; // the object grid (above)
            MeshDistanceFieldReads                                    MeshFields;
        };

        // FPS-DEVLOST18: where the asking view keeps its graph's imports of the buffers the composite and a trace
        // read - the scene's asset records and indirection, the field's objects (GlobalDistanceFieldView's
        // m_AssetImport / m_IndirectionImport / m_ObjectImport). None may be null.
        struct MeshFieldImports
        {
            RDG::ExternalBuffer* Assets      = nullptr;
            RDG::ExternalBuffer* Indirection = nullptr;
            RDG::ExternalBuffer* Objects     = nullptr;
        };

        // Brings the clipmaps up to @p camera and @p objects, once per @p engineFrame (the class comment). The
        // mesh fields are @p scene's - the one DistanceFieldSceneData of the Core::Scene every view of it shares:
        // the frame's update starts its frame there (@p poolMiB: Scalability
        // GlobalIllumination.DistanceFieldPoolMiB), looks its objects' fields up in the set the scene decided; then
        // its update plan and one composite pass per level that has bricks to rewrite. Every call imports the
        // clipmaps, the object grid and the brick atlas into @p graph through @p import, and registers the asset,
        // indirection and object buffers through @p imports (FrameField::MeshFields) once this frame's objects
        // are uploaded - BEFORE the composites, which declare their reads of all three (they run on the async
        // queue: an undeclared read gets neither a barrier nor a cross-queue join).
        [[nodiscard]] Common::ResultStr<FrameField>
        AddUpdatePasses( RDG::Builder& graph, const ImportImage& import, const glm::vec3& camera,
                         const GlobalDistanceFieldObjectSet& objects, DistanceFieldSceneData& scene,
                         uint64_t engineFrame, uint32_t poolMiB, const MeshFieldImports& imports );

        // The object records of the frame's update (DF_OBJECT_*): what a view's graph registers as
        // MeshDistanceFieldReads::Objects.
        [[nodiscard]] const std::shared_ptr<ShaderResources::StorageBuffer>& GetObjectBuffer() const
        {
            return m_ObjectBuffer;
        }

    private:
        // FPS-DEVLOST18: imports @p scene's asset and indirection buffers and this field's objects (each frame in
        // flight has its own copy: called after this frame's uploads) through @p imports and registers them in
        // @p graph as @p into's Assets / Indirection / Objects.
        [[nodiscard]] Common::BoolResultStr RegisterMeshFields( RDG::Builder& graph, const DistanceFieldSceneData& scene,
                                                                const MeshFieldImports& imports,
                                                                MeshDistanceFieldReads& into ) const;

        struct TrackedObject
        {
            Common::Math::AABB Bounds;
            uint32_t           Count = 0;
        };

        ResourceOwnerInstance                                               m_LedgerInstance; // see the constructor
        std::array<std::shared_ptr<Image3D>, kGlobalDistanceFieldClipmaps>  m_Clipmaps;
        std::shared_ptr<Image3D>                                            m_ObjectGrid;
        std::array<std::optional<glm::ivec3>, kGlobalDistanceFieldClipmaps> m_ValidCentre;
        // The objects the clipmaps hold, by identity (field and transform): a difference is a dirty box.
        std::unordered_map<uint64_t, TrackedObject> m_Tracked;
        // THE KEPT PLACEMENT (UE: the object buffer is rewritten for the changed objects only): what the last
        // full placement made of the object set - the fields BeginViewFrame is handed, the placed objects' boxes
        // and their DF_OBJECT_* rows - and what it was made from: the set's identity, the scene's record revision
        // and pool generation, and which objects the frame's resident set held (a hash of every Lookup's answer).
        // The same four again is the same placement: the rows are uploaded as they are, nothing is re-derived, and
        // no object is dirty. The Lookups still run every frame: they are the streaming's distance feedback.
        struct KeptPlacement
        {
            uint64_t                        SetIdentity   = 0;
            uint64_t                        SceneRevision = 0;
            uint64_t                        PoolGeneration = 0;
            uint64_t                        Lookups       = 0;
            std::vector<Common::Math::AABB> PlacedBounds;
            std::array<glm::vec4, 2>        OccupiedBox{}; // FrameField::OccupiedBox, made from PlacedBounds
            std::vector<glm::vec4>          ObjectData;
            // The placed mesh objects, which lead PlacedBounds and ObjectData; the landscape tiles follow them
            // (the global field's only - MeshDistanceFieldReads::NumMeshObjects).
            uint32_t MeshObjects = 0;
        };
        std::optional<KeptPlacement> m_Placement;
        // The resident slot of each of the set's Fields (DistanceFieldSceneData::ResolveFields), resolved for the
        // set m_SlotsIdentity names at the scene's resident epoch m_SlotsEpoch: re-resolved only when either
        // moved, so a frame's lookups read an array. The set holds the fields, so a slot cannot outlive its field.
        std::vector<DistanceFieldSceneData::FieldSlot> m_FieldSlots;
        std::optional<uint64_t>                        m_SlotsIdentity;
        uint64_t                                       m_SlotsEpoch = 0;
        // The frame's scratch, kept so a frame allocates nothing: the nearest object distance of each field, each
        // field's record index (nullopt: not resident) and the placed objects (object, record index).
        std::vector<float>                         m_FieldDistance;
        std::vector<std::optional<uint32_t>>       m_FieldIndex;
        std::vector<std::pair<uint32_t, uint32_t>> m_Placed;
        // The scene pool generation the clipmaps were composited from (DistanceFieldSceneData::GetPoolGeneration).
        std::optional<uint64_t> m_PoolGeneration;

        // THE FRAME'S UPDATE (once per frame): the engine frame it ran in, what it was asked for (the camera and
        // the identity of the object list: field, transform, foliage flag and row of each, in order), and what
        // a later call of that frame hands back without recording anything.
        struct FrameUpdate
        {
            uint64_t                                            Frame = 0;
            glm::vec3                                           Camera{ 0.0f };
            uint64_t                                            Objects = 0;
            std::array<glm::vec4, kGlobalDistanceFieldClipmaps> Levels{};
            std::array<glm::vec4, 2>                            OccupiedBox{};
            uint32_t                                            NumMeshObjects = 0;
            uint32_t                                            NumObjects     = 0;
        };
        std::optional<FrameUpdate> m_FrameUpdate;

        std::shared_ptr<Shader>          m_CompositeShader;
        std::shared_ptr<ComputePipeline> m_CompositePipeline;
        mutable ShaderBindingLayoutCache m_CompositeLayout;

        // Per-frame copies (one per frame in flight), written by the frame's update only.
        std::shared_ptr<ShaderResources::StorageBuffer> m_ObjectBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_UpdateBrickBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_BrickObjectBuffer;
    };

    // THE VIEW OF A FIELD: what one view does with the field it reads (its own, or one shared with other views at
    // its camera) - registers the field's buffers in its graph, culls the near field's froxel grid to its frustum,
    // draws the debug view. One per view: the near field's grid and feedback follow this view's frustum and frame
    // slots, and no other view's.
    class GlobalDistanceFieldView
    {
    public:
        GlobalDistanceFieldView();
        ~GlobalDistanceFieldView();
        GlobalDistanceFieldView( const GlobalDistanceFieldView& )            = delete;
        GlobalDistanceFieldView& operator=( const GlobalDistanceFieldView& ) = delete;

        // The near-field cull's and the debug view's shaders and pipelines. An error names what is missing.
        [[nodiscard]] Common::BoolResultStr Initialize();

        // The view a near-field grid is culled for: its unjittered matrices, its camera, its extent in pixels.
        struct NearFieldView
        {
            glm::mat4  ViewProjection{ 1.0f };
            glm::mat4  InvViewProjection{ 1.0f };
            glm::mat4  Projection{ 1.0f };
            glm::vec3  CameraPosition{ 0.0f };
            glm::vec3  Forward{ 0.0f, 0.0f, -1.0f }; // world, unit
            glm::uvec2 Extent{ 0u };
        };

        // This view's frame of @p field: GlobalDistanceField::AddUpdatePasses at @p view's camera (once per frame
        // for the field, whichever view asks first), the scene's asset and indirection buffers and the field's
        // objects registered in @p graph, then this view's near-field grid (AddNearFieldCullPasses).
        [[nodiscard]] Common::ResultStr<GlobalDistanceField::FrameField>
        AddFramePasses( RDG::Builder& graph, GlobalDistanceField& field,
                        const GlobalDistanceField::ImportImage&       import,
                        const GlobalDistanceFieldObjectSet& objects, DistanceFieldSceneData& scene,
                        uint64_t engineFrame, uint32_t poolMiB, const NearFieldView& view );

        // "Debug: Global Distance Field" over @p target (a full-screen sphere trace of the clipmaps).
        [[nodiscard]] Common::BoolResultStr AddDebugViewPass( RDG::Builder&                          graph,
                                                              const GlobalDistanceField::FrameField& field,
                                                              RDG::TextureRef target, const RDG::LoadOp& load,
                                                              const glm::mat4& inverseViewProjection );

    private:
        // THE NEAR FIELD'S FROXEL GRID (UE LumenMeshSDFCulling: CullMeshSDFObjectsForView,
        // CullMeshSDFObjectsToViewGrid, ComputeCulledObjectsStartOffset, CompactCulledObjects - written from
        // memory): five compute passes over @p field's objects - the ones near @p view's frustum; per froxel of
        // Common/MeshSDFGrid.glslh the count of those within DF_MESH_SDF_TRACE_DISTANCE of it; the counts' prefix
        // sum; the objects scattered into the froxels' compacted lists - every object, no per-cell cap; each list
        // sorted closest to its froxel first (MeshSDFCullGridSort.shader: what lets a near hit clip the rest). The
        // lists' room is m_NearFieldListRoom, grown from what earlier frames needed (ReadNearFieldFeedback); a
        // frame that needs more is refused on the GPU (every list empty) and logged. Fills @p field.MeshFields'
        // NearFieldGrid and NearFieldGridPlacement. An error names the bad input.
        [[nodiscard]] Common::BoolResultStr AddNearFieldCullPasses( RDG::Builder&                    graph,
                                                                    GlobalDistanceField::FrameField& field,
                                                                    const NearFieldView&             view );

        [[nodiscard]] Common::BoolResultStr PrepareDebugPipeline( Core::Formats::ImageFormat format,
                                                                  uint32_t                   samples );

        // This view's graph's view of the buffers a trace reads - the scene's records and table, the field's
        // objects (re-imported every frame: each frame in flight has its own copy).
        RDG::ExternalBuffer m_AssetImport;
        RDG::ExternalBuffer m_IndirectionImport;
        RDG::ExternalBuffer m_ObjectImport;

        std::shared_ptr<Shader>                                               m_ViewShader;
        mutable ShaderBindingLayoutCache                                      m_ViewLayout;
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_ViewPipelines;
        std::shared_ptr<GraphicsPipeline>                                     m_ViewPipeline;

        // The near field's cull (AddNearFieldCullPasses).
        std::shared_ptr<Shader>          m_CullObjectsShader;
        std::shared_ptr<ComputePipeline> m_CullObjectsPipeline;
        mutable ShaderBindingLayoutCache m_CullObjectsLayout;
        std::shared_ptr<Shader>          m_CullGridShader;
        std::shared_ptr<ComputePipeline> m_CullGridPipeline;
        mutable ShaderBindingLayoutCache m_CullGridLayout;
        std::shared_ptr<Shader>          m_CullOffsetsShader;
        std::shared_ptr<ComputePipeline> m_CullOffsetsPipeline;
        mutable ShaderBindingLayoutCache m_CullOffsetsLayout;
        std::shared_ptr<Shader>          m_CullSortShader;
        std::shared_ptr<ComputePipeline> m_CullSortPipeline;
        mutable ShaderBindingLayoutCache m_CullSortLayout;

        // What the cull of frame slot @p slot handed back (MeshSDFCullFeedbackUints of Common/MeshSDFGrid.glslh),
        // read once that slot comes round again (its fence waited): grows m_NearFieldListRoom to what the lists
        // needed and logs a refused frame. A readback of another cull than the slot's last (its stamp) is ignored.
        void ReadNearFieldFeedback( uint32_t slot );

        // The list entries the near-field grid buffer has room for, grown, never cut, to what a frame needed.
        uint64_t m_NearFieldListRoom = 0;
        // Per frame slot: the HostRead buffer the cull extracted into and the stamp of that cull (0 = none).
        std::vector<RDG::ExternalBuffer> m_NearFieldFeedback;
        std::vector<uint32_t>            m_NearFieldFeedbackStamp;
        uint32_t                         m_NearFieldStamp          = 0;
        bool                             m_NearFieldFeedbackLogged = false; // the first readback is logged once
        uint32_t                         m_NearFieldRefusedRoom    = 0;     // the room of the last refusal logged
    };
} // namespace Desert::Graphic::DistanceField
