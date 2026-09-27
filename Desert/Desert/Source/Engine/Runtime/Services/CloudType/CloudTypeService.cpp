#include "CloudTypeService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    Common::BoolResultStr CloudTypeService::Register( const std::shared_ptr<Assets::CloudTypeAsset>& asset )
    {
        if ( !asset )
            return Common::MakeError( "CloudTypeService::Register was given a null asset" );

        if ( !asset->IsReadyForUse() )
            return Common::MakeFormattedError<bool>( "cloud type '{}' is not loaded",
                                                     asset->GetMetadata().Filepath.string() );

        const Assets::AssetHandle& handle = asset->GetMetadata().Handle;

        if ( auto it = m_Types.find( handle ); it != m_Types.end() && it->second.Revision == asset->GetRevision() )
            return BOOLSUCCESS;

        m_Types[handle] = Entry{ asset->GetShape(), asset->GetNoiseVolume(), asset->GetRevision() };
        ++m_Generation;

        LOG_INFO( "[Clouds] Cloud type '{}' registered as {} ({}).", asset->GetDisplayName(),
                  static_cast<uint64_t>( handle ), asset->GetMetadata().Filepath.filename().string() );
        return BOOLSUCCESS;
    }

    void CloudTypeService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    const CloudTypeService::Entry* CloudTypeService::FindOrRequest( const Assets::AssetHandle& handle )
    {
        if ( handle == 0 )
            return nullptr;
        if ( auto it = m_Types.find( handle ); it != m_Types.end() )
            return &it->second;
        // Said once per handle rather than once per frame: a missing type is a permanent state of the
        // scene, and a message repeated sixty times a second is a log nobody reads.
        if ( m_Reported.contains( handle ) || m_Requests.contains( handle ) )
            return nullptr;

        // AL1-7: no boot stage reads every `.decloudtype` any more. The first layer that names one creates
        // it from its registry row and requests the read; the host's ContentGate holds the loading screen
        // until it lands (a type is a link in the material -> type -> noise volume chain the gate waits on).
        auto created = Assets::CreateFromRegistryRow<Assets::CloudTypeAsset>(
             m_Assets, handle, Common::Content::ContentKind::CloudType );
        if ( !created )
        {
            m_Reported.insert( handle );
            LOG_ERROR( "[Clouds] Cloud type {} is referenced but cannot be used; the layer falls back to the "
                       "built-in default: {}",
                       static_cast<uint64_t>( handle ), created.GetError() );
            return nullptr;
        }
        if ( created.GetValue()->IsReadyForUse() )
        {
            if ( const auto registered = Register( created.GetValue() ); !registered )
            {
                m_Reported.insert( handle );
                LOG_ERROR( "[Clouds] Cloud type {} cannot be used: {}", static_cast<uint64_t>( handle ),
                           registered.GetError() );
                return nullptr;
            }
            return &m_Types.at( handle );
        }

        m_Requests[handle] = Assets::AsyncAssetLoader::Get().Request(
             created.GetValue(),
             [this, handle]( const Assets::Asset<Assets::AssetBase>& loaded, const Assets::LoadOutcome outcome,
                             const std::string& error )
             {
                 m_Requests.erase( handle );
                 if ( outcome != Assets::LoadOutcome::Loaded )
                 {
                     m_Reported.insert( handle );
                     LOG_ERROR( "[Clouds] Cloud type '{}' could not be read; its layers fall back to the "
                                "built-in default: {}",
                                loaded->GetMetadata().Filepath.string(), error );
                     return;
                 }
                 // The shell was created unread, so CreateAsset bound nothing; the noise volume binds here.
                 if ( const auto manager = m_Assets.lock() )
                     loaded->ResolveDependencies( *manager );
                 if ( const auto registered =
                           Register( std::static_pointer_cast<Assets::CloudTypeAsset>( loaded ) );
                      !registered )
                 {
                     m_Reported.insert( handle );
                     LOG_ERROR( "[Clouds] Cloud type {} cannot be used: {}", static_cast<uint64_t>( handle ),
                                registered.GetError() );
                 }
             },
             [this, handle] { m_Requests.erase( handle ); } );
        return nullptr;
    }

    const Graphic::CloudTypeShape& CloudTypeService::GetShape( const Assets::AssetHandle& handle )
    {
        if ( const Entry* entry = FindOrRequest( handle ) )
            return entry->Shape;
        return Assets::CloudTypeDefaultShape();
    }

    Assets::AssetHandle CloudTypeService::GetNoiseVolume( const Assets::AssetHandle& handle )
    {
        if ( const Entry* entry = FindOrRequest( handle ) )
            return entry->NoiseVolume;

        // The built-in type has no volume of its own, and a null handle is exactly what
        // CloudNoiseService reads as "the default volume". The two empty slots therefore mean the same
        // thing, which is the property that keeps a scene with nothing authored in it renderable.
        return Assets::AssetHandle::Null();
    }

    void CloudTypeService::Clear()
    {
        m_Requests.clear(); // released: no delegate fires into a cleared service
        m_Types.clear();
        m_Reported.clear();
        ++m_Generation;
    }
} // namespace Desert::Runtime
