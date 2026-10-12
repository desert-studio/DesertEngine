#include "MeshUploader.hpp"

#include <Engine/Geometry/MeshFactory.hpp>

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
                return mesh;
            }
        };
    } // namespace

    std::unique_ptr<IMeshUploader> MakeGpuMeshUploader()
    {
        return std::make_unique<GpuMeshUploader>();
    }
} // namespace Desert::Runtime
