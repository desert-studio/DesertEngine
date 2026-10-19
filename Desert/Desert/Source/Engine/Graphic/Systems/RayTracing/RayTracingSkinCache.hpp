#pragma once

#include <Common/Core/Core.hpp> // BOOLSUCCESS (the .cpp)
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>
#include <Engine/Graphic/Systems/RayTracing/SkinnedRayTracingGeometry.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Desert
{
    class SkinnedMesh;
}

namespace Desert::Graphic
{
    class ComputePipeline;
    struct GpuObjectMotion;
} // namespace Desert::Graphic

namespace Desert::Graphic::System
{
    class MeshRenderer;
}

// THE GPU SKIN CACHE (UE FGPUSkinCache + FRayTracingDynamicGeometryCollection), one per scene, shared by every
// view. When it is on (Scalability Effects.SkinCache and a device with buffer device addresses), EVERY skinned
// primitive is skinned ONCE per frame by a compute pass ("SkinCache: Skin", the first view of the frame that draws
// it) into its cached streams, and both consumers read those streams:
//   * the raster view passes (Mesh/Surface/Vertex_Skinned.glslh) fetch the posed position, the PREVIOUS frame's
//     posed position (velocity / TAA) and the posed tangent frame by vertex index, through the address the
//     primitive's GpuObjectMotion row carries (UE FGPUSkinPassthroughVertexFactory) - no skinning in the vertex
//     stage of a view pass;
//   * the ray-tracing scene builds / refits the primitive's BLAS from the cached current positions: the cache
//     appends one build request per primitive, and the RT scene's ONE "RayTracingScene.DynamicGeometry" pass
//     (RayTracingDynamicGeometry.hpp, shared with procedural meshes and landscape tiles) records them all before
//     "RayTracingScene.TLAS". The RT pass never skins: its vertex input is the entry's streams.
// When it is off (or the device cannot address buffers from a shader) the raster skins in the vertex stage as
// before and the TLAS gets no skinned geometry: said once in the log, and every skinned primitive is counted
// RayTracingSkip::Skinned in the view's ray-tracing stats - never silent.
//
// ONE SOURCE OF THE POSE. The skin pass reads the bytes the raster would skin from: the view's ObjectBones palette
// at the primitive's row BoneOffset (MeshRenderer::BuildObjectMotions) and the vertices of the mesh the queue
// record names - a morphed component's twin (MeshRendererMorph), so morph targets apply before skinning, as UE's
// cache does.
//
// STREAMS of one entry: ONE buffer, mesh space, the mesh's vertex order (its index buffer and submesh ranges
// address it unchanged), all float:
//   [0, 3V)   current positions  - kDeformedPositionLayout, the BLAS's vertex input (address = buffer start)
//   [3V, 6V)  previous positions - last frame's current, rotated in the skin shader (each invocation reads its
//             vertex's current before overwriting it); a fresh entry (or one not skinned last frame) writes the
//             new pose into both, so a newly cached primitive carries camera motion only
//   [6V, 15V) tangent frame      - normal, tangent, bitangent (3 x float3) skinned by the palette's 3x3
namespace Desert::Graphic::RayTracing
{
    inline constexpr std::string_view kSkinCacheShader    = "SkinCache";
    inline constexpr std::string_view kSkinCachePassName  = "SkinCache: Skin";
    inline constexpr uint32_t         kSkinCacheGroupSize = 64; // LocalSize of Programs/Compute/SkinCache.shader

    class RayTracingSkinCache
    {
    public:
        // The consumer of Scalability Effects.SkinCache (SceneRenderer::BeginScene -> MeshRenderer). Turning it
        // off releases every entry: nothing keeps memory for a cache nobody reads.
        void               SetEnabled( bool enabled );
        [[nodiscard]] bool IsEnabled() const
        {
            return m_Enabled;
        }

        // RASTER, step 1 (MeshRenderer::BuildObjectMotions, before the rows are uploaded): makes an entry (keyed
        // {Entity, n-th skinned record of the entity this frame}, MotionKey's slot rule) fit each queued skinned
        // record of @p meshes and writes its streams' address and vertex count into the record's row, so the view
        // passes fetch instead of skinning. A record whose entry cannot be made keeps a row without an address
        // (the raster skins it in the vertex stage), logged once per entry. Off: rows are left without addresses.
        void PrepareRows( const System::MeshRenderer& meshes, std::span<GpuObjectMotion> rows, uint64_t frame );

        // RASTER, step 2 (MeshRenderer::BuildObjectMotions, after the view's ObjectBones are uploaded and before
        // its rows are): the scene's skin step (UE FGPUSkinCache::DispatchUpdateSkinning, which runs once per
        // FScene before any view renders). Builds its OWN graph declaring one "SkinCache: Skin" dispatch per entry
        // PrepareRows addressed and no graph of @p frame has skinned yet (this view's ObjectBones imported through
        // @p viewBones, a morphed record's vertices through the morph twin's own external), and executes it at
        // once - so every view graph, built and submitted after, only reads the streams, and the streams'
        // carried state gives each reader its StorageWrite->StorageRead barrier whatever the views' order. A later
        // view whose primitives an earlier view did not queue runs a graph for just those: still one skinning per
        // primitive per frame. A graph that fails to execute clears the addresses of the rows it was to fill
        // (those draws skin in the vertex stage this frame), logged.
        [[nodiscard]] Common::BoolResultStr SkinFrame( System::MeshRenderer&      meshes,
                                                       std::span<GpuObjectMotion> rows,
                                                       RDG::ExternalBuffer& viewBones, uint64_t frame );

        // RASTER, step 3 (SceneRenderer, after BuildObjectMotions, before any pass that draws a skinned primitive
        // or traces rays): registers the streams of every entry SkinFrame skinned in @p frame in the view graph
        // @p graph; their refs land in @p rasterReads, and each pass that draws skinned primitives through a
        // view-pass cell declares them StorageRead (MeshRenderer::DeclareSkinnedDraws). Declares no pass.
        void DeclareReads( RDG::Builder& graph, const System::MeshRenderer& meshes, uint64_t frame,
                           std::vector<RDG::BufferRef>& rasterReads );

        // RAY TRACING (RayTracingSceneRenderer, after DeclareReads of the same graph): for every primitive
        // SelectDynamicGeometry picks, prepares its entry's BLAS over DescribeDeformedMeshBlas (the instance's
        // hidden and non-opaque segments) and appends ONE build request per BLAS not yet built in @p frame to
        // @p buildsOut - its vertex input is the entry's streams (the current positions SkinFrame wrote), so the
        // RT scene's one "RayTracingScene.DynamicGeometry" pass refits / builds it without a second skinning - and
        // points the primitive's Geometry at it (and its DeformedPositions at the streams, registered in @p
        // graph, for the other readers of the posed positions). @p primitives[firstSkinned + i] is MeshRenderer's
        // skinned record i. A BLAS an earlier view of the frame built is appended to @p blasOut for this view's
        // TLAS pass to read. Off, or an entry without skinned streams: the primitive keeps Geometry null (counted
        // RayTracingSkip::Skinned), logged once.
        [[nodiscard]] Common::BoolResultStr AddPasses( RDG::Builder& graph, const System::MeshRenderer& meshes,
                                                       std::span<RayTracingPrimitive> primitives,
                                                       uint32_t firstSkinned, uint64_t frame,
                                                       std::vector<RayTracingDynamicGeometryBuild>& buildsOut,
                                                       std::vector<RDG::BufferRef>&                 blasOut );

        // Releases the entries no view asked for in @p frame (RayTracingSceneCache::BeginViewFrame).
        void EndFrame( const uint64_t frame )
        {
            std::erase_if( m_Entries, [frame]( const auto& entry ) { return entry.second.LastFrame != frame; } );
        }

        struct MemoryBytes
        {
            uint64_t Positions = 0; // the entries' cached streams (positions, previous positions, tangents)
            uint64_t Blas      = 0; // the entries' BLAS storage
        };
        // What the live entries hold (stats overlay).
        [[nodiscard]] MemoryBytes GetMemoryBytes() const
        {
            MemoryBytes bytes;
            for ( const auto& item : m_Entries )
            {
                if ( item.second.Streams )
                    bytes.Positions += item.second.Streams->GetBytes();
                if ( item.second.Geometry )
                    bytes.Blas += item.second.Geometry->GetMemoryBytes();
            }
            return bytes;
        }

        // Live entries (one per skinned primitive drawn last frame).
        [[nodiscard]] std::size_t GetEntryCount() const
        {
            return m_Entries.size();
        }

    private:
        struct Entry
        {
            std::unique_ptr<SkinCacheStreams>          Streams;
            std::unique_ptr<SkinnedRayTracingGeometry> Geometry;       // made on the first frame a view traces it
            const SkinnedMesh*                         Mesh = nullptr; // the mesh the streams were prepared for
            RDG::ExternalBuffer VerticesImport;        // the mesh's vertex buffer as a graph external
            uint64_t            LastFrame     = 0;     // last frame a view asked for it
            uint64_t            SkinnedFrame  = 0;     // last frame a skin graph that skinned it executed
            uint64_t            BuiltFrame    = 0;     // last frame its BLAS build was declared
            bool                StreamsReady  = false; // prepared in LastFrame: rows carry its address
            bool                HasSkinned    = false; // SkinnedFrame is meaningful
            bool                BlasReady     = false; // BuiltFrame's build was declared
            bool                RefusalLogged = false;
            // The registration of the streams / BLAS in the graph being declared
            // (RayTracingSkinCache::m_GraphSerial): an external is registered once per graph.
            uint64_t                      GraphSerial = 0;
            std::optional<RDG::BufferRef> StreamsRef;
            std::optional<RDG::BufferRef> StorageRef;
        };

        // The entry key of each skinned record of @p meshes, in order (MotionKey's slot rule).
        void KeyRecords( const System::MeshRenderer& meshes );
        // Starts @p entry's registrations afresh when it was last registered in another graph than the current
        // one.
        void Touch( Entry& entry ) const;
        // @p entry's streams / BLAS in @p graph, registered on first use in that graph.
        RDG::BufferRef     StreamsIn( RDG::Builder& graph, Entry& entry );
        RDG::BufferRef     StorageIn( RDG::Builder& graph, Entry& entry );
        [[nodiscard]] bool EnsurePipeline( std::size_t skinnedCount );

        std::unordered_map<uint64_t, Entry>    m_Entries;
        std::vector<uint64_t>                  m_RecordKeys;       // CPU scratch: KeyRecords' output
        std::unordered_map<uint32_t, uint32_t> m_RecordsPerEntity; // CPU scratch: skinned records seen per entity
        std::shared_ptr<ComputePipeline>       m_Pipeline;
        ShaderBindingLayoutCache               m_Layout;
        // The graph DeclareReads registered into last, and its serial: AddPasses must run on that same graph.
        const RDG::Builder* m_DeclaringGraph        = nullptr;
        uint64_t            m_GraphSerial           = 0;
        bool                m_Enabled               = true;
        bool                m_PipelineRefusalLogged = false;
        bool                m_OffLogged             = false;
    };
} // namespace Desert::Graphic::RayTracing
