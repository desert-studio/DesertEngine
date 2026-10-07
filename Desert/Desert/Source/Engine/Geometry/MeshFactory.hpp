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
                return CreateStatic( asset );
            }
        }

    private:
        static std::shared_ptr<Mesh> CreateStatic( const std::shared_ptr<Assets::MeshAsset>& baseAsset )
        {
            auto asset = std::dynamic_pointer_cast<Assets::StaticMeshAsset>( baseAsset );

            if ( !asset )
            {
                LOG_ERROR( "MeshFactory: Asset is not StaticMeshAsset" );
                return nullptr;
            }

            const auto staticMesh = std::make_shared<StaticMesh>(
                 asset->GetVertices(), asset->GetIndices(), asset->GetSubmeshes(), asset->GetVertexStreams() );
            if ( !staticMesh->Invalidate() )
            {
                DESERT_VERIFY( false );
            }

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
                                                skeletonRT, asset->GetVertexStreams() );
            if ( !skinnedMesh->Invalidate() )
            {
                DESERT_VERIFY( false );
            }

            return skinnedMesh;
        }
    };
} // namespace Desert::Graphic