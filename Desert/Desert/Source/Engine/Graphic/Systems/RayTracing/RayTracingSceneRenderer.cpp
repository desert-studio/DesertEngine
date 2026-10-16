#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneRenderer.hpp>

#include <Engine/Core/FrameManager.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingGeometry.hpp>
#include <Engine/Graphic/Render/RayTracingMeshInput.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGather.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <unordered_set>

namespace Desert::Graphic::RayTracing
{
    RayTracingGeometryVariants::BuildFn MakeVariantBuild()
    {
        return []( Mesh& mesh, const RayTracingSubmeshMasks& masks )
                    -> Common::ResultStr<std::shared_ptr<RayTracingGeometryResource>>
        {
            auto built =
                 API::Vulkan::VulkanRayTracingGeometry::BuildStatic( DescribeStaticMeshBlas( mesh, masks ) );
            if ( !built )
                return Common::MakeError<std::shared_ptr<RayTracingGeometryResource>>( built.GetError() );
            return Common::MakeSuccess( std::shared_ptr<RayTracingGeometryResource>( built.GetValue() ) );
        };
    }

    std::string FormatRayTracingSceneMemory( const RayTracingSceneStats& stats )
    {
        const auto mib = []( const uint64_t bytes ) { return static_cast<double>( bytes ) / ( 1024.0 * 1024.0 ); };
        std::string text;
        auto        out = std::back_inserter( text );
        std::format_to( out, "ray tracing scene\n" );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "TLAS", mib( stats.TlasBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "TLAS instance rows", mib( stats.TlasInstanceBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "primitive instance ranges",
                        mib( stats.PrimitiveInstanceBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "TLAS build scratch", mib( stats.TlasScratchBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "dynamic BLAS scratch", mib( stats.DynamicScratchBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "traced BLAS", mib( stats.TracedBlasBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "variant BLAS (scene)", mib( stats.VariantBlasBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "procedural BLAS (scene)",
                        mib( stats.ProceduralBlasBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB  {} entries\n", "skin cache BLAS (scene)",
                        mib( stats.SkinCacheBlasBytes ), stats.SkinCacheEntries );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "skin cache positions",
                        mib( stats.SkinCachePositionsBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "landscape vertices", mib( stats.LandscapeVertexBytes ) );
        std::format_to( out, "  {:<26} {:9.2f} MiB\n", "landscape BLAS", mib( stats.LandscapeBlasBytes ) );
        return text;
    }

    Common::BoolResultStr RayTracingSceneRenderer::AddPasses(
         RDG::Builder& graph, bool required, const System::MeshRenderer& meshes, RayTracingSceneCache& cache,
         const glm::vec3& viewOrigin, const float drawDistanceScale, const RayTracingExternalPrimitives& external )
    {
        m_Frame     = {};
        m_Instances = {};
        m_Stats     = {};
        if ( !required )
            return BOOLSUCCESS;

        // The scene's shared geometry ends its previous frame on the first view of this one
        // (RayTracingSceneCache).
        cache.BeginViewFrame( Engine::FrameManager::GetInstance().GetAbsoluteFrameCount() );

        m_DynamicBlas.clear();
        m_DynamicBuilds.clear();
        {
            // Every device object the scene's cache makes below (BLAS variants, skin-cache BLASes and positions,
            // procedural BLASes) is a row of the SCENE, not of this view (RT-LEDGER-2): shared by every view, so
            // in no view's budget. The view's own scope (its TLAS below, its landscape tiles) resumes after.
            const ResourceAttributionScope sceneRows = cache.OpenLedgerScope();

            // Everything the raster queued, frustum-independent (RayTracingGather.hpp).
            GatherRayTracingPrimitives( { .Static              = meshes.GetStaticQueue(),
                                          .Generic             = meshes.GetGenericQueue(),
                                          .Instanced           = meshes.GetInstancedQueue(),
                                          .InstancedMotionRows = meshes.GetInstancedMotionRows(),
                                          .Skinned             = meshes.GetSkinnedQueue(),
                                          .ViewOrigin          = viewOrigin,
                                          .DrawDistanceScale   = drawDistanceScale },
                                        cache.GetVariants(), m_Gather );
            // Skinned primitives trace their per-frame BLAS, built from the positions the scene's skin step
            // (RayTracingSkinCache::SkinFrame) already wrote this frame - no skinning here; an earlier view's build
            // is reused. The gather appends one skinned primitive per skinned record, last.
            const auto firstSkinned =
                 static_cast<uint32_t>( m_Gather.Primitives.size() - meshes.GetSkinnedQueue().size() );
            if ( const Common::BoolResultStr skinned = cache.GetSkinCache().AddPasses(
                      graph, meshes, m_Gather.Primitives, firstSkinned, cache.GetFrame(), m_DynamicBuilds,
                      m_DynamicBlas );
                 !skinned )
                return skinned;
            // Meshes the uploader built no BLAS for (procedural): built in the same pass, once per buffer upload.
            if ( const Common::BoolResultStr procedural = cache.GetProcedural().AddBuilds(
                      graph, m_Gather.Primitives, cache.GetFrame(), m_DynamicBuilds, m_DynamicBlas );
                 !procedural )
                return procedural;
        }
        // Primitives other renderers produced (landscape tiles), after the skinned ones (firstSkinned above counts
        // from the end): their BLAS requests join the same pass, and their BLASes built in an earlier frame are
        // read by the TLAS like every other dynamic BLAS.
        const std::size_t firstExternal      = m_Gather.Primitives.size();
        const auto        externalRecordBase = static_cast<uint32_t>( m_Gather.SegmentRecords.size() );
        m_Gather.Primitives.insert( m_Gather.Primitives.end(), external.Primitives.begin(),
                                    external.Primitives.end() );
        m_Gather.SegmentRecords.insert( m_Gather.SegmentRecords.end(), external.SegmentRecords.begin(),
                                        external.SegmentRecords.end() );
        for ( std::size_t i = firstExternal; i < m_Gather.Primitives.size(); ++i )
            m_Gather.Primitives[i].FirstSegmentRecord += externalRecordBase;
        m_DynamicBuilds.insert( m_DynamicBuilds.end(), external.Builds.begin(), external.Builds.end() );
        m_DynamicBlas.insert( m_DynamicBlas.end(), external.Resident.begin(), external.Resident.end() );
        // ONE pass builds every dynamic BLAS of the view (skin cache, procedural, landscape tiles).
        const Common::ResultStr<uint64_t> dynamicScratch =
             AddRayTracingDynamicGeometryPass( graph, m_DynamicBuilds, m_DynamicBlas );
        if ( !dynamicScratch )
            return Common::MakeError( dynamicScratch.GetError() );
        m_Instances = BuildRayTracingInstances( m_Gather.Primitives );
        for ( std::size_t reason = 0; reason < m_Instances.Skipped.size(); ++reason )
            m_Instances.Skipped[reason] += m_Gather.Skipped[reason];

        // The GPU twin of the segment records: record i of SegmentRecords becomes geometry record i, the row an
        // instance's SBT offset (its FirstSegmentRecord) + a hit's geometry index names. No material declares an
        // opacity source the metadata can read yet (REMAINDER RT-UNIFY): every alpha-tested segment is counted in
        // SegmentsWithoutOpacity, and its candidates are confirmed as hits.
        const Common::ResultStr<RayTracingMetadataBuild> metadata = BuildRayTracingMetadata(
             m_Gather.Primitives, m_Gather.SegmentRecords,
             []( const RayTracingSegmentRecord& ) -> std::optional<RayTracingMaterialRecord> { return std::nullopt; } );
        if ( !metadata )
            return Common::MakeError( metadata.GetError() );

        if ( !m_Target )
            m_Target = CreateRayTracingSceneTarget();
        const Common::ResultStr<RayTracingSceneFrame> frame =
             AddRayTracingScenePass( graph, required, m_Instances, *m_Target, m_DynamicBlas );
        if ( !frame )
            return Common::MakeError( frame.GetError() );
        m_Frame = frame.GetValue();
        if ( m_Frame.Built )
            m_Frame.Metadata = AddRayTracingMetadataUpload( graph, metadata.GetValue().Metadata );

        m_Stats.Built           = m_Frame.Built;
        m_Stats.InstancesByKind = m_Instances.InstancesByKind;
        m_Stats.SkippedByReason = m_Instances.Skipped;
        std::unordered_set<const void*> traced;
        for ( const RayTracingInstance& instance : m_Instances.Instances )
            if ( traced.insert( instance.Geometry ).second )
                m_Stats.TracedBlasBytes +=
                     static_cast<const RayTracingGeometryResource*>( instance.Geometry )->GetMemoryBytes();
        m_Stats.VariantBlasBytes                    = cache.GetVariants().GetMemoryBytes();
        m_Stats.ProceduralBlasBytes                 = cache.GetProcedural().GetMemoryBytes();
        const RayTracingSkinCache::MemoryBytes skin = cache.GetSkinCache().GetMemoryBytes();
        m_Stats.SkinCacheBlasBytes                  = skin.Blas;
        m_Stats.SkinCachePositionsBytes             = skin.Positions;
        m_Stats.TlasBytes                           = m_Target->GetMemoryBytes();
        m_Stats.TlasInstanceBytes                   = m_Frame.InstanceBytes;
        m_Stats.PrimitiveInstanceBytes              = m_Frame.PrimitiveInstanceBytes;
        m_Stats.TlasScratchBytes                    = m_Frame.ScratchBytes;
        m_Stats.SegmentsWithoutOpacity              = metadata.GetValue().SegmentsWithoutOpacity;
        m_Stats.DynamicScratchBytes                 = dynamicScratch.GetValue();
        m_Stats.SkinCacheEntries     = static_cast<uint32_t>( cache.GetSkinCache().GetEntryCount() );
        m_Stats.LandscapeBuilt       = external.Built;
        m_Stats.LandscapeRefit       = external.Refit;
        m_Stats.LandscapeVertexBytes = external.VertexBytes;
        m_Stats.LandscapeBlasBytes   = external.BlasBytes;
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::RayTracing
