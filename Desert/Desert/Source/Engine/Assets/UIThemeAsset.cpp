#include <Engine/Assets/UIThemeAsset.hpp>
#include <Common/Content/CanonicalText.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>

namespace Desert::Assets
{
    UIThemeAsset::UIThemeAsset( AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::UITheme )
    {
        m_DisplayName = m_Metadata.Filepath.stem().string();
    }

    Common::BoolResultStr UIThemeAsset::LoadFromFile()
    {
        const std::string path = m_Metadata.Filepath.string();

        // Through the VFS first, so a packaged build reads the theme out of its .dpak exactly like every
        // other asset, then off the disk for a loose file the pak does not carry.
        std::string text;
        if ( const auto packed = Common::Utils::VFS::Exists( m_Metadata.Filepath )
                                      ? Common::Utils::VFS::ReadFile( m_Metadata.Filepath )
                                      : std::nullopt;
             packed.has_value() )
        {
            text = packed.value();
        }
        else
        {
            if ( auto read = Common::Utils::FileSystem::ReadFileContent( m_Metadata.Filepath ); read )
                text = read.ExtractValue();
            // A failed read leaves `text` empty on purpose: the branch below is the one refusal that names
            // both shapes ("empty or could not be opened").
        }

        if ( text.empty() )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "UI theme '{}' is empty or could not be opened", path );
        }

        auto parsed = ParseUITheme( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "UI theme '{}' is not usable: {}", path, parsed.GetError() );
        }

        m_Data        = parsed.ExtractValue();
        m_DisplayName = m_Data.DisplayName.value_or( m_Metadata.Filepath.stem().string() );
        ++m_Revision;
        m_Ready = true;

        LOG_INFO( "[UI] Theme '{}' loaded: {} colours ({} high-contrast), {} metrics, {} fonts, {} styles.",
                  m_DisplayName, m_Data.Colors.size(), m_Data.HighContrastColors.size(), m_Data.Metrics.size(),
                  m_Data.Fonts.size(), m_Data.Styles.size() );
        return BOOLSUCCESS;
    }

    Common::BoolResultStr UIThemeAsset::Unload()
    {
        m_Data        = UIThemeData{};
        m_DisplayName = m_Metadata.Filepath.stem().string();
        m_Ready       = false;
        // m_Revision is NOT reset. It is monotonic per asset instance: rewinding it would make the next
        // load look like no change at all to a service that compares revisions, which is the whole point
        // of it existing.
        return BOOLSUCCESS;
    }

    Common::BoolResultStr UIThemeAsset::Save( const Common::Filepath& filepath, const UIThemeData& data )
    {
        if ( auto valid = ValidateUIThemeData( data ); !valid )
            return Common::MakeFormattedError<bool>( "refusing to write '{}': {}", filepath.string(),
                                                     valid.GetError() );

        std::error_code ec;
        if ( filepath.has_parent_path() )
            std::filesystem::create_directories( filepath.parent_path(), ec );

        const auto canonicalText = Common::Content::CanonicalJsonTextOfWriterOutput( WriteUITheme( data ) );
        if ( !canonicalText )
            return Common::MakeError<bool>( canonicalText.GetError() );
        const std::string& text = canonicalText.GetValue();

        // Through the write primitive, not a local std::ofstream (Д35): a theme is a few kilobytes —
        // smaller than one filebuf — so it is precisely the payload that never reaches the OS until the
        // flush, and the flush of a local stream is its destructor, running after this function has
        // already returned success. The primitive closes before it decides, and writes through a
        // temporary so a failed save cannot cost the author the theme they already had.
        if ( const auto ok = Common::Utils::FileSystem::WriteContentToFileAtomic( filepath, text ); !ok )
            return Common::MakeFormattedError<bool>( "'{}' ({} bytes) could not be written: {}", filepath.string(),
                                                     text.size(), ok.GetError() );

        LOG_INFO( "[UI] Theme written: '{}', {} colours, {} metrics, {} fonts, {} styles, {} bytes.",
                  filepath.string(), data.Colors.size(), data.Metrics.size(), data.Fonts.size(),
                  data.Styles.size(), text.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
