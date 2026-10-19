#include "WorldParametersService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    void WorldParametersService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    Common::ResultStr<const Assets::Serialization::WorldParametersData*>
    WorldParametersService::Get( const Assets::AssetHandle& handle )
    {
        using Result = const Assets::Serialization::WorldParametersData*;
        if ( const auto failed = m_Failed.find( handle ); failed != m_Failed.end() )
            return Common::MakeError<Result>( failed->second );
        if ( const auto held = m_Held.find( handle ); held != m_Held.end() && held->second->IsReadyForUse() )
            return Common::MakeSuccess<Result>( &held->second->GetData() );

        // Failed once, refused from then on with the same reason (logged once): a scene naming a broken
        // collection must not re-read the file every frame.
        const auto fail = [this, handle]( const std::string& why ) -> Common::ResultStr<Result>
        {
            LOG_ERROR( "[WorldParameters] Collection {} cannot be read: {}", static_cast<uint64_t>( handle ), why );
            m_Failed[handle] = why;
            return Common::MakeError<Result>( why );
        };
        auto created = Assets::CreateFromRegistryRow<Assets::WorldParametersAsset>(
             m_Assets, handle, Common::Content::ContentKind::WorldParameters );
        if ( !created )
            return fail( created.GetError() );
        const auto asset = created.GetValue();
        if ( !asset->IsReadyForUse() )
            if ( const auto loaded = asset->LoadFromFile(); !loaded )
                return fail( loaded.GetError() );
        m_Held[handle] = asset;
        return Common::MakeSuccess<Result>( &asset->GetData() );
    }

    void WorldParametersService::Clear()
    {
        m_Held.clear();
        m_Failed.clear();
    }
} // namespace Desert::Runtime
