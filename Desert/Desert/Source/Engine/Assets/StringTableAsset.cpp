#include "StringTableAsset.hpp"
#include <Common/Content/AssetRedirector.hpp>
#include <Common/Content/CanonicalText.hpp>

#include <Engine/Assets/ContentRegistry.hpp>

#include <Engine/Localization/LocalizationService.hpp>
#include <Engine/Assets/TextAssetHeaderIdentity.hpp>

#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/VFS.hpp>

#include <filesystem>
#include <set>

namespace Desert::Assets
{
    StringTableAsset::StringTableAsset( const AssetPriority priority, const Common::Filepath& filepath )
         : AssetBase( priority, filepath, AssetTypeID::StringTable )
    {
        m_DisplayName = m_Metadata.Filepath.stem().string();

        // THE TABLE'S IDENTITY IS ITS HEADER GUID (format 2), adopted HERE rather than in the load because the
        // asset manager keys its handle lookup at creation. A file with no readable header keeps the
        // path-derived handle - the load refuses a version-1 file by name, so none is ever READY under it.
        const Common::Content::AssetGuid guid = ReadTextHeaderGuid( m_Metadata.Filepath );
        if ( !guid.IsNull() )
            AdoptHandleFromFile( Common::UUID( static_cast<uint64_t>( Common::Content::HandleForGuid( guid ) ) ),
                                 Common::AssetHandle::StableKeyForPath( m_Metadata.Filepath ) );
    }

    std::string StringTableAsset::PublishId() const
    {
        return m_Metadata.Filepath.generic_string();
    }

    Common::BoolResultStr StringTableAsset::LoadFromFile()
    {
        // The old path of a moved table reads the table where it now lives, through the registry.
        const std::filesystem::path file = ContentRegistry::FileToOpen( m_Metadata.Filepath );
        const std::string           path = file.string();

        // Through the VFS first, so a packaged build reads its translations out of the `.dpak` exactly
        // like every other asset, then off the disk for a loose file the pak does not carry.
        std::string text;
        if ( const auto packed =
                  Common::Utils::VFS::Exists( file ) ? Common::Utils::VFS::ReadFile( file ) : std::nullopt;
             packed.has_value() )
        {
            text = packed.value();
        }
        else
        {
            if ( auto read = Common::Utils::FileSystem::ReadFileContent( file ); read )
            {
                text = read.ExtractValue();
            }
        }

        if ( text.empty() )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "String table '{}' is empty or could not be opened", path );
        }

        if ( auto moved =
                  Common::Content::RefuseRedirectorBytes( path, text, ContentRegistry::KeyOfRedirectorTarget );
             !moved )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "String table not loaded: {}", moved.GetError() );
        }

        auto parsed = Localization::ParseStringTable( text );
        if ( !parsed )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "String table '{}' is not usable: {}", path,
                                                     parsed.GetError() );
        }

        Localization::StringTableData data = parsed.ExtractValue();

        // Published BEFORE the members are updated, because a refusal must leave this asset exactly as it
        // was: a hot reload that fails has to keep showing the strings that were working, not swap in a
        // table the lookup never accepted.
        if ( const auto published = Localization::Localization::Get().RegisterTable( PublishId(), data );
             !published )
        {
            m_Ready = false;
            return Common::MakeFormattedError<bool>( "String table '{}' was not published: {}", path,
                                                     published.GetError() );
        }

        m_Data        = std::move( data );
        m_DisplayName = m_Data.DisplayName.value_or( m_Metadata.Filepath.stem().string() );
        m_Ready       = true;

        // The count of LANGUAGES matters as much as the count of keys: a table with one language is a
        // table nobody has translated yet, and that is the state worth seeing in a log at a glance.
        std::set<std::string> languages;
        for ( const Localization::LocalizedEntry& entry : m_Data.Entries )
        {
            for ( const auto& [language, forms] : entry.Forms )
                languages.insert( language );
        }
        std::string list;
        for ( const std::string& language : languages )
        {
            if ( !list.empty() )
                list += ", ";
            list += language;
        }
        LOG_INFO( "[Localization] String table '{}' loaded: {}, {} keys in {} languages ({})", path, m_DisplayName,
                  m_Data.Entries.size(), languages.size(), list );

        return BOOLSUCCESS;
    }

    Common::BoolResultStr StringTableAsset::Unload()
    {
        Localization::Localization::Get().UnregisterTable( PublishId() );
        m_Data  = {};
        m_Ready = false;
        return BOOLSUCCESS;
    }

    Common::BoolResultStr StringTableAsset::Save( const Common::Filepath&              filepath,
                                                  const Localization::StringTableData& data )
    {
        // The round trip is the invariant this format lives by, so it is CHECKED rather than assumed: a
        // table that would not read back is refused here, where the author can still see what they typed,
        // instead of on somebody else's machine at load time.
        const auto canonicalText =
             Common::Content::CanonicalJsonTextOfWriterOutput( Localization::WriteStringTable( data ) );
        if ( !canonicalText )
            return Common::MakeError<bool>( canonicalText.GetError() );
        const std::string& text = canonicalText.GetValue();
        if ( const auto reread = Localization::ParseStringTable( text ); !reread )
        {
            return Common::MakeFormattedError<bool>(
                 "refusing to write string table '{}': what it would write does not read back — {}",
                 filepath.string(), reread.GetError() );
        }

        std::error_code ec;
        if ( filepath.has_parent_path() )
            std::filesystem::create_directories( filepath.parent_path(), ec );

        // Through the atomic write primitive, not a local std::ofstream (Д35). A `.destrings` is a few
        // kilobytes — smaller than one filebuf — so it is precisely the payload that never reaches the OS
        // until the flush, and the flush of a local stream is its destructor, running after this function
        // has already returned success. The primitive closes before it decides, and writes through a
        // temporary so a failed save cannot cost a translator the file they already had.
        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( filepath, text ); !written )
        {
            return Common::MakeFormattedError<bool>( "'{}' ({} bytes) could not be written: {}", filepath.string(),
                                                     text.size(), written.GetError() );
        }

        LOG_INFO( "[Localization] String table written: '{}', {} keys, {} bytes.", filepath.string(),
                  data.Entries.size(), text.size() );
        return BOOLSUCCESS;
    }
} // namespace Desert::Assets
