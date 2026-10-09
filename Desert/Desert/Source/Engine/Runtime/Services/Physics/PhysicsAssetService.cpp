#include "PhysicsAssetService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Logger.hpp>

#include <format>

namespace Desert::Runtime
{
    void PhysicsAssetService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    void PhysicsAssetService::Request( const Assets::AssetHandle& handle )
    {
        if ( m_Ready.contains( handle ) || m_Failed.contains( handle ) || m_Requests.contains( handle ) )
            return;

        const auto fail = [this, handle]( const std::string& why )
        {
            LOG_ERROR( "[Physics] Physics asset {} cannot be read: {}", static_cast<uint64_t>( handle ), why );
            m_Failed[handle] = why;
        };

        auto created = Assets::CreateFromRegistryRow<Assets::PhysicsAsset>(
             m_Assets, handle, Common::Content::ContentKind::PhysicsAsset );
        if ( !created )
        {
            fail( created.GetError() );
            return;
        }
        if ( created.GetValue()->IsReadyForUse() )
        {
            m_Ready[handle] = created.GetValue();
            return;
        }
        m_Requests[handle] = Assets::AsyncAssetLoader::Get().Request(
             created.GetValue(),
             [this, handle, fail]( const Assets::Asset<Assets::AssetBase>& loaded,
                                   const Assets::LoadOutcome outcome, const std::string& error )
             {
                 m_Requests.erase( handle );
                 if ( outcome == Assets::LoadOutcome::Loaded )
                     m_Ready[handle] = std::static_pointer_cast<Assets::PhysicsAsset>( loaded );
                 else
                     fail( error );
             },
             [this, handle] { m_Requests.erase( handle ); } );
    }

    Common::BoolResultStr PhysicsAssetService::CheckSkeleton( const Physics::PhysicsAssetData&  asset,
                                                              const Common::Content::AssetGuid& meshSkeleton,
                                                              std::string_view                  meshSkeletonName )
    {
        if ( meshSkeleton.IsNull() )
            return Common::MakeError<bool>( std::format(
                 "the mesh (rig '{}') names no skeleton asset, and physics asset {} is authored on skeleton {}",
                 meshSkeletonName, Common::Content::AssetGuidToText( asset.Guid ),
                 Common::Content::AssetGuidToText( asset.Skeleton ) ) );
        if ( asset.Skeleton != meshSkeleton )
            return Common::MakeError<bool>( std::format( "physics asset {} is authored on skeleton {}, but the "
                                                         "mesh plays on skeleton '{}' ({}); its bodies "
                                                         "name another rig's bones",
                                                         Common::Content::AssetGuidToText( asset.Guid ),
                                                         Common::Content::AssetGuidToText( asset.Skeleton ),
                                                         meshSkeletonName,
                                                         Common::Content::AssetGuidToText( meshSkeleton ) ) );
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<std::shared_ptr<const Physics::PhysicsAssetData>>
    PhysicsAssetService::Get( const Assets::AssetHandle& handle, const Common::Content::AssetGuid& meshSkeleton,
                              std::string_view meshSkeletonName )
    {
        using Result = std::shared_ptr<const Physics::PhysicsAssetData>;
        Request( handle );
        if ( const auto failed = m_Failed.find( handle ); failed != m_Failed.end() )
            return Common::MakeError<Result>( failed->second );
        const auto found = m_Ready.find( handle );
        if ( found == m_Ready.end() )
            return Common::MakeSuccess( Result{} );
        const Physics::PhysicsAssetData& data  = found->second->GetData();
        auto                             match = CheckSkeleton( data, meshSkeleton, meshSkeletonName );
        if ( !match.IsSuccess() )
            return Common::MakeError<Result>( match.GetError() );
        // Aliasing: the pointer keeps the asset, and so its data, alive.
        return Common::MakeSuccess( Result( found->second, &data ) );
    }

    void PhysicsAssetService::Clear()
    {
        m_Requests.clear();
        m_Ready.clear();
        m_Failed.clear();
    }
} // namespace Desert::Runtime
