#pragma once

#include <Common/Core/Core.hpp> // BOOLSUCCESS (the .cpp and VulkanSkinnedRayTracingGeometry.cpp)
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>

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
//   * the ray-tracing scene builds / refits the primitive's BLAS from the cached current positions
//     ("RayTracingScene.DynamicGeometry", before "RayTracingScene.TLAS").
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
    inline constexpr std::string_view kSkinCacheShader         = "SkinCache";
    inline constexpr std::string_view kSkinCachePassName       = "SkinCache: Skin";
    inline constexpr std::string_view kDynamicGeometryPassName = "RayTracingScene.DynamicGeometry";
    inline constexpr uint32_t         kSkinCacheGroupSize = 64; // LocalSize of Programs/Compute/SkinCache.shader
    // Floats per vertex in an entry's streams buffer (layout above; Vertex_Skinned.glslh and SkinCache.shader).
    inline constexpr uint32_t kSkinCacheFloatsPerVertex = 15;

    // What one primitive's build costs this frame: the scratch it needs (alignment slack included) and whether it
    // refits the BLAS already built (true) or builds it anew (false).
    struct SkinnedBlasPrepared
    {
        uint64_t ScratchBytes = 0;
        bool     Refit        = false;
    };

    // The API side of one entry's cached streams (layout above), alive across frames.
    class SkinCacheStreams
    {
    public:
        virtual ~SkinCacheStreams() = default;
        // Makes the buffer fit @p vertexCount vertices. Answers true when the buffer was (re)created this call:
        // its previous positions hold nothing yet. Errors: no device, the device has no buffer device addresses,
        // the allocation failed.
        [[nodiscard]] virtual Common::ResultStr<bool> Prepare( uint32_t vertexCount ) = 0;
        // Graph external carrying its own state across graphs (StorageWrite by the skin pass, StorageRead by the
        // raster view passes, AccelStructBuildInput by the BLAS build).
        [[nodiscard]] virtual RDG::ExternalBuffer& GetBuffer() = 0;
        // The buffer's device address (its start = the current positions); 0 before a successful Prepare.
        [[nodiscard]] virtual uint64_t GetDeviceAddress() const = 0;
    };

    // The API's streams (VulkanSkinnedRayTracingGeometry.hpp).
    [[nodiscard]] std::unique_ptr<SkinCacheStreams> CreateSkinCacheStreams();

    // The API side of one entry's BLAS, alive across frames, built from the entry's current positions.
    class SkinnedRayTracingGeometry : public RayTracingGeometryResource
    {
    public:
        // Makes the BLAS fit @p mesh with @p hiddenSubmeshes left out, recreating it when the mesh's vertex/index
        // buffers, its vertex count or the mask changed. Errors: the device cannot build acceleration structures,
        // the mesh is not resident or has no visible triangle, an allocation failed.
        [[nodiscard]] virtual Common::ResultStr<SkinnedBlasPrepared> Prepare( const SkinnedMesh& mesh,
                                                                              uint64_t hiddenSubmeshes ) = 0;
        // Graph external of the BLAS storage the build writes.
        [[nodiscard]] virtual RDG::ExternalBuffer& GetStorage() = 0;
        // Records the build (or refit, per the last Prepare) into the pass's command buffer from the positions at
        // the start of @p positions (the entry's streams), its scratch at @p scratchOffset bytes into @p scratch.
        [[nodiscard]] virtual Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                                 const RDG::BufferBinding& positions,
                                                                 const RDG::BufferBinding& scratch,
                                                                 uint64_t                  scratchOffset ) = 0;
    };

    // The API's BLAS of a skinned entry (VulkanSkinnedRayTracingGeometry).
    [[nodiscard]] std::unique_ptr<SkinnedRayTracingGeometry> CreateSkinnedRayTracingGeometry();

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
        // record of @p meshes and writes its streams' address and vertex count into the record's row, so the view passes fetch
        // instead of skinning. A record whose entry cannot be made keeps a row without an address (the raster
        // skins it in the vertex stage), logged once per entry. Off: rows are left without addresses.
        void PrepareRows( const System::MeshRenderer& meshes,
                          std::span<GpuObjectMotion> rows, uint64_t frame );

        // RASTER, step 2 (SceneRenderer, after BuildObjectMotions, before any pass that draws a skinned primitive
        // or traces rays): declares one "SkinCache: Skin" dispatch per entry not yet skinned in @p frame (from
        // this view's ObjectBones, imported through @p viewBones) and registers every addressed entry's streams in
        // @p graph. Their refs land in @p rasterReads: each pass that draws skinned primitives through a view-pass
        // cell declares them StorageRead (MeshRenderer::DeclareSkinnedDraws).
        [[nodiscard]] Common::BoolResultStr AddSkinPasses( RDG::Builder& graph, const System::MeshRenderer& meshes,
                                                           RDG::ExternalBuffer& viewBones, uint64_t frame,
                                                           std::vector<RDG::BufferRef>& rasterReads );

        // RAY TRACING (RayTracingSceneRenderer, after AddSkinPasses of the same graph): for every primitive
        // SelectDynamicGeometry picks, prepares its entry's BLAS and declares ONE
        // "RayTracingScene.DynamicGeometry" pass building / refitting every BLAS not yet built in @p frame from
        // the entry's cached positions, and points the primitive's Geometry at it. @p primitives[firstSkinned + i]
        // is MeshRenderer's skinned record i. Every BLAS storage the TLAS reads is appended to @p blasOut. Off, or
        // an entry without skinned streams: the primitive keeps Geometry null (counted RayTracingSkip::Skinned),
        // logged once.
        [[nodiscard]] Common::BoolResultStr AddPasses( RDG::Builder& graph, const System::MeshRenderer& meshes,
                                                       std::span<RayTracingPrimitive> primitives,
                                                       uint32_t firstSkinned, uint64_t frame,
                                                       std::vector<RDG::BufferRef>& blasOut );

        // Releases the entries no view asked for in @p frame (RayTracingSceneCache::BeginViewFrame).
        void EndFrame( const uint64_t frame )
        {
            std::erase_if( m_Entries, [frame]( const auto& entry ) { return entry.second.LastFrame != frame; } );
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
            uint64_t            SkinnedFrame  = 0;     // last frame its skin pass was declared
            uint64_t            BuiltFrame    = 0;     // last frame its BLAS build was declared
            bool                StreamsReady  = false; // prepared in LastFrame: rows carry its address
            bool                HasSkinned    = false; // SkinnedFrame is meaningful
            bool                BlasReady     = false; // BuiltFrame's build was declared
            bool                RefusalLogged = false;
            // The registration of the streams / BLAS in the graph being declared (RayTracingSkinCache::m_GraphSerial):
            // an external is registered once per graph.
            uint64_t                      GraphSerial = 0;
            std::optional<RDG::BufferRef> StreamsRef;
            std::optional<RDG::BufferRef> StorageRef;
        };

        // The entry key of each skinned record of @p meshes, in order (MotionKey's slot rule).
        void KeyRecords( const System::MeshRenderer& meshes );
        // @p entry's streams / BLAS in @p graph, registered on first use in that graph.
        RDG::BufferRef StreamsIn( RDG::Builder& graph, Entry& entry );
        RDG::BufferRef StorageIn( RDG::Builder& graph, Entry& entry );
        [[nodiscard]] bool EnsurePipeline( std::size_t skinnedCount );

        std::unordered_map<uint64_t, Entry>    m_Entries;
        std::vector<uint64_t>                  m_RecordKeys;       // CPU scratch: KeyRecords' output
        std::unordered_map<uint32_t, uint32_t> m_RecordsPerEntity; // CPU scratch: skinned records seen per entity
        std::shared_ptr<ComputePipeline>       m_Pipeline;
        ShaderBindingLayoutCache               m_Layout;
        // The graph AddSkinPasses declared into last, and its serial: AddPasses must run on that same graph.
        const RDG::Builder* m_DeclaringGraph = nullptr;
        uint64_t            m_GraphSerial    = 0;
        bool                                   m_Enabled               = true;
        bool                                   m_PipelineRefusalLogged = false;
        bool                                   m_OffLogged             = false;
    };
} // namespace Desert::Graphic::RayTracing
