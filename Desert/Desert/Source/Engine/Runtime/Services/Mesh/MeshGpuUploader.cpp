#include "MeshUploader.hpp"

#include <Common/Core/Logger.hpp>
#include <Engine/Geometry/MeshFactory.hpp>

namespace Desert::Runtime
{
    namespace
    {
        class GpuMeshUploader final : public IMeshUploader
        {
        public:
            std::shared_ptr<Mesh> Stage( const std::shared_ptr<Assets::MeshAsset>& asset ) override
            {
                return Graphic::MeshFactory::Stage( asset );
            }

            bool Commit( const std::shared_ptr<Mesh>& mesh, const Assets::MeshAsset& asset ) override
            {
                if ( !mesh )
                    return false;
                // A static mesh's geometry is staged by Stage and lands in the geometry pool here; a skinned one
                // was built whole by Stage (MeshFactory::Stage says why).
                // No BLAS is built here: the ray-tracing scene builds a mesh's BLAS on the first frame a traced row
                // asks for it, at that row's RT-LOD, and releases it when no row asks (RayTracingGeometryVariants).
                if ( !asset.IsSkinned() )
                {
                    if ( const auto uploaded = mesh->Invalidate(); !uploaded.IsSuccess() )
                    {
                        LOG_ERROR( "[MeshService] '{}': the staged geometry was not uploaded: {}",
                                   asset.GetMetadata().Filepath.string(), uploaded.GetError() );
                        return false;
                    }
                }
                return true;
            }
        };
    } // namespace

    std::unique_ptr<IMeshUploader> MakeGpuMeshUploader()
    {
        return std::make_unique<GpuMeshUploader>();
    }
} // namespace Desert::Runtime
