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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// THE VIEW'S TLAS (UE FRayTracingScene::Create + its build pass). Every frame whose resolved methods trace rays
// (RayTracingSceneRequired) declares ONE graph pass, "RayTracingScene.TLAS", that builds the view's top-level
// acceleration structure from this frame's instance list (RayTracingScene.hpp, BuildRayTracingInstances):
//   * the instance rows are the view's PERSISTENT rows (RayTracingSceneTarget::GetRows, SceneRowBuffer::Instances,
//     a graph external, AccelStructBuildInput; UE FRayTracingScene's instance buffer): a frame writes only the CPU
//     rows that changed since the last upload that executed (SceneRowUploadState, one scatter copy), all of them
//     after a relist, a new buffer or an upload that never ran, none on a still frame; the GPU-written rows follow
//     them, rewritten by their pass each frame. RayTracingScene.PrimitiveInstances and the metadata
//     (GeometryRecords / MaterialRecords) are persistent rows of the target the same way;
//   * the scratch is a graph transient (AccelStructBuildWrite), sized by the API for this frame's count;
//   * the TLAS storage is the view's PERSISTENT acceleration structure, registered as a graph external
//     (AccelStructBuildWrite) — kept across frames, updated in place on a frame whose rows only moved and built
//     otherwise (RayTracingSceneRenderer::SetTlasUpdatesBeforeRebuild), recreated only when the instance count
//     outgrows it. A reader (RT-SHADOWS, ray-traced reflections / GI) declares AccelStructRead on
//     RayTracingSceneFrame::Tlas and binds it through RDG::ShaderResourceKind::AccelerationStructure.
// A frame whose methods trace no ray declares nothing: no pass, no buffer, no upload.
//
// Device-free: the API object behind the TLAS is a RayTracingSceneTarget (VulkanRayTracingScene); this file only
// encodes the instance rows and declares the graph work, so the RenderGraphCompile suite can compile it.
namespace Desert::Graphic::System
{
    class MeshRenderer;
}

namespace Desert::Graphic::GpuDriven
{
    class GpuScene;
    struct PrimitiveChange;
}

namespace Desert::Graphic::RayTracing
{
    struct RayTracingGatherInputs;

    inline constexpr std::string_view kTlasPassName               = "RayTracingScene.TLAS";
    inline constexpr std::string_view kTlasInstancesName          = "RayTracingScene.Instances";
    inline constexpr std::string_view kTlasInstanceScatterName    = "RayTracingScene.InstanceScatter";
    inline constexpr std::string_view kPrimitiveInstanceScatterName = "RayTracingScene.PrimitiveInstanceScatter";
    inline constexpr std::string_view kMetadataGeometryScatterName  = "RayTracingScene.GeometryRecordScatter";
    inline constexpr std::string_view kMetadataMaterialScatterName  = "RayTracingScene.MaterialRecordScatter";
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
        // indexed by an instance's custom index. A reader declares it StorageRead; the view's persistent rows
        // (SceneRowBuffer::PrimitiveInstances), written only on a frame whose list was laid out anew.
        RDG::BufferRef PrimitiveInstances;
        uint32_t       InstanceCount = 0;
        // The CPU rows this frame copied into the persistent instance rows (0 on a still frame; every CPU row
        // after a relist, a new buffer or an upload that never ran).
        uint32_t UploadedInstanceRows = 0;
        // The persistent row buffers' device bytes (their ledger rows, RayTracingSceneTarget::GetRowMemoryBytes).
        uint64_t InstanceBytes          = 0;
        uint64_t PrimitiveInstanceBytes = 0;
        // The scratch is a graph transient: its size as declared (the desc was created with exactly this), the
        // frame's share of the device's transient heaps (ledger kind TransientHeap), read by RayTracingSceneStats.
        uint64_t ScratchBytes = 0;
        // WHAT THE FRAME TRACES, as a value a reader can compare across frames: an FNV-1a hash of the TLAS rows
        // (transforms, BLAS addresses, custom indices, SBT offsets, masks) and whether a BLAS was rebuilt in this
        // graph (DynamicGeometry: skinned/deformed geometry changes under an unchanged row). The path tracer
        // restarts its accumulation when either says the traced scene is not the previous frame's.
        uint64_t InstanceHash    = 0;
        bool     DynamicGeometry = false;
        // The TLAS was updated in place from its last build (only rows moved), not built (TlasBuildSizes::Update).
        bool Updated = false;
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
    inline void EncodeInstanceRow( const RayTracingInstance& instance, std::byte* const row )
    {
        std::memcpy( row, instance.Transform.data(), kInstanceTransformBytes );
        // The whole 24-bit field (kNoGpuSceneRow is its last value, and an instance with no row carries it).
        const uint32_t indexAndMask =
             ( instance.CustomIndex & kNoGpuSceneRow ) | ( static_cast<uint32_t>( instance.Mask ) << 24u );
        // Flags 0: culling is the ray's choice and opacity each segment's (RayTracingGeometrySegment::Opaque).
        const uint32_t sbtAndFlags = instance.FirstSegmentRecord & kMaxSegmentRecord;
        const uint64_t address =
             static_cast<const RayTracingGeometryResource*>( instance.Geometry )->GetDeviceAddress();
        std::memcpy( row + 48, &indexAndMask, sizeof( indexAndMask ) );
        std::memcpy( row + 52, &sbtAndFlags, sizeof( sbtAndFlags ) );
        std::memcpy( row + 56, &address, sizeof( address ) );
    }
    [[nodiscard]] inline std::vector<std::byte> EncodeInstanceRows( std::span<const RayTracingInstance> instances )
    {
        const std::size_t      rows = instances.empty() ? 1u : instances.size();
        std::vector<std::byte> bytes( rows * kInstanceRowBytes );
        for ( std::size_t i = 0; i < instances.size(); ++i )
            EncodeInstanceRow( instances[i], bytes.data() + i * kInstanceRowBytes );
        return bytes;
    }

    inline constexpr std::string_view kGpuInstancesPassName = "RayTracingScene.GpuInstances";

    // THE GPU-WRITTEN TLAS ROWS (RayTracingGpuInstanceSource): after the CPU rows, each GPU-instanced primitive's
    // reserved rows, written by "RayTracingScene.GpuInstances" between the instance upload and the TLAS build from
    // the producer's transforms and drawn count. Device-free here; RayTracingSceneRenderer.cpp owns the program.
    // Writing rows is not a BLAS build: it never sets RayTracingSceneFrame::DynamicGeometry (the path tracer's
    // accumulation runs on over a still grass carpet).
    class RayTracingGpuInstanceRows
    {
    public:
        virtual ~RayTracingGpuInstanceRows() = default;
        // This frame's sources: each traced source's rows counted in @p list.InstancesByKind (GpuInstanced), each
        // one that cannot be traced counted in @p list.Skipped. Errors: the program could not be made.
        [[nodiscard]] virtual Common::BoolResultStr Prepare( std::span<const RayTracingGpuInstanceSource> sources,
                                                             std::span<const RayTracingPrimitive> primitives,
                                                             RayTracingInstanceList&              list ) = 0;
        // The rows the prepared sources take, after the CPU rows.
        [[nodiscard]] virtual uint32_t GetRows() const = 0;
        // What the rows trace, as a value comparable across frames (BLAS addresses, rows, records, masks): folded
        // into RayTracingSceneFrame::InstanceHash. The instances themselves are the GPU's.
        [[nodiscard]] virtual uint64_t GetKey() const = 0;
        // Declares the pass writing the rows into @p instances from row @p firstRow on.
        [[nodiscard]] virtual Common::BoolResultStr AddPass( RDG::Builder& graph, RDG::BufferRef instances,
                                                             uint32_t firstRow ) = 0;
    };
    [[nodiscard]] std::unique_ptr<RayTracingGpuInstanceRows> CreateRayTracingGpuInstanceRows();

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

    // THE CPU ROWS OF A TLAS, kept across frames by the view (RayTracingSceneRenderer): encoded once when its
    // instance list is built, then patched row by row while only rows move (PatchRayTracingSceneRow), so a still
    // frame encodes nothing and a moving primitive re-encodes its own row. Hash: what the rows trace, comparable
    // across frames (RayTracingSceneFrame::InstanceHash) - FNV-1a over every row when encoded; a patch that
    // changes a row's bytes folds the row's index and new bytes in, so the hash moves exactly when a row did.
    // THE UPLOAD STATE of one persistent row buffer (UE FRDGScatterUploadBuffer): AllDirty - every row goes up (an
    // encode, a build, a new buffer); DirtyRows - the rows changed since the last upload (may repeat). Queued /
    // Executed: the serial of the last upload declared, and of the last one whose copy was recorded (written by
    // its pass's exec, read on a later frame, as RayTracingSceneTarget::Prepare reads what RecordBuild built):
    // a frame finding them apart sends every row, so a graph that never ran its upload loses no row.
    struct SceneRowUploadState
    {
        std::vector<uint32_t> DirtyRows;
        bool                  AllDirty = true;
        uint64_t              Queued   = 0;
        uint64_t              Executed = 0;
    };

    // THE UPLOAD STATE of the persistent device rows: InstanceUpload - the TLAS rows (AllDirty after an encode,
    // DirtyRows the rows a patch changed); PrimitiveUpload - RayTracingScene.PrimitiveInstances (an encode only).
    struct RayTracingSceneRows
    {
        std::vector<std::byte> Instances;                       // EncodeInstanceRows (at least one row)
        std::vector<std::byte> PrimitiveInstances;              // EncodePrimitiveInstanceRows
        uint32_t               Count = 0;                       // the CPU rows
        uint64_t               Hash  = 14695981039346656037ull; // FNV-1a 64 offset basis
        SceneRowUploadState    InstanceUpload;
        SceneRowUploadState    PrimitiveUpload;
    };
    [[nodiscard]] inline RayTracingSceneRows EncodeRayTracingSceneRows( const RayTracingInstanceList& list )
    {
        RayTracingSceneRows rows;
        rows.Instances          = EncodeInstanceRows( list.Instances );
        rows.PrimitiveInstances = EncodePrimitiveInstanceRows( list.Ranges );
        rows.Count              = static_cast<uint32_t>( list.Instances.size() );
        for ( const std::byte b : rows.Instances )
            rows.Hash = ( rows.Hash ^ static_cast<uint64_t>( b ) ) * 1099511628211ull;
        return rows;
    }
    // Re-encodes row @p index of @p rows from @p instance; true when its bytes changed (the hash took them in).
    inline bool PatchRayTracingSceneRow( RayTracingSceneRows& rows, const uint32_t index,
                                         const RayTracingInstance& instance )
    {
        std::array<std::byte, kInstanceRowBytes> encoded{};
        EncodeInstanceRow( instance, encoded.data() );
        std::byte* const row = rows.Instances.data() + static_cast<std::size_t>( index ) * kInstanceRowBytes;
        if ( std::memcmp( row, encoded.data(), kInstanceRowBytes ) == 0 )
            return false;
        std::memcpy( row, encoded.data(), kInstanceRowBytes );
        rows.InstanceUpload.DirtyRows.push_back( index );
        rows.Hash = ( rows.Hash ^ index ) * 1099511628211ull;
        for ( const std::byte b : encoded )
            rows.Hash = ( rows.Hash ^ static_cast<uint64_t>( b ) ) * 1099511628211ull;
        return true;
    }

    // THE ROW UPLOAD (UE FRDGScatterUploadBuffer): writes the rows of @p rows (@p rowBytes each) into the
    // persistent
    // @p target buffer @p buffer, registered in this graph as @p destination - every row when @p state is AllDirty
    // or its last upload never ran, else only its DirtyRows - packed into one staging buffer and copied run by run
    // (RayTracingSceneTarget::RecordRowScatter, pass and staging @p scatterName); nothing on a clean frame.
    // Answers
    // the rows declared and leaves @p state clean; its exec marks the upload executed.
    [[nodiscard]] inline uint32_t AddSceneRowUpload( RDG::Builder& graph, const std::string_view scatterName,
                                                     const std::span<const std::byte> rows,
                                                     const uint32_t rowBytes, SceneRowUploadState& state,
                                                     RayTracingSceneTarget& target,
                                                     const RDG::BufferRef   destination )
    {
        const auto             rowCount = static_cast<uint32_t>( rows.size() / rowBytes );
        const bool             every    = state.AllDirty || state.Executed != state.Queued;
        std::vector<uint32_t>& dirty    = state.DirtyRows;
        const auto             runs     = std::make_shared<std::vector<SceneRowRun>>();
        std::vector<std::byte> packed;
        uint32_t               written = 0;
        if ( every )
        {
            written = rowCount;
            if ( rowCount > 0 )
                runs->push_back( SceneRowRun{ .FirstRow = 0u, .RowCount = rowCount } );
        }
        else if ( !dirty.empty() )
        {
            std::sort( dirty.begin(), dirty.end() );
            dirty.erase( std::unique( dirty.begin(), dirty.end() ), dirty.end() );
            packed.reserve( dirty.size() * rowBytes );
            for ( const uint32_t row : dirty )
            {
                if ( row >= rowCount )
                    continue; // a row past this frame's rows: no reader indexes it
                const std::byte* const source = rows.data() + static_cast<std::size_t>( row ) * rowBytes;
                packed.insert( packed.end(), source, source + rowBytes );
                if ( !runs->empty() && runs->back().FirstRow + runs->back().RowCount == row )
                    ++runs->back().RowCount;
                else
                    runs->push_back( SceneRowRun{ .FirstRow = row, .RowCount = 1u } );
                ++written;
            }
        }
        dirty.clear();
        state.AllDirty = false;
        if ( runs->empty() )
            return 0;
        std::span<const std::byte> payload = packed;
        if ( every )
            payload = rows.first( static_cast<std::size_t>( rowCount ) * rowBytes );
        const RDG::BufferRef staging = graph.CreateBuffer( RDG::BufferDesc{ payload.size() }, scatterName );
        graph.QueueBufferUpload( staging, payload );
        const uint64_t             serial    = ++state.Queued;
        SceneRowUploadState* const committed = &state;
        graph.AddPass(
             scatterName, RDG::PassFlags::Copy,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( staging, RDG::Access::CopySrc );
                 pass.Write( destination, RDG::Access::CopyDst );
             },
             [&target, staging, destination, runs, rowBytes, committed,
              serial]( RDG::PassContext& context ) -> Common::BoolResultStr
             {
                 const auto source = context.GetBuffer( staging, RDG::Access::CopySrc );
                 if ( !source )
                     return Common::MakeError( source.GetError() );
                 const auto rowsBinding = context.GetBuffer( destination, RDG::Access::CopyDst );
                 if ( !rowsBinding )
                     return Common::MakeError( rowsBinding.GetError() );
                 Common::BoolResultStr recorded = target.RecordRowScatter( context, source.GetValue(),
                                                                    rowsBinding.GetValue(), *runs, rowBytes );
                 if ( recorded )
                     committed->Executed = serial;
                 return recorded;
             } );
        return written;
    }

    // One persistent row buffer in this graph: @p buffer made to hold @p bytes (a new buffer makes @p state
    // AllDirty before anything else can fail), registered as @p name, and @p rows' changed rows written
    // (AddSceneRowUpload).
    struct SceneRowsFrame
    {
        RDG::BufferRef Rows;
        uint32_t       Uploaded = 0; // rows declared this frame
    };
    [[nodiscard]] inline Common::ResultStr<SceneRowsFrame>
    AddSceneRows( RDG::Builder& graph, RayTracingSceneTarget& target, const SceneRowBuffer buffer,
                  const std::string_view name, const std::string_view scatterName, const uint64_t bytes,
                  const std::span<const std::byte> rows, const uint32_t rowBytes, SceneRowUploadState& state )
    {
        const Common::ResultStr<bool> made = target.EnsureRows( buffer, bytes );
        if ( !made )
            return Common::MakeFormattedError<SceneRowsFrame>( "{}: {}", name, made.GetError() );
        if ( made.GetValue() )
            state.AllDirty = true;
        SceneRowsFrame frame;
        frame.Rows     = graph.RegisterExternal( target.GetRows( buffer ), name );
        frame.Uploaded = AddSceneRowUpload( graph, scatterName, rows, rowBytes, state, target, frame.Rows );
        return Common::MakeSuccess( frame );
    }

    // THE METADATA'S PERSISTENT ROWS: @p metadata's geometry and material records in the target's GeometryRecords
    // /
    // MaterialRecords buffers, only the rows @p geometry / @p materials name changed (every row after a build, a
    // new buffer or an upload that never ran). A buffer holds at least one row: a scene with no masked segment
    // reads no material row. Answers this graph's refs.
    [[nodiscard]] inline Common::ResultStr<RayTracingMetadataFrame>
    AddRayTracingMetadataUpload( RDG::Builder& graph, const RayTracingMetadata& metadata,
                                 SceneRowUploadState& geometry, SceneRowUploadState& materials,
                                 RayTracingSceneTarget& target )
    {
        using Result                     = RayTracingMetadataFrame;
        constexpr uint32_t kGeometryRow  = sizeof( RayTracingGeometryRecord );
        constexpr uint32_t kMaterialRow  = sizeof( RayTracingMaterialRecord );
        const std::span<const std::byte> geometryRows = std::as_bytes( std::span( metadata.Geometry ) );
        const std::span<const std::byte> materialRows = std::as_bytes( std::span( metadata.Materials ) );
        const uint64_t     geometryBytes = std::max<uint64_t>( geometryRows.size(), kGeometryRow );
        const uint64_t     materialBytes = std::max<uint64_t>( materialRows.size(), kMaterialRow );
        const Common::ResultStr<SceneRowsFrame> geometryFrame =
             AddSceneRows( graph, target, SceneRowBuffer::GeometryRecords, kMetadataGeometryName,
                           kMetadataGeometryScatterName, geometryBytes, geometryRows, kGeometryRow, geometry );
        if ( !geometryFrame )
            return Common::MakeError<Result>( geometryFrame.GetError() );
        const Common::ResultStr<SceneRowsFrame> materialFrame =
             AddSceneRows( graph, target, SceneRowBuffer::MaterialRecords, kMetadataMaterialsName,
                           kMetadataMaterialScatterName, materialBytes, materialRows, kMaterialRow, materials );
        if ( !materialFrame )
            return Common::MakeError<Result>( materialFrame.GetError() );
        return Common::MakeSuccess( Result{ .Geometry  = geometryFrame.GetValue().Rows,
                                            .Materials = materialFrame.GetValue().Rows } );
    }

    // Declares this frame's TLAS build when @p required (RayTracingSceneRequired of the view's methods); declares
    // nothing and answers Built = false otherwise. Errors: the target could not be prepared (the frame then has
    // no TLAS, and a reader must not be declared). @p blasInputs: the BLASes this graph builds before the TLAS
    // (RayTracingScene.DynamicGeometry); the TLAS build reads them, which orders it after their builds.
    // @p blasBuiltInThisGraph: some of them were (re)built in this graph (RayTracingSceneFrame::DynamicGeometry).
    // @p gpuRows: the GPU-written rows after @p sceneRows' (prepared for this frame), or null when there are none.
    // @p update: @p sceneRows hold the instances of the target's last build in the same order, only moved since:
    // the TLAS is updated in place when the target can take it (RayTracingSceneTarget::Prepare;
    // RayTracingSceneFrame::Updated), built otherwise. @p sceneRows' changed rows go up into the target's
    // persistent rows, and its PrimitiveInstances when laid out anew (AddSceneRows); they are clean after the
    // call.
    [[nodiscard]] inline Common::ResultStr<RayTracingSceneFrame>
    AddRayTracingScenePass( RDG::Builder& graph, bool required, RayTracingSceneRows& sceneRows,
                            RayTracingSceneTarget& target, std::span<const RDG::BufferRef> blasInputs = {},
                            const bool blasBuiltInThisGraph = false, RayTracingGpuInstanceRows* gpuRows = nullptr,
                            const bool update = false )
    {
        RayTracingSceneFrame frame;
        if ( !required )
            return Common::MakeSuccess( frame );

        const uint32_t cpuRows                        = sceneRows.Count;
        const uint32_t gpuRowCount                    = gpuRows != nullptr ? gpuRows->GetRows() : 0u;
        frame.InstanceCount                           = cpuRows + gpuRowCount;
        const Common::ResultStr<TlasBuildSizes> sizes = target.Prepare( frame.InstanceCount, update );
        if ( !sizes )
            return Common::MakeFormattedError<RayTracingSceneFrame>( "{}: {}", kTlasPassName, sizes.GetError() );
        frame.Updated      = sizes.GetValue().Update;
        frame.InstanceHash = sceneRows.Hash;
        // A BLAS BUILT in this graph, not one merely read: resident dynamic BLASes (landscape tiles, procedural
        // meshes built in an earlier frame) are blasInputs every frame without anything having changed.
        frame.DynamicGeometry = blasBuiltInThisGraph;
        if ( gpuRowCount > 0 )
            frame.InstanceHash = ( frame.InstanceHash ^ gpuRows->GetKey() ) * 1099511628211ull;
        // The persistent instance rows hold every row the build reads (at least one), the CPU rows' changed ones
        // written here; the GPU rows follow them, the pass below writes every one of them.
        const auto     cpuRowBytes = std::span<const std::byte>( sceneRows.Instances )
                                      .first( static_cast<std::size_t>( cpuRows ) * kInstanceRowBytes );
        const uint64_t instanceRowBytes =
             static_cast<uint64_t>( std::max( frame.InstanceCount, 1u ) ) * kInstanceRowBytes;
        const Common::ResultStr<SceneRowsFrame> instanceRows =
             AddSceneRows( graph, target, SceneRowBuffer::Instances, kTlasInstancesName, kTlasInstanceScatterName,
                           instanceRowBytes, cpuRowBytes, kInstanceRowBytes, sceneRows.InstanceUpload );
        if ( !instanceRows )
            return Common::MakeFormattedError<RayTracingSceneFrame>( "{}: {}", kTlasPassName,
                                                                     instanceRows.GetError() );
        frame.Instances            = instanceRows.GetValue().Rows;
        frame.UploadedInstanceRows = instanceRows.GetValue().Uploaded;
        frame.InstanceBytes        = target.GetRowMemoryBytes( SceneRowBuffer::Instances );
        if ( gpuRowCount > 0 )
            if ( const Common::BoolResultStr written = gpuRows->AddPass( graph, frame.Instances, cpuRows );
                 !written )
                return Common::MakeFormattedError<RayTracingSceneFrame>( "{}: {}", kGpuInstancesPassName,
                                                                         written.GetError() );
        const Common::ResultStr<SceneRowsFrame> primitiveRows = AddSceneRows(
             graph, target, SceneRowBuffer::PrimitiveInstances, kPrimitiveInstancesName,
             kPrimitiveInstanceScatterName, sceneRows.PrimitiveInstances.size(), sceneRows.PrimitiveInstances,
             static_cast<uint32_t>( kPrimitiveInstancesRowBytes ), sceneRows.PrimitiveUpload );
        if ( !primitiveRows )
            return Common::MakeFormattedError<RayTracingSceneFrame>( "{}: {}", kTlasPassName,
                                                                     primitiveRows.GetError() );
        frame.PrimitiveInstances     = primitiveRows.GetValue().Rows;
        frame.PrimitiveInstanceBytes = target.GetRowMemoryBytes( SceneRowBuffer::PrimitiveInstances );
        frame.ScratchBytes = sizes.GetValue().ScratchBytes;
        const RDG::BufferRef scratch =
             graph.CreateBuffer( RDG::BufferDesc{ frame.ScratchBytes }, kTlasScratchName );
        frame.Tlas = graph.RegisterExternal( target.GetStorage(), kTlasStorageName );

        const RDG::BufferRef instances = frame.Instances;
        const RDG::BufferRef tlas      = frame.Tlas;
        const uint32_t       count     = frame.InstanceCount;
        const bool           updates   = frame.Updated;
        graph.AddPass(
             kTlasPassName, RDG::PassFlags::Compute | RDG::PassFlags::NeverParallel,
             [&]( RDG::PassBuilder& pass )
             {
                 pass.Read( instances, RDG::Access::AccelStructBuildInput );
                 for ( const RDG::BufferRef blas : blasInputs )
                     pass.Read( blas, RDG::Access::AccelStructBuildInput );
                 pass.Write( scratch, RDG::Access::AccelStructBuildWrite );
                 pass.Write( tlas, RDG::Access::AccelStructBuildWrite );
             },
             [&target, instances, scratch, tlas, count,
              updates]( RDG::PassContext& context ) -> Common::BoolResultStr
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
                 return target.RecordBuild( context, instanceBinding.GetValue(), scratchBinding.GetValue(), count,
                                            updates );
             } );
        frame.Built = true;
        return Common::MakeSuccess( frame );
    }
    // One view's ray-tracing scene (SceneRenderer owns one per view): gathers the primitives MeshRenderer queued
    // and gave a GPU-scene row, builds the instance list and declares the TLAS pass. The API target is created on
    // the first frame that needs a TLAS and kept for the view's life.
    // What one view's RT scene holds this frame (the viewport's RT scene overlay and the [ViewMemory] lines).
    // Every persistent byte count is READ FROM the ledger rows of the allocations (ResourceOwnership::GetBytes
    // through GetMemoryBytes / SkinCacheStreams::GetBytes / GetBuildInputBytes): one statement per allocation,
    // the census and these numbers cannot disagree. The graph transients (instance rows, primitive ranges,
    // scratch) are placed in the device's TransientHeap rows; their sizes are the frame's declared descs
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
        uint64_t SkinCachePositionsBytes = 0; // the scene's skin-cache streams (positions, previous, tangents)
        uint64_t TlasBytes               = 0; // this view's TLAS storage
        uint64_t TlasInstanceBytes       = 0; // this view's persistent instance rows (SceneRowBuffer::Instances)
        uint64_t PrimitiveInstanceBytes  = 0; // this view's persistent RayTracingScene.PrimitiveInstances rows
        uint64_t TlasScratchBytes        = 0; // this frame's TLAS build scratch (transient)
        uint64_t DynamicScratchBytes     = 0; // this frame's RayTracingScene.DynamicGeometry scratch (transient)
        uint32_t SkinCacheEntries        = 0; // the scene's skin-cache entries (skinned primitives it holds)
        // The scene's displaced geometry (DisplacementCache, CONTRACT-DISPLACE §5): the twins' buffers, the
        // topologies they read, and the pool's level lowerings - every one counted, none silent.
        uint64_t DisplacedBufferBytes    = 0;
        uint64_t DisplacedTopologyBytes  = 0;
        uint32_t DisplacedEntries        = 0;
        uint32_t DisplacedGeometries     = 0;
        uint32_t DisplacedLevelsLowered  = 0; // this frame
        uint32_t DisplacedLevelsHeld     = 0; // lowering steps still in force
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
        uint32_t                                 Built       = 0; // BLASes built from scratch
        uint32_t                                 Refit       = 0; // BLASes refit
        uint64_t                                 VertexBytes = 0; // vertex + index buffers the builds read
        uint64_t                                 BlasBytes   = 0; // BLAS storage of every primitive
        std::span<const RayTracingSegmentRecord> SegmentRecords;
        // The alpha test of each landscape tile's masked segment, by RayTracingSegmentRecord::LandscapeTile
        // (LandscapeVisibilityRecord); empty for a tile with no weightmap, whose masked segment holds no triangle.
        std::span<const std::optional<RayTracingMaterialRecord>> LandscapeOpacity;
    };

    class RayTracingSceneRenderer
    {
    public:
        // Called once per frame per view, after MeshRenderer::SyncGpuScene (the GpuScene instance indices are
        // the instances' custom indices) and before any pass that reads the TLAS is declared.
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

        // THE SCENE'S ROWS WITHOUT A TLAS: on a frame whose AddPasses built none, gathers this frame's primitives
        // and
        // segment records with no BLAS resolved (GatherRayTracingPrimitives with no variants) from the queues and
        // view AddPasses was given, so GetPrimitives / GetSegmentRecords answer for a reader of the scene's meshes
        // (the emissive light list under the software GI tracer, EmissiveLights.hpp). A frame with a TLAS keeps
        // its gather (the same rows). Cached across frames and kept current by THE GPUSCENE CHANGE STREAM
        // (GpuScene::ChangesSince, GI-ALL-LIGHTS step 6; UE FScene primitive updates): a still scene reads no
        // change, a moved primitive's change sets World on the kept primitives of its row (O(changes), no record
        // walked, no key built). Gathered in full only on a structure change of a meshed primitive (added,
        // removed, updated, Content) or a Discarded frame, when the stream no longer reaches the cursor, when
        // MakeRayTracingSceneUnsyncedKey (the grass producers; the view while an ISM culls by distance) differs,
        // or after a TLAS gather replaced it. Error: called before this frame's AddPasses or GPU-scene sync.
        [[nodiscard]] Common::BoolResultStr GatherScene();
        // How many full scene gathers GatherScene ran since the view was made (a still scene, or one whose
        // primitives only move: one).
        [[nodiscard]] uint64_t GetSceneGatherCount() const
        {
            return m_SceneGathers;
        }
        // How many GatherScene calls moved kept primitives in place instead (the stream carried only Moved
        // changes, at least one of them of a primitive of the gather).
        [[nodiscard]] uint64_t GetSceneGatherMoveCount() const
        {
            return m_SceneGatherMoves;
        }
        // The primitives (GetPrimitives indices) whose World this frame's gather (GatherScene, or AddPasses'
        // kept TLAS gather) moved in place; empty on a frame that gathered in full or moved nothing.
        [[nodiscard]] std::span<const uint32_t> GetSceneMovedPrimitives() const
        {
            return m_SceneMoved;
        }

        // THE TLAS FOLLOWS THE CHANGE STREAM (GI-ALL-LIGHTS step 8): a TLAS frame keeps the last TLAS gather while
        // the GpuScene stream carries only moves since its cursor (the same test as GatherScene), the variants it
        // resolved still answer the same BLAS and the skinned queue is the same; the moved primitives' rows are
        // then patched in place (PatchRayTracingSceneRow) and the TLAS UPDATED from its last build. The instance
        // list is rebuilt on a full gather, on a change of the external primitives (landscape tiles: their
        // shape, records, opacity) and when a patched row would trace another BLAS (or a dynamic primitive gains
        // or loses its BLAS); the TLAS is BUILT then, when the GPU-written rows trace other BLASes, and after
        // @p updates in-place updates in a row (BVH quality: an update refits, never re-sorts;
        // Scalability GlobalIllumination.TlasUpdatesBeforeRebuild; 0 = build every frame).
        void SetTlasUpdatesBeforeRebuild( const uint32_t updates )
        {
            m_TlasUpdatesBeforeRebuild = updates;
        }
        // Since the view was made: frames whose TLAS was built, frames whose TLAS was updated in place, and rows
        // an update frame patched with other bytes (a moving primitive: one per frame it moves).
        [[nodiscard]] uint64_t GetTlasFullBuildCount() const
        {
            return m_TlasFullBuilds;
        }
        [[nodiscard]] uint64_t GetTlasUpdateCount() const
        {
            return m_TlasUpdates;
        }
        [[nodiscard]] uint64_t GetTlasMovedRowCount() const
        {
            return m_TlasMovedRows;
        }
        // Moves exactly when GetPrimitives / GetSegmentRecords / GetInstances were laid out anew (a full gather,
        // scene or TLAS, or an instance-list rebuild): a reader keyed on their indices (the emissive light list)
        // rebuilds then, and otherwise follows GetSceneMovedPrimitives.
        [[nodiscard]] uint64_t GetLayoutSerial() const
        {
            return m_LayoutSerial;
        }
        // This frame's TLAS traces a skinned primitive's skin-cache stream
        // (RayTracingPrimitive::DeformedPositions):
        // a reader holding that graph ref must take this frame's.
        [[nodiscard]] bool HasDeformedPositions() const
        {
            return m_FrameDeformed;
        }

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
        // This frame's gathered primitives (the TLAS instances' sources; a primitive with no BLAS is in the list
        // and not in the TLAS). Meaningful after AddPasses.
        [[nodiscard]] std::span<const RayTracingPrimitive> GetPrimitives() const
        {
            return m_Gather.Primitives;
        }
        // This frame's ONE per-segment table, both sides (meaningful after a built AddPasses): the segment records
        // (whose material each segment is) and their geometry records (record i of each is segment i, the row a
        // TLAS instance's SBT offset + a hit's geometry index names). The hit-lighting SBT is built from these
        // two (RayTracingHitGroupTable.hpp), so its hit record i is the same segment's.
        [[nodiscard]] std::span<const RayTracingSegmentRecord> GetSegmentRecords() const
        {
            return m_Gather.SegmentRecords;
        }
        [[nodiscard]] std::span<const RayTracingGeometryRecord> GetGeometryRecords() const
        {
            if ( !m_Metadata )
                return {};
            return m_Metadata->Metadata.Geometry;
        }

    private:
        std::unique_ptr<RayTracingSceneTarget>      m_Target;
        std::unique_ptr<RayTracingGpuInstanceRows>  m_GpuInstanceRows; // made on the first frame with a source
        RayTracingGatherResult                      m_Gather;        // CPU scratch, capacity kept
        // AddPasses' inputs of this frame, what GatherScene reads.
        const System::MeshRenderer* m_SceneMeshes      = nullptr;
        glm::vec3                   m_SceneViewOrigin{ 0.0f };
        float                       m_SceneLensScale   = 1.0f;
        uint64_t                    m_SceneGatherFrame = ~0ull;
        // The scene gather's cache (GatherScene), while m_GatherIsScene: the GpuScene stream m_Gather follows
        // (GpuScene::GetStreamId) and the serial it has read up to, its unsynced key (and this frame's scratch,
        // capacity kept), and whether an ISM of it culls by view.
        uint64_t                 m_SceneStream = 0;
        uint64_t                 m_SceneCursor = 0;
        std::vector<uint64_t>    m_SceneUnsyncedKey;
        std::vector<uint64_t>    m_SceneUnsyncedScratch;
        bool                     m_SceneViewCulls = false;
        bool                     m_GatherIsScene = false;
        uint64_t                 m_SceneGathers  = 0;
        uint64_t                 m_SceneGatherMoves = 0;
        // The kept scene gather's primitives by GPU-scene row (kinds with a CPU world), and this frame's moved
        // ones.
        std::unordered_multimap<uint32_t, uint32_t> m_ScenePrimitivesOfRow;
        std::vector<uint32_t>                       m_SceneMoved;
        // GatherScene: each Moved change of @p changes onto the kept primitives of its row (the GpuScene row's
        // first
        // instance, RayTracingPrimitive::GpuSceneRow); returns whether any primitive of the gather moved.
        bool MoveScenePrimitives( const GpuDriven::GpuScene& scene, std::span<const GpuDriven::PrimitiveChange> changes );
        // The changes since the kept gather's cursor when they keep it (only moves of meshed primitives, the same
        // stream, the same unsynced key), nullopt when it must be gathered anew. @p inputs: this frame's.
        [[nodiscard]] std::optional<std::span<const GpuDriven::PrimitiveChange>>
        KeptGatherChanges( const GpuDriven::GpuScene& scene, const RayTracingGatherInputs& inputs );
        // After a full gather of @p inputs: the rows map, the stream cursor and the unsynced key it follows from.
        void FollowGather( const GpuDriven::GpuScene& scene, const RayTracingGatherInputs& inputs );
        // AddPasses: whether the kept TLAS gather still answers this frame (KeptGatherChanges + the skinned queue
        // + every variant it resolved); fills @p changes when it does.
        [[nodiscard]] bool KeepTlasGather( const System::MeshRenderer& meshes, RayTracingSceneCache& cache,
                                           const RayTracingGatherInputs&                         inputs,
                                           std::optional<std::span<const GpuDriven::PrimitiveChange>>& changes );
        // AddPasses: whether @p external is the shape the instance list was built with (World aside: patched).
        [[nodiscard]] bool SameExternal( const RayTracingExternalPrimitives& external ) const;

        // The TLAS gather (AddPasses): m_Gather resolved BLASes (variants, skin cache, procedural) and carries the
        // external tail after m_GatherSceneCount primitives / m_GatherSceneRecords records; the scene's part is
        // what the stream follows. m_DynamicPrimitives: its skinned and procedural primitives, refilled each
        // frame.
        bool                  m_GatherHasBlas      = false;
        std::size_t           m_GatherSceneCount   = 0;
        std::size_t           m_GatherSceneRecords = 0;
        std::vector<uint32_t> m_DynamicPrimitives;
        // The external primitives the instance list was built with (SameExternal).
        std::vector<RayTracingPrimitive>                     m_LastExternalPrimitives;
        std::vector<RayTracingSegmentRecord>                 m_LastExternalRecords;
        std::vector<std::optional<RayTracingMaterialRecord>> m_LastLandscapeOpacity;
        uint64_t                                             m_LastGpuRowsKey = 0;
        // The instance list's own counts as built (BuildRayTracingInstances + the gather's skips), before the
        // GPU-written rows' Prepare adds theirs: a patch frame starts from them again.
        std::array<uint32_t, static_cast<std::size_t>( RayTracingSkip::Count )>          m_ListSkipped{};
        std::array<uint32_t, static_cast<std::size_t>( RayTracingPrimitiveKind::Count )> m_ListInstancesByKind{};
        bool                                                                             m_ListValid = false;
        RayTracingSceneRows                 m_SceneRows;        // the CPU rows of the last build, patched since
        std::optional<RayTracingMetadataBuild> m_Metadata;      // the kept metadata (rebuilt with the list)
        uint32_t                            m_TlasUpdatesBeforeRebuild = 0;
        uint32_t                            m_UpdatesSinceBuild        = 0;
        uint64_t                            m_TlasFullBuilds           = 0;
        uint64_t                            m_TlasUpdates              = 0;
        uint64_t                            m_TlasMovedRows            = 0;
        uint64_t                            m_LayoutSerial             = 0;
        bool                                m_FrameDeformed            = false;
        // An instance-list rebuild was declared and no build has followed it yet: the next frame builds too.
        bool                                m_BuildPending             = false;
        std::vector<RDG::BufferRef>                 m_DynamicBlas;   // CPU scratch: this frame's built BLASes
        std::vector<RayTracingDynamicGeometryBuild> m_DynamicBuilds; // CPU scratch: this frame's BLAS builds
        RayTracingSceneStats                        m_Stats;
        RayTracingInstanceList                      m_Instances;
        RayTracingSceneFrame                        m_Frame;
        // The metadata's persistent rows' upload state (AddRayTracingMetadataUpload) and the patched primitives
        // of this frame (CPU scratch, capacity kept).
        SceneRowUploadState   m_GeometryUpload;
        SceneRowUploadState   m_MaterialUpload;
        std::vector<uint32_t> m_MetadataPatched;
        // The distinct BLASes the list traces (RayTracingSceneStats::TracedBlasBytes), kept with the list and the
        // GPU rows' key they were collected for.
        std::vector<const RayTracingGeometryResource*> m_TracedGeometry;
        bool                                           m_TracedGeometryValid = false;
        uint64_t                                       m_TracedGpuRowsKey    = 0;
    };
} // namespace Desert::Graphic::RayTracing
