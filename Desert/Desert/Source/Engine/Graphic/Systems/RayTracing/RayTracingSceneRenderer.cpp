#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneRenderer.hpp>

#include <Engine/Core/FrameManager.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingGeometry.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingScene.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGather.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>

#include <algorithm>
#include <unordered_set>

namespace Desert::Graphic::RayTracing
{
    std::unique_ptr<RayTracingSceneTarget> CreateRayTracingSceneTarget()
    {
        return std::make_unique<API::Vulkan::VulkanRayTracingScene>();
    }

    RayTracingGeometryVariants::BuildFn MakeVariantBuild()
    {
        return []( Mesh& mesh, const uint64_t hiddenMask )
                    -> Common::ResultStr<std::shared_ptr<RayTracingGeometryResource>>
        {
            auto built = API::Vulkan::VulkanRayTracingGeometry::BuildStatic( mesh, hiddenMask );
            if ( !built )
                return Common::MakeError<std::shared_ptr<RayTracingGeometryResource>>( built.GetError() );
            return Common::MakeSuccess( std::shared_ptr<RayTracingGeometryResource>( built.GetValue() ) );
        };
    }

    Common::BoolResultStr RayTracingSceneRenderer::AddPasses( RDG::Builder& graph, bool required,
                                                              const System::MeshRenderer& meshes,
                                                              RayTracingSceneCache&       cache,
                                                              const glm::vec3&            viewOrigin,
                                                              const float                 drawDistanceScale )
    {
        m_Frame     = {};
        m_Instances = {};
        m_Stats     = {};
        if ( !required )
            return BOOLSUCCESS;

        // The scene's shared geometry ends its previous frame on the first view of this one (RayTracingSceneCache).
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
        // Skinned primitives trace their per-frame BLAS: the scene's skin cache skins and builds each once per frame
        // (an earlier view's build is reused). The gather appends one skinned primitive per skinned record, last.
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
        // ONE pass builds every dynamic BLAS of the view (skin cache, procedural; landscape tiles join it).
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
        m_Stats.VariantBlasBytes                   = cache.GetVariants().GetMemoryBytes();
        m_Stats.ProceduralBlasBytes                = cache.GetProcedural().GetMemoryBytes();
        const RayTracingSkinCache::MemoryBytes skin = cache.GetSkinCache().GetMemoryBytes();
        m_Stats.SkinCacheBlasBytes                 = skin.Blas;
        m_Stats.SkinCachePositionsBytes            = skin.Positions;
        m_Stats.TlasBytes                          = m_Target->GetStorage().Desc.Bytes;
        m_Stats.TlasInstanceBytes =
             static_cast<uint64_t>( std::max<std::size_t>( m_Instances.Instances.size(), 1u ) ) * kInstanceRowBytes;
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::RayTracing
