#pragma once

#include <Common/Core/Core.hpp> // BOOLSUCCESS (the .cpp and the tests)
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneMetadata.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneTarget.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// THE VIEW'S TLAS (UE FRayTracingScene::Create + its build pass). Every frame whose resolved methods trace rays
// (RayTracingSceneRequired) declares ONE graph pass, "RayTracingScene.TLAS", that builds the view's top-level
// acceleration structure from this frame's instance list (RayTracingScene.hpp, BuildRayTracingInstances):
//   * the instance rows are a graph buffer filled by Builder::QueueBufferUpload (AccelStructBuildInput);
//   * the scratch is a graph transient (AccelStructBuildWrite), sized by the API for this frame's count;
//   * the TLAS storage is the view's PERSISTENT acceleration structure, registered as a graph external
//     (AccelStructBuildWrite) — kept across frames, rebuilt every frame (instances move), recreated only when the
//     instance count outgrows it. A reader (RT-SHADOWS, ray-traced reflections / GI) declares AccelStructRead on
//     RayTracingSceneFrame::Tlas and binds it through RDG::ShaderResourceKind::AccelerationStructure.
// A frame whose methods trace no ray declares nothing: no pass, no buffer, no upload.
//
// Device-free: the API object behind the TLAS is a RayTracingSceneTarget (VulkanRayTracingScene); this file only
// encodes the instance rows and declares the graph work, so the RenderGraphCompile suite can compile it.
namespace Desert::Graphic::System
{
    class MeshRenderer;
}

namespace Desert::Graphic::RayTracing
{
    inline constexpr std::string_view kTlasPassName               = "RayTracingScene.TLAS";
    inline constexpr std::string_view kTlasInstancesName          = "RayTracingScene.Instances";
    inline constexpr std::string_view kTlasScratchName            = "RayTracingScene.Scratch";
    inline constexpr std::string_view kTlasStorageName            = "RayTracingScene.TLAS";
    inline constexpr std::string_view kPrimitiveInstancesName     = "RayTracingScene.PrimitiveInstances";
    inline constexpr uint64_t         kPrimitiveInstancesRowBytes = sizeof( uint32_t ) * 2u; // uint2 per row

    // This frame's TLAS as the graph being built sees it. Refs are valid only for that graph.
    struct RayTracingSceneFrame
    {
        bool           Built = false; // false: the frame traces no ray; the refs are invalid
        RDG::BufferRef Tlas;
        RDG::BufferRef Instances;
        // RayTracingInstanceList::Ranges on the GPU: uint2 {FirstInstance, InstanceCount} per GPU-scene row,
        // indexed by an instance's custom index. A reader declares it StorageRead; uploaded with the TLAS, and the
        // upload is culled on a frame nobody reads it.
        RDG::BufferRef PrimitiveInstances;
        uint32_t       InstanceCount = 0;
        // The graph-transient buffers' sizes as declared (the descs above were created with exactly these): the
        // memory is placed in the device's transient heaps (ledger kind TransientHeap), so these are the frame's
        // share of those rows, stated once here and read by RayTracingSceneStats.
        uint64_t InstanceBytes          = 0;
        uint64_t PrimitiveInstanceBytes = 0;
        uint64_t ScratchBytes           = 0;
        // The GPU side of the frame's segment records (RayTracingGatherResult::SegmentRecords, one row each),
        // which the TLAS rows' SBT offsets index (a ray query's hit -> its vertex/index streams and alpha-test
        // material). Valid exactly when Built.
        RayTracingMetadataFrame Metadata;
    };

    // The rows of VkAccelerationStructureInstanceKHR (= D3D12_RAYTRACING_INSTANCE_DESC): a row-major 3x4
    // transform, customIndex:24 | mask:8, sbtOffset:24 | flags:8, the BLAS device address. The SBT offset is the
    // instance's first segment record (RayTracingSegmentRecord): a ray query adds the hit's geometry index to it.
    // At least one row: a graph buffer cannot be empty, and an empty TLAS is built with a primitive count of 0
    // over it.
    [[nodiscard]] inline std::vector<std::byte> EncodeInstanceRows( std::span<const RayTracingInstance> instances )
    {
        const std::size_t      rows = instances.empty() ? 1u : instances.size();
        std::vector<std::byte> bytes( rows * kInstanceRowBytes );
        for ( std::size_t i = 0; i < instances.size(); ++i )
        {
            const RayTracingInstance& instance = instances[i];
            std::byte* const          row      = bytes.data() + i * kInstanceRowBytes;
            std::memcpy( row, instance.Transform.data(), kInstanceTransformBytes );
            const uint32_t indexAndMask =
                 ( instance.CustomIndex & kMaxGpuSceneRow ) | ( static_cast<uint32_t>( instance.Mask ) << 24u );
            // Flags 0: culling is the ray's choice and opacity each segment's (RayTracingGeometrySegment::Opaque).
            const uint32_t sbtAndFlags = instance.FirstSegmentRecord & kMaxSegmentRecord;
            const uint64_t address =
                 static_cast<const RayTracingGeometryResource*>( instance.Geometry )->GetDeviceAddress();
            std::memcpy( row + 48, &indexAndMask, sizeof( indexAndMask ) );
            std::memcpy( row + 52, &sbtAndFlags, sizeof( sbtAndFlags ) );
            std::memcpy( row + 56, &address, sizeof( address ) );
        }
        return bytes;
    }

    // The rows of RayTracingScene.PrimitiveInstances: {FirstInstance, InstanceCount} per GPU-scene row, at least
    // one row (a graph buffer cannot be empty).
    [[nodiscard]] inline std::vector<std::byte>
    EncodePrimitiveInstanceRows( std::span<const RayTracingPrimitiveInstances> ranges )
    {
        const std::size_t      rows = ranges.empty() ? 1u : ranges.size();
        std::vector<std::byte> bytes( rows * kPrimitiveInstancesRowBytes );
        for ( std::size_t i = 0; i < ranges.size(); ++i )
        {
            std::byte* const row = bytes.data() + i * kPrimitiveInstancesRowBytes;
            std::memcpy( row, &ranges[i].FirstInstance, sizeof( uint32_t ) );
            std::memcpy( row + sizeof( uint32_t ), &ranges[i].InstanceCount, sizeof( uint32_t ) );
        }
        return bytes;
    }

    // Declares this frame's TLAS build when @p required (RayTracingSceneRequired of the view's methods); declares
    // nothing and answers Built = false otherwise. Errors: the target could not be prepared (the frame then has
    // no TLAS, and a reader must not be declared). @p blasInputs: the BLASes this graph builds before the TLAS
    // (RayTracingScene.DynamicGeometry); the TLAS build reads them, which orders it after their builds.
    [[nodiscard]] inline Common::ResultStr<RayTracingSceneFrame>
    AddRayTracingScenePass( RDG::Builder& graph, bool required, const RayTracingInstanceList& list,
                            RayTracingSceneTarget& target, std::span<const RDG::BufferRef> blasInputs = {} )
    {
        RayTracingSceneFrame frame;
        if ( !required )
            return Common::MakeSuccess( frame );

        frame.InstanceCount                           = static_cast<uint32_t>( list.Instances.size() );
        const Common::ResultStr<TlasBuildSizes> sizes = target.Prepare( frame.InstanceCount );
        if ( !sizes )
            return Common::MakeFormattedError<RayTracingSceneFrame>( "{}: {}", kTlasPassName, sizes.GetError() );

        const std::vector<std::byte> rows = EncodeInstanceRows( list.Instances );
        frame.InstanceBytes               = rows.size();
        frame.Instances = graph.CreateBuffer( RDG::BufferDesc{ frame.InstanceBytes }, kTlasInstancesName );
        graph.QueueBufferUpload( frame.Instances, rows );
        const std::vector<std::byte> ranges = EncodePrimitiveInstanceRows( list.Ranges );
        frame.PrimitiveInstanceBytes        = ranges.size();
        frame.PrimitiveInstances =
             graph.CreateBuffer( RDG::BufferDesc{ frame.PrimitiveInstanceBytes }, kPrimitiveInstancesName );
        graph.QueueBufferUpload( frame.PrimitiveInstances, ranges );
        frame.ScratchBytes = sizes.GetValue().ScratchBytes;
        const RDG::BufferRef scratch =
             graph.CreateBuffer( RDG::BufferDesc{ frame.ScratchBytes }, kTlasScratchName );
        frame.Tlas = graph.RegisterExternal( target.GetStorage(), kTlasStorageName );

        const RDG::BufferRef instances = frame.Instances;
        const RDG::BufferRef tlas      = frame.Tlas;
        const uint32_t       count     = frame.InstanceCount;
        graph.AddPass(
             kTlasPassName, RDG::PassFlags::Compute,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( instances, RDG::Access::AccelStructBuildInput );
                 for ( const RDG::BufferRef blas : blasInputs )
                     pass.Read( blas, RDG::Access::AccelStructBuildInput );
                 pass.Write( scratch, RDG::Access::AccelStructBuildWrite );
                 pass.Write( tlas, RDG::Access::AccelStructBuildWrite );
             },
             [&target, instances, scratch, tlas, count]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const auto instanceBinding = context.GetBuffer( instances, RDG::Access::AccelStructBuildInput );
                 if ( !instanceBinding )
                     return Common::MakeError( instanceBinding.GetError() );
                 const auto scratchBinding = context.GetBuffer( scratch, RDG::Access::AccelStructBuildWrite );
                 if ( !scratchBinding )
                     return Common::MakeError( scratchBinding.GetError() );
                 if ( const auto storage = context.GetBuffer( tlas, RDG::Access::AccelStructBuildWrite );
                      !storage )
                     return Common::MakeError( storage.GetError() );
                 return target.RecordBuild( context, instanceBinding.GetValue(), scratchBinding.GetValue(),
                                            count );
             } );
        frame.Built = true;
        return Common::MakeSuccess( frame );
    }
    // One view's ray-tracing scene (SceneRenderer owns one per view): gathers the primitives MeshRenderer queued
    // and gave a GPU-scene row, builds the instance list and declares the TLAS pass. The API target is created on
    // the first frame that needs a TLAS and kept for the view's life.
    // What one view's RT scene holds this frame (the viewport's RT scene overlay and the [ViewMemory] lines).
    // Every persistent byte count is READ FROM the ledger rows of the allocations (ResourceOwnership::GetBytes
    // through GetMemoryBytes / GetPositionsBytes / GetBuildInputBytes): one statement per allocation, the census
    // and these numbers cannot disagree. The graph transients (instance rows, primitive ranges, scratch) are
    // placed in the device's TransientHeap rows; their sizes are the frame's declared descs
    // (RayTracingSceneFrame).
    struct RayTracingSceneStats
    {
        bool Built = false; // the frame built a TLAS: the rest is meaningful
        std::array<uint32_t, static_cast<std::size_t>( RayTracingPrimitiveKind::Count )> InstancesByKind{};
        std::array<uint32_t, static_cast<std::size_t>( RayTracingSkip::Count )>          SkippedByReason{};
        uint64_t TracedBlasBytes         = 0; // every distinct BLAS this view's TLAS instances, variants included
        uint64_t VariantBlasBytes        = 0; // the scene's hidden-submesh variants
        uint64_t ProceduralBlasBytes     = 0; // the scene's procedural BLASes
        uint64_t SkinCacheBlasBytes      = 0; // the scene's skin-cache BLASes
        uint64_t SkinCachePositionsBytes = 0; // the scene's skin-cache positions buffers
        uint64_t TlasBytes               = 0; // this view's TLAS storage
        uint64_t TlasInstanceBytes       = 0; // this frame's instance rows (transient)
        uint64_t PrimitiveInstanceBytes  = 0; // this frame's RayTracingScene.PrimitiveInstances rows (transient)
        uint64_t TlasScratchBytes        = 0; // this frame's TLAS build scratch (transient)
        uint64_t DynamicScratchBytes     = 0; // this frame's RayTracingScene.DynamicGeometry scratch (transient)
        uint32_t SkinCacheEntries        = 0; // the scene's skin-cache entries (skinned primitives it holds)
        // Alpha-tested segments (Opaque = false) whose material has no opacity source in the ray-tracing
        // metadata yet: a ray query confirms their candidates as hits (RayTracingMetadataBuild).
        uint32_t SegmentsWithoutOpacity = 0;
        // The landscape tiles (RayTracingExternalPrimitives): built / refit this frame, their vertex + index
        // buffers and their BLAS storage (also counted in TracedBlasBytes, as the variants are).
        uint32_t LandscapeBuilt       = 0;
        uint32_t LandscapeRefit       = 0;
        uint64_t LandscapeVertexBytes = 0;
        uint64_t LandscapeBlasBytes   = 0;
    };

    // The view-memory lines of the RT scene (SceneRenderer logs them under [ViewMemory] when the TLAS storage is
    // (re)allocated): the same numbers the viewport overlay draws, from the same stats.
    [[nodiscard]] std::string FormatRayTracingSceneMemory( const RayTracingSceneStats& stats );

    // Primitives a renderer outside the mesh queues produced for this view's TLAS (LandscapeRayTracing): the
    // primitives, the dynamic-geometry builds they need this frame, and the storage of their BLASes built in an
    // earlier frame (the TLAS reads both). Refs are valid only for the graph they were declared in.
    // SegmentRecords: the primitives' segment records; their FirstSegmentRecord indexes this span, and AddPasses
    // rebases it onto the frame's table.
    struct RayTracingExternalPrimitives
    {
        std::span<const RayTracingPrimitive>            Primitives;
        std::span<const RayTracingDynamicGeometryBuild> Builds;
        std::span<const RDG::BufferRef>                 Resident;
        // The producer's own counts and storage this frame (RayTracingSceneStats::Landscape*).
        uint32_t Built       = 0; // BLASes built from scratch
        uint32_t Refit       = 0; // BLASes refit
        uint64_t VertexBytes = 0; // vertex + index buffers the builds read
        uint64_t BlasBytes   = 0; // BLAS storage of every primitive
        std::span<const RayTracingSegmentRecord> SegmentRecords;
    };

    class RayTracingSceneRenderer
    {
    public:
        // Called once per frame per view, after MeshRenderer::BuildObjectMotions (the rows are the instances'
        // custom indices) and before any pass that reads the TLAS is declared.
        // @p viewOrigin (cm) and @p drawDistanceScale (Graphic::DrawDistanceScale of the lens) feed the ISM
        // distance cull only: the TLAS is never frustum-culled (RayTracingGather.hpp).
        // @p cache is the scene's (Core::Scene::GetRayTracingCache): skinned BLASes and variants shared by its
        // views.
        // @p external: primitives another renderer produced this frame (the landscape tiles), instanced after the
        // meshes; their BLAS requests join RayTracingScene.DynamicGeometry.
        [[nodiscard]] Common::BoolResultStr AddPasses( RDG::Builder& graph, bool required,
                                                       const System::MeshRenderer& meshes,
                                                       RayTracingSceneCache& cache, const glm::vec3& viewOrigin,
                                                       float                               drawDistanceScale,
                                                       const RayTracingExternalPrimitives& external = {} );

        // This frame's TLAS refs (Built = false when the frame traces no ray or the build could not be declared).
        [[nodiscard]] const RayTracingSceneFrame& GetFrame() const
        {
            return m_Frame;
        }
        // This frame's instance list: its Skipped counts say how much of the queue the rays cannot see.
        [[nodiscard]] const RayTracingInstanceList& GetInstances() const
        {
            return m_Instances;
        }
        // The variant BLAS builds that failed this frame, one line each (the caller logs them).
        [[nodiscard]] const RayTracingSceneStats& GetStats() const
        {
            return m_Stats;
        }
        [[nodiscard]] const std::vector<std::string>& GetGatherErrors() const
        {
            return m_Gather.Errors;
        }

    private:
        std::unique_ptr<RayTracingSceneTarget>      m_Target;
        RayTracingGatherResult                      m_Gather;        // CPU scratch, capacity kept
        RDG::ExternalBuffer                         m_BonesImport;   // this view's ObjectBones, for the skin cache
        std::vector<RDG::BufferRef>                 m_DynamicBlas;   // CPU scratch: this frame's built BLASes
        std::vector<RayTracingDynamicGeometryBuild> m_DynamicBuilds; // CPU scratch: this frame's BLAS builds
        RayTracingSceneStats                        m_Stats;
        RayTracingInstanceList                      m_Instances;
        RayTracingSceneFrame                        m_Frame;
    };
} // namespace Desert::Graphic::RayTracing
