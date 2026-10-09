#include <Common/Content/AssetTrash.hpp>

#include <Common/Content/ExternalEntitiesFolder.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Json/Json.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <ranges>
#include <set>

namespace Common::Content
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr std::string_view kManifestName = "Trash.json";
        constexpr std::string_view kRowsName     = "Rows.json";
        constexpr std::string_view kFilesFolder  = "Files";

        struct TrashedFileJson
        {
            std::string Original; // absolute, generic separators
            std::string Stored;   // relative to the slot
        };

        struct TrashManifestJson
        {
            std::string                  From;
            std::string                  Guid; // AssetGuidToText, "" for none
            std::vector<TrashedFileJson> Files;
        };

        // <root>/<UTC time>-<n>: names sort by the time of the delete, so newest-first is a name order.
        ResultStr<fs::path> NewSlot( const fs::path& trashRoot )
        {
            const auto now = std::chrono::floor<std::chrono::milliseconds>( std::chrono::system_clock::now() );
            const std::string stamp = std::format( "{:%Y%m%d-%H%M%S}", now );
            std::error_code   ec;
            fs::create_directories( trashRoot, ec );
            if ( ec )
                return MakeError<fs::path>( "trash: could not create '" + trashRoot.string() +
                                            "': " + ec.message() );
            for ( int n = 0; n < 10000; ++n )
            {
                const fs::path slot = trashRoot / std::format( "{}-{:04}", stamp, n );
                if ( fs::create_directory( slot, ec ) )
                    return MakeSuccess( slot );
                if ( ec )
                    return MakeError<fs::path>( "trash: could not create '" + slot.string() +
                                                "': " + ec.message() );
            }
            return MakeError<fs::path>( "trash: no free slot name under '" + trashRoot.string() + "' for " +
                                        stamp );
        }

        // Every file a path is on disk: itself, or everything under it.
        std::vector<fs::path> FilesOf( const fs::path& path )
        {
            std::error_code ec;
            if ( fs::is_directory( path, ec ) )
                return Utils::FileSystem::ListFilesRecursive( path );
            return { path };
        }

        BoolResultStr MoveBack( const std::vector<std::pair<fs::path, fs::path>>& moved )
        {
            std::string failures;
            for ( const auto& [from, to] : std::views::reverse( moved ) )
            {
                std::error_code ec;
                fs::create_directories( from.parent_path(), ec );
                fs::rename( to, from, ec );
                if ( ec )
                    failures += " '" + to.string() + "' -> '" + from.string() + "' (" + ec.message() + ")";
            }
            if ( !failures.empty() )
                return MakeError( "could not move back:" + failures );
            return MakeSuccess( true );
        }
    } // namespace

    fs::path ProjectTrashRoot()
    {
        return ( Constants::Path::CurrentProjectRoot().ProjectDir / "Saved" / "Trash" ).lexically_normal();
    }

    ResultStr<AssetTrashRecord> MoveToTrash( Utils::AssetRegistry& registry, const fs::path& path,
                                             const fs::path& trashRoot )
    {
        std::error_code ec;
        if ( !fs::exists( path, ec ) )
            return MakeError<AssetTrashRecord>( "delete '" + path.string() +
                                                "': it is not on disk (a file that lives only in a mounted pak "
                                                "cannot be deleted)" );

        AssetTrashRecord record;
        record.From = fs::absolute( path ).lexically_normal();

        // What goes: the path, its import record, a scene's entity folder.
        std::vector<fs::path> originals = { record.From };
        if ( const fs::path importRecord = ImportRecordPathFor( record.From ); fs::exists( importRecord, ec ) )
            originals.push_back( importRecord );
        if ( record.From.extension() == Constants::Extensions::SCENE_EXTENSION )
            if ( const fs::path entities = ExternalEntitiesDirectoryOf( record.From );
                 fs::is_directory( entities, ec ) )
                originals.push_back( entities );

        // The rows those files hold, found BEFORE anything moves (a key is a function of the path).
        std::set<std::string> keys;
        for ( const fs::path& original : originals )
            for ( const fs::path& file : FilesOf( original ) )
                if ( const std::string key = AssetHandle::StableKeyForPath( file ); !key.empty() )
                    keys.insert( key );
        for ( const std::string& key : keys )
            if ( const Utils::AssetRegistryEntry* row = registry.FindByKey( key ) )
                record.Rows.push_back( *row );
        if ( const Utils::AssetRegistryEntry* own =
                  registry.FindByKey( AssetHandle::StableKeyForPath( record.From ) ) )
            record.Guid = own->Guid;

        auto slot = NewSlot( trashRoot );
        if ( !slot )
            return MakeError<AssetTrashRecord>( "delete '" + path.string() + "': " + slot.GetError() );
        record.Slot = slot.GetValue();

        std::vector<std::pair<fs::path, fs::path>> moved;
        for ( std::size_t i = 0; i < originals.size(); ++i )
        {
            const fs::path stored = fs::path( kFilesFolder ) / std::to_string( i ) / originals[i].filename();
            const fs::path to     = record.Slot / stored;
            fs::create_directories( to.parent_path(), ec );
            if ( !ec )
                fs::rename( originals[i], to, ec );
            if ( ec )
            {
                const std::string why = ec.message();
                static_cast<void>( MoveBack( moved ) );
                fs::remove_all( record.Slot, ec );
                return MakeError<AssetTrashRecord>( "delete '" + path.string() + "': could not move '" +
                                                    originals[i].string() + "' into the trash '" + to.string() +
                                                    "': " + why + "; nothing was deleted" );
            }
            moved.emplace_back( originals[i], to );
            record.Files.push_back( { originals[i], stored } );
        }

        TrashManifestJson manifest{
             record.From.generic_string(), record.Guid ? AssetGuidToText( *record.Guid ) : std::string(), {} };
        for ( const TrashedFile& file : record.Files )
            manifest.Files.push_back( { file.Original.generic_string(), file.Stored.generic_string() } );
        Utils::AssetRegistry rows;
        for ( const Utils::AssetRegistryEntry& row : record.Rows )
            static_cast<void>( rows.Insert( row ) );
        const std::string rowsText        = rows.Serialize();
        const auto        writtenManifest = Json::WriteFileAtomic( record.Slot / kManifestName, manifest );
        const auto        writtenRows =
             writtenManifest ? Utils::FileSystem::WriteBytesToFileAtomic(
                                    record.Slot / kRowsName, std::as_bytes( std::span<const char>( rowsText ) ) )
                                    : writtenManifest;
        if ( !writtenRows )
        {
            static_cast<void>( MoveBack( moved ) );
            fs::remove_all( record.Slot, ec );
            return MakeError<AssetTrashRecord>( "delete '" + path.string() + "': " + writtenRows.GetError() +
                                                "; nothing was deleted" );
        }

        // A scene's `__ExternalEntities__` folder its entity folder left empty goes too (only when empty).
        for ( const TrashedFile& file : record.Files )
            if ( file.Original.parent_path().filename() == kExternalEntitiesFolder )
                fs::remove( file.Original.parent_path(), ec );
        for ( const Utils::AssetRegistryEntry& row : record.Rows )
            registry.Remove( row.Key );
        return MakeSuccess( std::move( record ) );
    }

    BoolResultStr RestoreFromTrash( Utils::AssetRegistry& registry, const AssetTrashRecord& record )
    {
        std::error_code ec;
        for ( const TrashedFile& file : record.Files )
        {
            if ( fs::exists( file.Original, ec ) )
                return MakeError( "restore '" + record.From.string() + "': '" + file.Original.string() +
                                  "' exists again; refusing to overwrite it, nothing was restored" );
            if ( !fs::exists( record.Slot / file.Stored, ec ) )
                return MakeError( "restore '" + record.From.string() + "': the trash no longer holds '" +
                                  ( record.Slot / file.Stored ).string() + "'; nothing was restored" );
        }

        std::vector<std::pair<fs::path, fs::path>> moved; // original <- stored, for the take-back
        for ( const TrashedFile& file : record.Files )
        {
            const fs::path stored = record.Slot / file.Stored;
            fs::create_directories( file.Original.parent_path(), ec );
            if ( !ec )
                fs::rename( stored, file.Original, ec );
            if ( ec )
            {
                const std::string why = ec.message();
                for ( const auto& [original, back] : std::views::reverse( moved ) )
                    fs::rename( original, back, ec );
                return MakeError( "restore '" + record.From.string() + "': could not move '" + stored.string() +
                                  "' back to '" + file.Original.string() + "': " + why +
                                  "; nothing was restored" );
            }
            moved.emplace_back( file.Original, stored );
        }

        std::string failures;
        for ( const Utils::AssetRegistryEntry& row : record.Rows )
        {
            registry.Remove( row.Key ); // a stale row a rescan left for the missing file
            if ( const auto inserted = registry.Insert( row ); !inserted )
                failures += " " + row.Key + " (" + inserted.GetError() + ")";
        }
        fs::remove_all( record.Slot, ec );
        if ( !failures.empty() )
            return MakeError( "restore '" + record.From.string() +
                              "': the files are back but these rows could not be restored:" + failures );
        return MakeSuccess( true );
    }

    ResultStr<AssetTrashRecord> ReadTrashSlot( const fs::path& slot )
    {
        const auto manifest = Json::ReadFile<TrashManifestJson>( slot / kManifestName );
        if ( !manifest )
            return MakeError<AssetTrashRecord>( "trash slot '" + slot.string() + "': " + manifest.GetError() );
        const auto rowsText = Utils::FileSystem::ReadFileContent( slot / kRowsName );
        if ( !rowsText )
            return MakeError<AssetTrashRecord>( "trash slot '" + slot.string() + "': " + rowsText.GetError() );
        auto rows = Utils::AssetRegistry::Parse( rowsText.GetValue() );
        if ( !rows )
            return MakeError<AssetTrashRecord>( "trash slot '" + slot.string() + "': " + rows.GetError() );

        AssetTrashRecord record;
        record.From = fs::path( manifest.GetValue().From );
        record.Slot = slot;
        if ( !manifest.GetValue().Guid.empty() )
        {
            const auto guid = AssetGuidFromText( manifest.GetValue().Guid );
            if ( !guid )
                return MakeError<AssetTrashRecord>( "trash slot '" + slot.string() + "': " + guid.GetError() );
            record.Guid = guid.GetValue();
        }
        for ( const TrashedFileJson& file : manifest.GetValue().Files )
            record.Files.push_back( { fs::path( file.Original ), fs::path( file.Stored ) } );
        record.Rows = rows.GetValue().Entries();
        return MakeSuccess( std::move( record ) );
    }

    std::vector<AssetTrashRecord> ListTrash( const fs::path& trashRoot )
    {
        std::vector<fs::path> slots;
        std::error_code       ec;
        for ( const auto& entry : fs::directory_iterator( trashRoot, ec ) )
            if ( entry.is_directory( ec ) )
                slots.push_back( entry.path() );
        std::ranges::sort( slots, std::greater<>() );

        std::vector<AssetTrashRecord> records;
        for ( const fs::path& slot : slots )
        {
            auto record = ReadTrashSlot( slot );
            if ( !record )
            {
                LOG_WARN( "[Trash] skipped: {}", record.GetError() );
                continue;
            }
            records.push_back( record.ExtractValue() );
        }
        return records;
    }
} // namespace Common::Content
