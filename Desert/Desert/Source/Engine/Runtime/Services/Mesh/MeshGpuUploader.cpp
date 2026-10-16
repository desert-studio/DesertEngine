#include "MeshUploader.hpp"

#include <Common/Core/Logger.hpp>
#include <Engine/Geometry/MeshFactory.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingGeometry.hpp>
#include <Engine/Graphic/Render/RayTracingMeshInput.hpp>

namespace Desert::Runtime
{
    namespace
    {
        // The two device buffers a mesh IS, named as the asset's. See ResourceLedger.hpp.
        void ClaimMeshBuffers( const std::shared_ptr<Mesh>& mesh, const Graphic::ResourceOwner owner,
                               const Assets::AssetHandle& asset )
        {
            if ( !mesh )
                return;
            if ( const auto& vertices = mesh->GetVertexBuffer() )
            {
                vertices->ClaimOwnership( owner, asset );
                vertices->RecordDeviceBytes( vertices->GetSize() );
            }
            if ( const auto& indices = mesh->GetIndexBuffer() )
            {
                indices->ClaimOwnership( owner, asset );
                indices->RecordDeviceBytes( indices->GetSize() );
            }
        }

        class GpuMeshUploader final : public IMeshUploader
        {
        public:
            std::shared_ptr<Mesh> Upload( const std::shared_ptr<Assets::MeshAsset>& asset ) override
            {
                auto mesh = Graphic::MeshFactory::Create( asset );
                ClaimMeshBuffers( mesh, Graphic::ResourceOwner::AssetService, asset->GetMetadata().Handle );
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
