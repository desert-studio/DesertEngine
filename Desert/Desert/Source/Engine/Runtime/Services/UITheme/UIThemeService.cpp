#include "UIThemeService.hpp"

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>

#include <filesystem>

namespace Desert::Runtime
{
    Common::BoolResultStr UIThemeService::Register( const std::shared_ptr<Assets::UIThemeAsset>& asset )
    {
        if ( !asset )
            return Common::MakeError( "UIThemeService::Register was given a null asset" );

        if ( !asset->IsReadyForUse() )
            return Common::MakeFormattedError<bool>( "UI theme '{}' is not loaded",
                                                     asset->GetMetadata().Filepath.string() );

        const Assets::AssetHandle& handle = asset->GetMetadata().Handle;

        if ( auto it = m_Themes.find( handle );
             it != m_Themes.end() && it->second.Revision == asset->GetRevision() )
            return BOOLSUCCESS;

        const Assets::UIThemeData& data = asset->GetData();

        // The font tokens' paths become handles here, and here only. A path is stated in the file
        // RELATIVE to the assets root — that is what makes a theme portable off the machine it was
        // authored on — and is joined to that root exactly once, at this line.
        std::unordered_map<std::string, Assets::AssetHandle> fontHandles;
        FontService*                                         fonts = ResourceRegistry::GetFontService();
        for ( const auto& font : data.Fonts )
        {
            if ( font.Asset.empty() )
                continue; // "" means the built-in face, by choice — not a failure to resolve

            // TWO ROOTS, IN A FIXED ORDER, AND THAT IS NOT AMBIGUITY. FontService::AvailableFonts already
            // scans both — the project's assets tree AND the engine's shared Resources/Fonts — so a theme
            // that could only name one of them would be unable to name half the faces the font picker
            // offers. Project content wins, so a project may shadow a shipped face by shipping its own.
            const std::filesystem::path named( font.Asset );
            std::filesystem::path       full = Common::Constants::Path::ASSETS_PATH / named;

            std::error_code ec;
            if ( !std::filesystem::exists( full, ec ) )
            {
                const std::filesystem::path shared = Common::Constants::Path::FONTS_PATH / named;
                if ( std::filesystem::exists( shared, ec ) )
                    full = shared;
            }

            // RegisterFont is idempotent and path-derived, so a theme naming a face the picker already
            // knows gets the same handle the picker hands out — the two cannot disagree about identity.
            const uint64_t registered = fonts != nullptr ? fonts->RegisterFont( full.string() ) : 0;
            if ( registered == 0 )
            {
                // Named here rather than swallowed: a themed font that quietly falls back to the built-in
                // face looks exactly like a theme that never bound a font at all.
                LOG_ERROR( "[UI] Theme '{}' font token '{}' names '{}', which is under neither the assets "
                           "root nor the shared font root — that token falls back to the built-in face.",
                           asset->GetDisplayName(), font.Name, font.Asset );
                continue;
            }
            fontHandles.emplace( font.Name, Assets::AssetHandle( registered ) );
        }

        auto built = Assets::BuildUIThemeRuntime( data, asset->GetDisplayName(), fontHandles );
        if ( !built )
            return Common::MakeFormattedError<bool>( "UI theme '{}' could not be flattened: {}",
                                                     asset->GetMetadata().Filepath.string(), built.GetError() );

        Entry entry{ built.ExtractValue(), asset->GetRevision() };
        entry.Runtime.Revision = asset->GetRevision();
        m_Themes[handle]       = std::move( entry );
        ++m_Generation;

        // A theme the scene has already complained about is now present — a hot reload of a file that was
        // broken when the scene loaded is exactly that case, and leaving the handle in the reported set
        // would silence the message if it broke again.
        m_Reported.erase( handle );

        LOG_INFO( "[UI] Theme '{}' registered as {} ({}): {} styles.", asset->GetDisplayName(),
                  static_cast<uint64_t>( handle ), asset->GetMetadata().Filepath.filename().string(),
                  m_Themes[handle].Runtime.Styles.size() );
        return BOOLSUCCESS;
    }

    const Assets::UIThemeRuntime* UIThemeService::Get( const Assets::AssetHandle& handle )
    {
        // An empty slot is silent: it is the state of every canvas authored before themes existed, and a
        // message about it would fire for every such canvas on every frame.
        if ( handle == 0 )
            return nullptr;

        if ( auto it = m_Themes.find( handle ); it != m_Themes.end() )
            return &it->second.Runtime;

        // Said once per handle rather than once per frame: a missing theme is a permanent state of the
        // scene, and a message repeated sixty times a second is a log nobody reads. The set is what makes
        // the FIRST occurrence findable.
        if ( m_Reported.insert( handle ).second )
            LOG_ERROR( "[UI] Theme {} is referenced by a canvas but not registered — every element of that "
                       "canvas draws its own authored colours. The scene names a .detheme the asset scan "
                       "did not find.",
                       static_cast<uint64_t>( handle ) );
        return nullptr;
    }

    void UIThemeService::Clear()
    {
        m_Themes.clear();
        m_Reported.clear();
        ++m_Generation;
    }
} // namespace Desert::Runtime
