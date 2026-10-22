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
        // Where the view drew the object last frame (its MotionHistory transform; World when it has no motion
        // record): a near-field hit's previous position (DF_OBJECT_ROW_TO_PREVIOUS_WORLD). Not part of the
        // object's identity - the clipmaps hold this frame's surface only.
        glm::mat4 PrevWorld = glm::mat4( 1.0f );
        // The instance-table row (SurfaceCache.hpp, contract B3) of the mesh instance this volume belongs to, or
        // kNoObjectGridRow when the surface cache holds none (no cards): what the object grid lists.
        uint32_t Row = kNoObjectGridRow;
        // The mesh is foliage (Assets::MeshDistanceFieldSlot::MostlyTwoSided, UE bMostlyTwoSided): its surface
        // marks no voxel of the field covered.
        bool MostlyTwoSided = false;
    };

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
            RDG::TextureRef                                           ObjectGrid; // the object grid (above)
            MeshDistanceFieldReads                                    MeshFields;
        };

        // Brings the clipmaps up to @p camera and @p objects, once per @p engineFrame (the class comment). The
        // mesh fields are @p scene's - the one DistanceFieldSceneData of the Core::Scene every view of it shares:
        // the frame's update starts its frame there (@p poolMiB: Scalability
        // GlobalIllumination.DistanceFieldPoolMiB), looks its objects' fields up in the set the scene decided; then
        // its update plan and one composite pass per level that has bricks to rewrite. Every call imports the
        // clipmaps, the object grid and the brick atlas into @p graph through @p import.
        [[nodiscard]] Common::ResultStr<FrameField>
        AddUpdatePasses( RDG::Builder& graph, const ImportImage& import, const glm::vec3& camera,
                         const std::vector<GlobalDistanceFieldObject>& objects, DistanceFieldSceneData& scene,
                         uint64_t engineFrame, uint32_t poolMiB );

        // The object records of the frame's update (DF_OBJECT_*): what a view's graph registers as
        // MeshDistanceFieldReads::Objects.
        [[nodiscard]] const std::shared_ptr<ShaderResources::StorageBuffer>& GetObjectBuffer() const
        {
            return m_ObjectBuffer;
        }

    private:
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
            uint32_t                                            NumObjects = 0;
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
                        const std::vector<GlobalDistanceFieldObject>& objects, DistanceFieldSceneData& scene,
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
