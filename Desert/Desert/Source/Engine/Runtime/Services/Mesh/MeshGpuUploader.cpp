#include "MeshUploader.hpp"

#include <Common/Core/Logger.hpp>
#include <Engine/Core/EngineContext.hpp>
#include <Engine/Geometry/MeshFactory.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingGeometry.hpp>
#include <Engine/Graphic/Render/RayTracingMeshInput.hpp>

#include <algorithm>
#include <mutex>

namespace Desert::Runtime
{
    namespace
    {
        class GpuMeshUploader final : public IMeshUploader
        {
        public:
            std::shared_ptr<Mesh> Upload( const std::shared_ptr<Assets::MeshAsset>& asset ) override
            {
                auto mesh = Graphic::MeshFactory::Create( asset );
                BuildRayTracingGeometry( mesh, *asset );
                return mesh;
            }

        private:
            // The BLAS is built where the mesh becomes resident, from the buffers just uploaded; it is released
            // with the mesh (RetireEvicted). A failed build keeps the mesh drawable and says so: the TLAS counts
            // it as NoGeometry, so rays miss it visibly rather than silently.
            static void BuildRayTracingGeometry( const std::shared_ptr<Mesh>& mesh,
                                                 const Assets::MeshAsset&     asset )
            {
                if ( !mesh )
                    return;
                // A device without ray tracing (none in hardware, or hidden by machine.json HideRayTracing) has no
                // acceleration structures: no BLAS is asked for, and the start says so once. Every RT consumer
                // reads the same catalog, so nothing downstream expects one (RayTracingSceneRequired is false).
                const Engine::DeviceCapabilities& capabilities = EngineContext::GetInstance().GetCapabilities();
                const auto& modes = capabilities.Catalog.RayTracingModes;
                if ( std::none_of( modes.begin(), modes.end(), []( Common::Scalability::RayTracingMode m )
                                   { return m != Common::Scalability::RayTracingMode::None; } ) )
                {
                    static std::once_flag s_Logged;
                    std::call_once( s_Logged,
                                    [&capabilities]
                                    {
                                        LOG_INFO( "[MeshService] no BLAS is built for meshes: ray tracing is {} on "
                                                  "this device",
                                                  capabilities.RayTracingHidden
                                                       ? "hidden by machine.json (HideRayTracing)"
                                                       : "not offered" );
                                    } );
                    return;
                }
                auto built = Graphic::API::Vulkan::VulkanRayTracingGeometry::BuildStatic(
                     Graphic::RayTracing::DescribeStaticMeshBlas( *mesh, {} ) );
                if ( !built.IsSuccess() )
                {
                    LOG_ERROR( "[MeshService] BLAS of '{}' not built: {}", asset.GetMetadata().Filepath.string(),
                               built.GetError() );
                    return;
                }
                if ( built.GetValue() )
                    built.GetValue()->ClaimOwnership( Graphic::ResourceOwner::AssetService,
                                                      asset.GetMetadata().Handle );
                mesh->SetRayTracingGeometry( built.GetValue() );
            }
        };
    } // namespace

    std::unique_ptr<IMeshUploader> MakeGpuMeshUploader()
    {
        return std::make_unique<GpuMeshUploader>();
    }
} // namespace Desert::Runtime
