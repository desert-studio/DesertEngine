#pragma once

#include <Common/Core/Core.hpp> // BOOLSUCCESS (the .cpp and the tests)
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingDynamicGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGeometry.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingScene.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneTarget.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
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
    inline constexpr std::string_view kTlasPassName      = "RayTracingScene.TLAS";
    inline constexpr std::string_view kTlasInstancesName = "RayTracingScene.Instances";
    inline constexpr std::string_view kTlasScratchName   = "RayTracingScene.Scratch";
    inline constexpr std::string_view kTlasStorageName   = "RayTracingScene.TLAS";

    // This frame's TLAS as the graph being built sees it. Refs are valid only for that graph.
    struct RayTracingSceneFrame
    {
        bool           Built = false; // false: the frame traces no ray; the refs are invalid
        RDG::BufferRef Tlas;
        RDG::BufferRef Instances;
        uint32_t       InstanceCount = 0;
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
        frame.Instances = graph.CreateBuffer( RDG::BufferDesc{ rows.size() }, kTlasInstancesName );
        graph.QueueBufferUpload( frame.Instances, rows );
        const RDG::BufferRef scratch =
             graph.CreateBuffer( RDG::BufferDesc{ sizes.GetValue().ScratchBytes }, kTlasScratchName );
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
    // What one view's RT scene holds this frame (the viewport's RT scene overlay). Bytes are the storage
    // allocations; the ledger rows of the same allocations are the census.
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
        uint64_t TlasInstanceBytes       = 0; // this frame's instance rows
    };

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
        std::span<const RayTracingSegmentRecord>        SegmentRecords;
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
