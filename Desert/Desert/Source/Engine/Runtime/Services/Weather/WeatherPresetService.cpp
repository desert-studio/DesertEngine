#include "WeatherPresetService.hpp"

#include <Engine/Assets/RegistryDiscovery.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <filesystem>
#include <system_error>

namespace Desert::Runtime
{
    void WeatherPresetService::BindAssetManager( const std::weak_ptr<Assets::AssetManager>& assets )
    {
        m_Assets = assets;
    }

    Common::ResultStr<const Assets::Serialization::WeatherPresetData*>
    WeatherPresetService::Get( const Assets::AssetHandle& handle )
    {
        using Result = const Assets::Serialization::WeatherPresetData*;
        if ( const auto failed = m_Failed.find( handle ); failed != m_Failed.end() )
            return Common::MakeError<Result>( failed->second );
        if ( const auto held = m_Held.find( handle ); held != m_Held.end() && held->second->IsReadyForUse() )
            return Common::MakeSuccess<Result>( &held->second->GetData() );

        // Failed once, refused from then on with the same reason (logged once): a scene naming a broken
        // preset must not re-read the file every frame.
        const auto fail = [this, handle]( const std::string& why ) -> Common::ResultStr<Result>
        {
            LOG_ERROR( "[Weather] Preset {} cannot be read: {}", static_cast<uint64_t>( handle ), why );
            m_Failed[handle] = why;
            return Common::MakeError<Result>( why );
        };
        auto created = Assets::CreateFromRegistryRow<Assets::WeatherPresetAsset>(
             m_Assets, handle, Common::Content::ContentKind::WeatherPreset );
        if ( !created )
            return fail( created.GetError() );
        const auto asset = created.GetValue();
        if ( !asset->IsReadyForUse() )
            if ( const auto loaded = asset->LoadFromFile(); !loaded )
                return fail( loaded.GetError() );
        m_Held[handle] = asset;
        return Common::MakeSuccess<Result>( &asset->GetData() );
    }

    Common::ResultStr<Assets::AssetHandle> WeatherPresetService::HandleForPath( const std::filesystem::path& path )
    {
        const auto assets = m_Assets.lock();
        if ( !assets )
            return Common::MakeError<Assets::AssetHandle>( std::string( "no asset manager is bound" ) );
        const std::filesystem::path full =
             path.is_absolute() ? path : ( Common::Constants::Path::ASSETS_PATH / path ).lexically_normal();
        std::error_code ec;
        if ( !std::filesystem::exists( full, ec ) )
            return Common::MakeFormattedError<Assets::AssetHandle>( "no weather preset at '{}'", full.string() );
        auto asset = assets->FindByPath<Assets::WeatherPresetAsset>( full );
        if ( !asset )
            asset = assets->CreateAsset<Assets::WeatherPresetAsset>( full, /*loadAfterCreate=*/false );
        if ( !asset )
            return Common::MakeFormattedError<Assets::AssetHandle>(
                 "'{}' cannot be registered as a weather preset", full.string() );
        return Common::MakeSuccess<Assets::AssetHandle>( Assets::AssetHandle( asset->GetMetadata().Handle ) );
    }

    void WeatherPresetService::Clear()
    {
        m_Held.clear();
        m_Failed.clear();
    }
} // namespace Desert::Runtime
