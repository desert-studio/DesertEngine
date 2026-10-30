#include "SubsurfaceProfileService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Runtime
{
    void SubsurfaceProfileService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    Common::ResultStr<uint32_t> SubsurfaceProfileService::IndexFor( const Assets::AssetGuidRef& reference )
    {
        const auto guid = Common::Content::AssetGuidFromText( reference.Guid );
        if ( !guid || guid.GetValue().IsNull() )
            return Common::MakeFormattedError<uint32_t>( "subsurface profile reference '{}' ('{}') is not a GUID",
                                                         reference.Guid, reference.Path );
        if ( const uint32_t known = m_Registry.IndexOf( guid.GetValue() );
             known != Graphic::SubsurfaceProfileRegistry::kInvalidIndex )
            return Common::MakeSuccess( known );

        const Assets::AssetHandle handle( Common::Content::HandleForGuid( guid.GetValue() ) );
        auto created = Assets::CreateFromRegistryRow<Assets::SubsurfaceProfileAsset>(
             m_Assets, handle, Common::Content::ContentKind::SubsurfaceProfile );
        if ( !created )
            return Common::MakeFormattedError<uint32_t>( "subsurface profile '{}' ('{}'): {}", reference.Guid,
                                                         reference.Path, created.GetError() );
        const auto asset = created.GetValue();
        if ( !asset->IsReadyForUse() )
            if ( const auto loaded = asset->LoadFromFile(); !loaded )
                return Common::MakeError<uint32_t>( loaded.GetError() );
        m_Held[handle] = asset;
        return m_Registry.Acquire( guid.GetValue(), asset->GetData() );
    }

    void SubsurfaceProfileService::Clear()
    {
        m_Held.clear();
        m_Registry.Clear();
    }
} // namespace Desert::Runtime
