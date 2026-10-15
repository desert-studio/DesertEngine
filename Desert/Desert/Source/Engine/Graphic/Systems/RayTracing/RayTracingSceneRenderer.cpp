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
        return []( Mesh&          mesh,
                   const uint64_t hiddenMask ) -> Common::ResultStr<std::shared_ptr<RayTracingGeometryResource>>
        {
            auto built =
                 API::Vulkan::VulkanRayTracingGeometry::BuildStatic( DescribeStaticMeshBlas( mesh, hiddenMask ) );
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

        // Everything the raster queued, frustum-independent (RayTracingGather.hpp).
        GatherRayTracingPrimitives( { .Static              = meshes.GetStaticQueue(),
                                      .Generic             = meshes.GetGenericQueue(),
                                      .Instanced           = meshes.GetInstancedQueue(),
                                      .InstancedMotionRows = meshes.GetInstancedMotionRows(),
                                      .Skinned             = meshes.GetSkinnedQueue(),
                                      .ViewOrigin          = viewOrigin,
                                      .DrawDistanceScale   = drawDistanceScale },
                                    cache.GetVariants(), m_Gather );
        // Skinned primitives trace their per-frame BLAS: the scene's skin cache skins and builds each once per
        // frame (an earlier view's build is reused). The gather appends one skinned primitive per skinned record,
        // last.
        m_DynamicBlas.clear();
        m_DynamicBuilds.clear();
        const auto firstSkinned =
             static_cast<uint32_t>( m_Gather.Primitives.size() - meshes.GetSkinnedQueue().size() );
        if ( const Common::BoolResultStr skinned =
                  cache.GetSkinCache().AddPasses( graph, meshes, m_BonesImport, m_Gather.Primitives, firstSkinned,
                                                  cache.GetFrame(), m_DynamicBuilds, m_DynamicBlas );
             !skinned )
            return skinned;
        // Meshes the uploader built no BLAS for (procedural): built in the same pass, once per buffer upload.
        if ( const Common::BoolResultStr procedural = cache.GetProcedural().AddBuilds(
                  graph, m_Gather.Primitives, cache.GetFrame(), m_DynamicBuilds, m_DynamicBlas );
             !procedural )
            return procedural;
        // Primitives other renderers produced (landscape tiles), after the skinned ones (firstSkinned above counts
        // from the end): their BLAS requests join the same pass, and their BLASes built in an earlier frame are
        // read by the TLAS like every other dynamic BLAS.
        m_Gather.Primitives.insert( m_Gather.Primitives.end(), external.Primitives.begin(),
                                    external.Primitives.end() );
        m_DynamicBuilds.insert( m_DynamicBuilds.end(), external.Builds.begin(), external.Builds.end() );
        m_DynamicBlas.insert( m_DynamicBlas.end(), external.Resident.begin(), external.Resident.end() );
        // ONE pass builds every dynamic BLAS of the view (skin cache, procedural, landscape tiles).
        if ( const Common::BoolResultStr built =
                  AddRayTracingDynamicGeometryPass( graph, m_DynamicBuilds, m_DynamicBlas );
             !built )
            return built;
        m_Instances = BuildRayTracingInstances( m_Gather.Primitives );
        for ( std::size_t reason = 0; reason < m_Instances.Skipped.size(); ++reason )
            m_Instances.Skipped[reason] += m_Gather.Skipped[reason];

        if ( !m_Target )
            m_Target = CreateRayTracingSceneTarget();
        const Common::ResultStr<RayTracingSceneFrame> frame =
             AddRayTracingScenePass( graph, required, m_Instances, *m_Target, m_DynamicBlas );
        if ( !frame )
            return Common::MakeError( frame.GetError() );
        m_Frame = frame.GetValue();

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
        m_Stats.TlasBytes                           = m_Target->GetStorage().Desc.Bytes;
        m_Stats.TlasInstanceBytes =
             static_cast<uint64_t>( std::max<std::size_t>( m_Instances.Instances.size(), 1u ) ) *
             kInstanceRowBytes;
        m_Stats.PrimitiveInstanceBytes =
             static_cast<uint64_t>( std::max<std::size_t>( m_Instances.Ranges.size(), 1u ) ) *
             kPrimitiveInstancesRowBytes;
        m_Stats.SkinCacheEntries     = static_cast<uint32_t>( cache.GetSkinCache().GetEntryCount() );
        m_Stats.LandscapeBuilt       = external.Built;
        m_Stats.LandscapeRefit       = external.Refit;
        m_Stats.LandscapeVertexBytes = external.VertexBytes;
        m_Stats.LandscapeBlasBytes   = external.BlasBytes;
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::RayTracing
