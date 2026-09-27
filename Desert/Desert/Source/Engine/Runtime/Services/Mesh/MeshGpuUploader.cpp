#include "MeshUploader.hpp"

#include <Engine/Geometry/MeshFactory.hpp>

namespace Desert::Runtime
{
    namespace
    {
        // The two device buffers a mesh IS, named as the asset's. A procedural mesh is claimed differently
        // in UploadProcedural: it has no file, so nothing may ever release it. See ResourceLedger.hpp.
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

            Common::BoolResultStr UploadProcedural( const std::shared_ptr<Mesh>& mesh,
                                                    const Assets::AssetHandle&   handle ) override
            {
                // `Procedural`, NOT `AssetService`, and the distinction is load-bearing rather than cosmetic:
                // this mesh was built from no file, so there is no recipe to rebuild it from and releasing it
                // is data loss. The ledger's owner category is what asset eviction reads to know it must not
                // touch this. Claimed even when the upload failed, as before: the ledger names what exists.
                const auto uploaded = mesh->Invalidate();
                ClaimMeshBuffers( mesh, Graphic::ResourceOwner::Procedural, handle );
                if ( !uploaded.IsSuccess() )
                    return Common::MakeError( uploaded.GetError() );
                return BOOLSUCCESS;
            }
        };
    } // namespace

    std::unique_ptr<IMeshUploader> MakeGpuMeshUploader()
    {
        return std::make_unique<GpuMeshUploader>();
    }
} // namespace Desert::Runtime
