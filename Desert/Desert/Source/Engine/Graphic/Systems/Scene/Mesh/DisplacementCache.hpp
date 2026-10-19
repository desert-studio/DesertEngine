#pragma once

// THE SCENE'S DISPLACED GEOMETRY (contract CONTRACT-DISPLACE §1d, decisions §9). UE has no counterpart: Nanite
// tessellates per view every frame and its ray-tracing stream-out does not see displacement at all. Here a
// displaced primitive's geometry is written ONCE per change of its levels or its height inputs, and every raster
// view (camera, cascades, VSM, velocity) and its BLAS read the same buffers - the primitive is drawn through a
// displaced TWIN mesh (StaticMesh::MakeDisplacedTwin) that every draw path takes like any static mesh.
//
// Per frame (main thread, one cache per Core::Scene):
//   1. every CAMERA view, after its queues are built, Request()s each static record whose mesh carries a
//      displacement topology: per cluster, the level whose diced edge covers the dicing rate in pixels at the
//      cluster's nearest point (no frustum test: shadows, reflections and rays see what the camera asked for);
//   2. the first view of frame N calls DisplaceFrame(N): levels(N) = the highest request of frame N-1 per cluster
//      (the first frame of an entry: its first request), tessellation of changed entries on workers
//      (TessellateCluster, exact counts), and for every finished one ONE graph of "Displacement: Heights" +
//      "Displacement: Emit" that writes its twin's vertex buffer, executed at once (SkinFrame's rule) - later
//      view graphs only read;
//   3. raster: Find() answers the twin a record draws instead of its mesh (null: not displaced yet, draw the
//   base);
//   4. EndFrame(N) drops the entries no view asked for.
// Geometry is SHARED: entries whose (mesh, per-submesh material records, cluster levels) are equal hold one twin -
// instances and ISM members at the same levels cost one buffer set and one BLAS.

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Formats/MaterialLayout.hpp>
#include <Engine/Core/Formats/MaterialParamRow.hpp>
#include <Engine/Core/Formats/ShaderProgramMeta.hpp>
#include <Engine/Geometry/DisplacementTessellation.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert
{
    class StaticMesh;
}
namespace Desert::Graphic
{
    class ComputePipeline;
}

namespace Desert::Graphic::System
{
    inline constexpr std::string_view kDisplacementHeightsShader   = "DisplacementHeights";
    inline constexpr std::string_view kDisplacementEmitShader      = "DisplacementEmit";
    inline constexpr std::string_view kDisplacementHeightsPassName = "Displacement: Heights";
    inline constexpr std::string_view kDisplacementEmitPassName    = "Displacement: Emit";
    inline constexpr uint32_t         kDisplacementGroupSize       = 64; // LocalSize of both programs

    // The Scalability rows' values (ViewDistance.DisplacementDicingRate / DisplacementPoolMiB; Low turns runtime
    // displacement off and the surfaces keep their parallax - §9.10), set by SceneRenderer::BeginScene.
    struct DisplacementQuality
    {
        bool     Enabled                                        = true;
        float    DicingRatePixels                               = 2.0f; // UE r.Nanite.DicingRate
        uint64_t PoolBytes                                      = 0;
        bool     operator==( const DisplacementQuality& ) const = default;
    };

    // One submesh's height inputs, the SurfaceHeight values (ShaderProgramMeta.hpp) read from its effective
    // material row. 12 words, the layout DisplacementHeights.shader reads.
    struct DisplacementMaterialRecord
    {
        uint32_t  Texture = 0xffffffffu; // bindless slot; ~0 = this submesh has no height (moves by 0)
        uint32_t  Sampler = 0;
        uint32_t  Channel = 0;
        uint32_t  UVSet   = 0;
        float     ScaleCm = 0.0f;
        float     Center  = 0.0f;
        glm::vec2 Tiling{ 1.0f };
        glm::vec2 Offset{ 0.0f };
        float     Rotation                                              = 0.0f;
        uint32_t  HasTransform                                          = 0;
        bool      operator==( const DisplacementMaterialRecord& ) const = default;
    };
    static_assert( sizeof( DisplacementMaterialRecord ) == 48 );

    // The record of a material whose template declares a Height (nullopt: it does not, or @p heightAssigned is false -
    // the material assigns no height texture, the same predicate that picks the template's `.Parallax` cells - so the
    // submesh is not displaced). Errors name the parameter
    // the Height line declares and the layout does not hold.
    [[nodiscard]] Common::ResultStr<std::optional<DisplacementMaterialRecord>>
    ResolveDisplacementMaterial( const Core::Formats::ShaderProgramMeta& schema,
                                 const Core::Formats::MaterialLayout&    layout,
                                 const Core::Formats::MaterialParamRow&  row, bool heightAssigned );

    struct DisplacementStats
    {
        uint32_t Entries       = 0; // primitives asked for this frame
        uint32_t Geometries    = 0; // distinct twins alive (shared between equal entries)
        uint32_t Displaced     = 0; // entries drawing a twin
        uint32_t Rebuilt       = 0; // twins emitted this frame
        uint32_t Pending       = 0; // entries whose tessellation is still on a worker
        uint32_t LevelsLowered = 0; // cluster levels lowered this frame to fit the pool (§5)
        uint32_t LevelsRaised  = 0; // lowerings undone this frame: the pool holds the higher levels again
        uint32_t LevelsHeld    = 0; // lowering steps still in force over all entries (overlay, [ViewMemory])
        uint32_t LevelsClamped = 0; // cluster requests above the fixed-point resolution limit (MaxLevel)
        uint64_t BufferBytes   = 0; // twins' vertex + stream + index buffers
        uint64_t TopologyBytes = 0; // the meshes' topologies the live entries read
    };

    class DisplacementCache
    {
    public:
        DisplacementCache();
        ~DisplacementCache();
        DisplacementCache( const DisplacementCache& )            = delete;
        DisplacementCache& operator=( const DisplacementCache& ) = delete;

        // Off releases every twin: nothing keeps memory for geometry nobody draws.
        void                                     SetQuality( const DisplacementQuality& quality );
        [[nodiscard]] const DisplacementQuality& GetQuality() const
        {
            return m_Quality;
        }

        // 1. A camera view's request for one static record (entity, part = the record's MotionKey slot) of @p mesh
        //    (which carries a topology) drawn at @p objectToWorld with @p materials (one record per submesh,
        //    nullopt = not displaced). @p viewHeightPixels converts the screen fraction into pixels.
        void Request( uint32_t entity, uint32_t part, const StaticMesh& mesh,
                      const glm::mat4&                                           objectToWorld,
                      std::span<const std::optional<DisplacementMaterialRecord>> materials,
                      const Desert::Geometry::LODView& view, float viewHeightPixels, uint64_t frame );

        // 2. The first view of @p frame, before its graph.
        [[nodiscard]] Common::BoolResultStr DisplaceFrame( uint64_t frame );

        // 3. The twin to draw instead of the record's mesh; null = draw the base mesh.
        [[nodiscard]] std::shared_ptr<StaticMesh> Find( uint32_t entity, uint32_t part ) const;

        // 4. Drops the entries no view asked for in @p frame.
        void EndFrame( uint64_t frame );

        [[nodiscard]] DisplacementStats GetStats() const
        {
            return m_Stats;
        }

    private:
        struct BuildKey
        {
            const StaticMesh*                       Mesh = nullptr;
            // The topology identifies the mesh for sharing: held by the entries, its address cannot be reused.
            const Desert::Geometry::DisplacementTopology* Topology = nullptr;
            std::vector<uint8_t>                    Levels;    // one per cluster
            std::vector<DisplacementMaterialRecord> Materials; // one per submesh
            bool                                    operator==( const BuildKey& ) const = default;
        };

        // CPU tessellation of one BuildKey, gathered over the clusters in submesh order.
        struct Tessellation
        {
            std::vector<Desert::Geometry::DisplacementPoint>  Points;
            std::vector<Desert::Geometry::DisplacementSample> Samples;
            std::vector<Desert::Geometry::DisplacementVertex> Vertices;
            std::vector<uint32_t>                     Indices; // entry-global vertex indices
            std::vector<uint32_t>                     SubmeshFirstIndex, SubmeshIndexCount;
        };

        // One displaced geometry, shared by every entry whose key equals Key.
        struct SharedGeometry
        {
            BuildKey                    Key;
            std::shared_ptr<StaticMesh> Twin;
            uint64_t                    Bytes = 0;
        };

        struct Entry
        {
            const StaticMesh* Mesh = nullptr; // the base, alive while a view submits it (requested this frame)
            std::shared_ptr<const Desert::Geometry::DisplacementTopology> Topology;
            std::vector<uint8_t>                         Asked; // highest request per cluster, this frame
            std::vector<uint8_t>                         Using; // the levels of N (last frame's Asked)
            float                                        SmallestScreen = 0.0f; // pool-fit order (§5)
            std::vector<DisplacementMaterialRecord>      Materials;
            std::shared_ptr<SharedGeometry>                    Current;
            std::optional<BuildKey>                      PendingKey;
            std::future<Common::ResultStr<Tessellation>> Pending;
            // Levels taken off every cluster above 0 to fit the pool (§5). Each step remembers the EXACT bytes the
            // entry held one step higher and the levels that size was measured at: the step is undone as soon as
            // the pool holds that size again, or when the levels changed (the measurement no longer applies, so
            // the higher key is tessellated and fitted anew).
            uint8_t                           Lowered = 0;
            std::vector<uint64_t>             RaiseBytes;
            std::vector<std::vector<uint8_t>> RaiseLevels;
            uint64_t                                     LastFrame     = 0;
            bool                                         RefusalLogged = false;
        };

        [[nodiscard]] bool                                         EnsurePipelines();
        [[nodiscard]] std::shared_ptr<SharedGeometry>                    FindShared( const BuildKey& key ) const;
        [[nodiscard]] Common::ResultStr<std::shared_ptr<SharedGeometry>> Emit( const BuildKey&     key,
                                                                         const Tessellation& t );
        [[nodiscard]] const std::vector<uint32_t>& TopologyWords( const Desert::Geometry::DisplacementTopology& topology );

        DisplacementQuality                  m_Quality;
        std::unordered_map<uint64_t, Entry>  m_Entries;
        std::vector<std::weak_ptr<SharedGeometry>> m_Shared;
        // The GPU form of each live topology (CornerVertex | TriangleSubmesh | WeldPosition | WeldDirection).
        std::unordered_map<const Desert::Geometry::DisplacementTopology*, std::vector<uint32_t>> m_TopologyWords;
        std::shared_ptr<ComputePipeline>                                                 m_HeightsPipeline;
        std::shared_ptr<ComputePipeline>                                                 m_EmitPipeline;
        ShaderBindingLayoutCache                                                         m_HeightsLayout;
        ShaderBindingLayoutCache                                                         m_EmitLayout;
        DisplacementStats                                                                m_Stats;
        uint64_t                                                                         m_Frame = 0;
        uint64_t m_DisplacedFrame = UINT64_MAX; // the frame DisplaceFrame last ran for (once per frame)
        bool m_PipelineRefusalLogged                                                             = false;
    };
} // namespace Desert::Graphic::System
