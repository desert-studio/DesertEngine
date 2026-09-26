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

    const CloudTypeService::Entry* CloudTypeService::FindOrDiscover( const Assets::AssetHandle& handle )
    {
        if ( handle == 0 )
            return nullptr;
        if ( auto it = m_Types.find( handle ); it != m_Types.end() )
            return &it->second;
        // Said once per handle rather than once per frame: a missing type is a permanent state of the
        // scene, and a message repeated sixty times a second is a log nobody reads.
        if ( m_Reported.contains( handle ) )
            return nullptr;

        // AL1-7: no boot stage reads every `.decloudtype` any more; the first layer that names one reads it
        // from its registry row, through the loader, in the call that needs its numbers.
        auto read = Assets::DiscoverAndReadNow<Assets::CloudTypeAsset>( m_Assets, handle,
                                                                        Common::Content::ContentKind::CloudType );
        if ( read )
        {
            if ( const auto registered = Register( read.GetValue() ); !registered )
                read = Common::MakeError<Assets::Asset<Assets::CloudTypeAsset>>( registered.GetError() );
        }
        if ( !read )
        {
            m_Reported.insert( handle );
            LOG_ERROR( "[Clouds] Cloud type {} is referenced but cannot be used; the layer falls back to the "
                       "built-in default: {}",
                       static_cast<uint64_t>( handle ), read.GetError() );
            return nullptr;
        }
        return &m_Types.at( handle );
    }

    const Graphic::CloudTypeShape& CloudTypeService::GetShape( const Assets::AssetHandle& handle )
    {
        if ( const Entry* entry = FindOrDiscover( handle ) )
            return entry->Shape;
        return Assets::CloudTypeDefaultShape();
    }

    Assets::AssetHandle CloudTypeService::GetNoiseVolume( const Assets::AssetHandle& handle )
    {
        if ( const Entry* entry = FindOrDiscover( handle ) )
            return entry->NoiseVolume;

        // The built-in type has no volume of its own, and a null handle is exactly what
        // CloudNoiseService reads as "the default volume". The two empty slots therefore mean the same
        // thing, which is the property that keeps a scene with nothing authored in it renderable.
        return Assets::AssetHandle::Null();
    }

    void CloudTypeService::Clear()
    {
        m_Types.clear();
        m_Reported.clear();
        ++m_Generation;
    }
} // namespace Desert::Runtime
