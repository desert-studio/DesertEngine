#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneRenderer.hpp>

#include <Engine/Geometry/StaticMesh.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingScene.hpp>
#include <Engine/Graphic/Systems/Scene/Mesh/MeshRenderer.hpp>

namespace Desert::Graphic::RayTracing
{
    std::unique_ptr<RayTracingSceneTarget> CreateRayTracingSceneTarget()
    {
        return std::make_unique<API::Vulkan::VulkanRayTracingScene>();
    }

    Common::BoolResultStr RayTracingSceneRenderer::AddPasses( RDG::Builder& graph, bool required,
                                                              const System::MeshRenderer& meshes )
    {
        m_Frame     = {};
        m_Instances = {};
        if ( !required )
            return BOOLSUCCESS;

        // Static and skinned queues. A skinned primitive traces its per-frame BLAS, which the skin cache skins and
        // builds below; a generic (procedural) mesh has no BLAS builder yet and is not a candidate (RT-SCENE-2).
        m_Primitives.clear();
        for ( const System::MeshRenderer::StaticMeshRenderData& data : meshes.GetStaticQueue() )
        {
            RayTracingPrimitive& primitive = m_Primitives.emplace_back();
            primitive.Geometry             = data.Mesh ? data.Mesh->GetRayTracingGeometry().get() : nullptr;
            primitive.World                = data.Transform;
            primitive.GpuSceneRow          = data.MotionRow;
            primitive.CastShadows          = data.CastShadows;
            primitive.HiddenSubmeshes      = data.HiddenSubmeshes;
        }
        for ( const System::MeshRenderer::SkinnedMeshRenderData& data : meshes.GetSkinnedQueue() )
        {
            RayTracingPrimitive& primitive = m_Primitives.emplace_back();
            primitive.World                = data.Transform;
            primitive.GpuSceneRow          = data.MotionRow;
            primitive.Skinned              = true;
            primitive.DeformedMesh         = data.Mesh;
        }
        m_DynamicBlas.clear();
        const auto firstSkinned = static_cast<uint32_t>( meshes.GetStaticQueue().size() );
        if ( const Common::BoolResultStr skinned =
                  m_SkinCache.AddPasses( graph, meshes, m_Primitives, firstSkinned, m_DynamicBlas );
             !skinned )
            return skinned;
        m_Instances = BuildRayTracingInstances( m_Primitives );

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
