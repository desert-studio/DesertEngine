#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneRenderer.hpp>

#include <Engine/Core/FrameManager.hpp>
#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingGeometry.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingScene.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingGather.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>

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
        const auto firstSkinned =
             static_cast<uint32_t>( m_Gather.Primitives.size() - meshes.GetSkinnedQueue().size() );
        if ( const Common::BoolResultStr skinned =
                  cache.GetSkinCache().AddPasses( graph, meshes, m_Gather.Primitives, firstSkinned, cache.GetFrame(),
                                                  m_DynamicBlas );
             !skinned )
            return skinned;
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
        return BOOLSUCCESS;
    }
} // namespace Desert::Graphic::RayTracing
