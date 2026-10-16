#pragma once

// THE GLOBAL DISTANCE FIELD (UE GlobalDistanceField.cpp / GlobalDistanceField.h): the scene's mesh distance fields
// composited into four camera-centred clipmaps of 128^3 voxels (Lumen's extents: 25 m half-size for the first,
// doubling per level), each the distance in centimetres to the nearest surface clamped to four of its voxels.
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
#include <Engine/Core/Frustum.hpp>
#include <Engine/Graphic/DistanceField/DistanceFieldSceneData.hpp>
#include <Engine/Core/Formats/ImageFormat.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
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
        // The instance-table row (SurfaceCache.hpp, contract B3) of the mesh instance this volume belongs to, or
        // kNoObjectGridRow when the surface cache holds none (no cards): what the object grid lists.
        uint32_t Row = kNoObjectGridRow;
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

    class GlobalDistanceField
    {
    public:
        GlobalDistanceField();
        ~GlobalDistanceField();
        GlobalDistanceField( const GlobalDistanceField& )            = delete;
        GlobalDistanceField& operator=( const GlobalDistanceField& ) = delete;

        // Shaders, pipelines, clipmap volumes and buffers. An error names what is missing.
        [[nodiscard]] Common::BoolResultStr Initialize();

        using ImportImage = std::function<RDG::TextureRef( const std::shared_ptr<Image>&, std::string_view )>;

        // What a reader of this frame's field binds: the clipmaps as graph textures and their parameters,
        // (centre.xyz in cm, voxel size in cm) per level - GlobalDistanceField.glslh's GDFLevel.
        struct FrameField
        {
            std::array<RDG::TextureRef, kGlobalDistanceFieldClipmaps> Clipmaps;
            std::array<glm::vec4, kGlobalDistanceFieldClipmaps>       Levels{};
            RDG::TextureRef                                           ObjectGrid; // the object grid (above)
        };

        // Brings the clipmaps up to @p camera and @p objects: residency and mip streaming in the brick atlas of
        // @p poolMiB (Scalability GlobalIllumination.DistanceFieldPoolMiB), the update plan, and one composite
        // pass per level that has bricks to rewrite.
        [[nodiscard]] Common::ResultStr<FrameField>
        AddUpdatePasses( RDG::Builder& graph, const ImportImage& import, const glm::vec3& camera,
                         const std::vector<GlobalDistanceFieldObject>& objects, uint32_t poolMiB );

        // THE MESH SDFS AS A NEAR-FIELD TRACE READS THEM (UE: the distance field object buffers + the view cull of
        // LumenMeshSDFCulling.usf, CullMeshSDFObjectsForViewCS): this frame's objects - the field's own records,
        // in the order AddUpdatePasses composited them - uploaded for the trace, beside the list of the objects a
        // trace from the view's surfaces can reach. Bindings by name (Common/GITraceMeshSDF.glslh).
        struct MeshSDFScene
        {
            RDG::TextureRef BrickAtlas;  // "u_DFBrickAtlas"
            RDG::BufferRef  AssetData;   // "DFAssetData"
            RDG::BufferRef  Indirection; // "DFIndirection"
            RDG::BufferRef  Objects;     // "DFObjects"
            RDG::BufferRef  ObjectRows;  // "GIMeshSDFObjectRowsUB": per object its instance-table row
            // "GIMeshSDFCulledUB": uvec4(count, 0, 0, 0), then per culled object two vec4: its world box min with
            // the object index's bits in w, its world box max.
            RDG::BufferRef Culled;
        };
        // Which objects the near field may meet: the box grown by @p traceDistance (cm) inside @p view and nearer
        // @p camera than @p endDistanceFromCamera + @p traceDistance (UE CardTraceEndDistanceFromCamera: a probe
        // farther than that traces no mesh SDF).
        struct MeshSDFCull
        {
            Core::Frustum View;
            glm::vec3     Camera{ 0.0f };
            float         TraceDistance         = 0.0f;
            float         EndDistanceFromCamera = 0.0f;
        };
        // After AddUpdatePasses of the same frame. An error when the field holds no frame yet. The records live in
        // device buffers of the field's own, uploaded by the graph only in a frame where they changed
        // (DistanceFieldSceneData::GetRevision, the objects' list); the cull is this frame's.
        [[nodiscard]] Common::ResultStr<MeshSDFScene>
        AddMeshSDFReads( RDG::Builder& graph, const ImportImage& import, const MeshSDFCull& cull );

        // "Debug: Global Distance Field" over @p target (a full-screen sphere trace of the clipmaps).
        [[nodiscard]] Common::BoolResultStr AddDebugViewPass( RDG::Builder& graph, const FrameField& field,
                                                              RDG::TextureRef target, const RDG::LoadOp& load,
                                                              const glm::mat4& inverseViewProjection );

        [[nodiscard]] const DistanceFieldSceneData& GetSceneData() const
        {
            return m_SceneData;
        }

    private:
        struct TrackedObject
        {
            Common::Math::AABB Bounds;
            uint32_t           Count = 0;
        };

        // One line when the streaming settles (no field waits for a finer mip) after it moved.
        void LogStreaming();

        [[nodiscard]] Common::BoolResultStr PrepareDebugPipeline( Core::Formats::ImageFormat format,
                                                                  uint32_t                   samples );

        DistanceFieldSceneData                                              m_SceneData;
        // The last AddUpdatePasses' objects (AddMeshSDFReads): the object records, their rows, their world boxes.
        std::vector<glm::vec4>                                              m_FrameObjectData;
        std::vector<uint32_t>                                               m_FrameObjectRows;
        std::vector<Common::Math::AABB>                                     m_FrameObjectBounds;
        uint64_t m_FrameObjectsRevision = 0; // bumped when the objects or their rows changed
        bool     m_HasFrame             = false;
        // A device copy of one record list for the mesh SDF readers: one persistent buffer, grown as needed,
        // written by a graph upload when the list's revision moved.
        struct DeviceRecords
        {
            std::shared_ptr<ShaderResources::StorageBuffer> Buffer;
            RDG::ExternalBuffer                             Import;
            uint64_t                                        Revision = ~0ull;
        };
        DeviceRecords                                                       m_MeshSDFAssetData;
        DeviceRecords                                                       m_MeshSDFIndirection;
        DeviceRecords                                                       m_MeshSDFObjects;
        DeviceRecords                                                       m_MeshSDFObjectRows;
        std::array<std::shared_ptr<Image3D>, kGlobalDistanceFieldClipmaps>  m_Clipmaps;
        std::shared_ptr<Image3D>                                            m_ObjectGrid;
        std::array<std::optional<glm::ivec3>, kGlobalDistanceFieldClipmaps> m_ValidCentre;
        // The objects the clipmaps hold, by identity (field and transform): a difference is a dirty box.
        std::unordered_map<uint64_t, TrackedObject> m_Tracked;
        bool                                        m_StreamingMoved = false;

        std::shared_ptr<Shader>                                               m_CompositeShader;
        std::shared_ptr<ComputePipeline>                                      m_CompositePipeline;
        mutable ShaderBindingLayoutCache                                      m_CompositeLayout;
        std::shared_ptr<Shader>                                               m_ViewShader;
        mutable ShaderBindingLayoutCache                                      m_ViewLayout;
        std::map<std::pair<int, uint32_t>, std::shared_ptr<GraphicsPipeline>> m_ViewPipelines;
        std::shared_ptr<GraphicsPipeline>                                     m_ViewPipeline;

        std::shared_ptr<ShaderResources::StorageBuffer> m_AssetBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_IndirectionBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_ObjectBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_UpdateBrickBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_BrickObjectBuffer;
        std::shared_ptr<ShaderResources::StorageBuffer> m_ObjectRowBuffer;
    };
} // namespace Desert::Graphic::DistanceField
