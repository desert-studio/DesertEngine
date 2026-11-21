#pragma once

#include "SkinnedMesh.hpp"
#include "StaticMesh.hpp"
#include "DynamicMesh.hpp"

#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Assets/Mesh/SkinnedMeshAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>

namespace Desert::Graphic
{
    class MeshFactory
    {
    public:
        static std::shared_ptr<Mesh> CreateProcedural( const std::vector<Vertex>&  vertices,
                                                       const std::vector<Index>&   indices,
                                                       const std::vector<Submesh>& submeshes )
        {
            auto mesh = std::make_shared<DynamicMesh>( vertices, indices, submeshes );

            if ( !mesh->Invalidate() )
            {
                DESERT_VERIFY( false );
            }

            return mesh;
        }

        static std::shared_ptr<Mesh> Create( const std::shared_ptr<Assets::MeshAsset>& asset )
        {
            if ( !asset )
                return nullptr;

            if ( asset->IsSkinned() )
            {
                return CreateSkinned( asset );
            }
            else
            {
                return CreateStatic( asset, true );
            }
        }

        // THE CPU HALF OF Create, SAFE OFF THE GAME THREAD for a static mesh (UE: FStaticMeshRenderData built on the
        // async loading thread, the RHI resources initialised later): the LOD chain, the screen coverage and the
        // staged host copy are built, nothing touches the device. The mesh draws once Invalidate() uploads what
        // was staged (MeshService's Commit). A skinned mesh is built whole, as Create does: its device half is not
        // split (its skin cache binds the skeleton at construction).
        static std::shared_ptr<Mesh> Stage( const std::shared_ptr<Assets::MeshAsset>& asset )
        {
            if ( !asset )
                return nullptr;
            return asset->IsSkinned() ? CreateSkinned( asset ) : CreateStatic( asset, false );
        }

    private:
        static std::shared_ptr<Mesh> CreateStatic( const std::shared_ptr<Assets::MeshAsset>& baseAsset,
                                                   const bool                                upload )
        {
            auto asset = std::dynamic_pointer_cast<Assets::StaticMeshAsset>( baseAsset );

            if ( !asset )
            {
                LOG_ERROR( "MeshFactory: Asset is not StaticMeshAsset" );
                return nullptr;
            }

            asset->AcquireCpuGeometryForUpload(); // a rebuild after ReleaseCpuGeometry reads the arrays back
            const auto staticMesh = std::make_shared<StaticMesh>(
                 asset->GetVertices(), asset->GetIndices(), asset->GetSubmeshes(), asset->GetVertexStreams(),
                 VirtualGeometrySource{ asset->GetClusterDags(), asset->GetMetadata().Filepath.string() } );
            if ( upload && !staticMesh->Invalidate() )
            {
                DESERT_VERIFY( false );
            }
            staticMesh->SetDistanceField( asset->GetDistanceField() );
            staticMesh->SetCards( asset->GetCards() );
            staticMesh->SetDisplacementTopology( asset->GetDisplacementTopology() );
            staticMesh->SetDisplacedMesh( asset->GetDisplacedMesh() );
            // The GPU copy is staged (StageGeometry copied the bytes): the asset's CPU arrays are released, UE
            // bAllowCPUAccess=false. The distance-field/cards/displacement job took its copy in
            // ResolveDependencies.
            asset->ReleaseCpuGeometry();

            return staticMesh;
        }

        static std::shared_ptr<Mesh> CreateSkinned( const std::shared_ptr<Assets::MeshAsset>& baseAsset )
        {
            auto asset = std::dynamic_pointer_cast<Assets::SkinnedMeshAsset>( baseAsset );

            if ( !asset )
            {
                LOG_ERROR( "MeshFactory: Asset is not SkinnedMeshAsset" );
                return nullptr;
            }
            const auto skeletonAsset = asset->GetSkeletonDependency().Get();
            if ( !skeletonAsset )
            {
                LOG_ERROR( "MeshFactory: Skeleton dependency invalid" );
                return nullptr;
            }

            const Animation::Skeleton* skeletonRT = skeletonAsset->GetSkeleton();

            if ( !skeletonRT )
            {
                LOG_ERROR( "MeshFactory: Skeleton runtime object is null" );
                return nullptr;
            }

            const auto skinnedMesh =
                 std::make_shared<SkinnedMesh>( asset->GetVertices(), asset->GetIndices(), asset->GetSubmeshes(),
                                                skeletonRT, asset->GetVertexStreams(), asset->GetMorphTargets() );
            if ( !skinnedMesh->Invalidate() )
            {
                DESERT_VERIFY( false );
            }
            // Runtime displacement over the skin cache's posed vertices (CONTRACT-DISPLACE §9.8): null slot = none.
            skinnedMesh->SetDisplacementTopology( asset->GetDisplacementTopology() );

            return skinnedMesh;
        }
    };
} // namespace Desert::Graphic