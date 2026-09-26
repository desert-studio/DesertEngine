#include <Common/Content/AssetMove.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <ranges>
#include <span>

namespace Common::Content
{
    namespace fs = std::filesystem;

    namespace
    {
        std::optional<ContentKind> KindNamed( std::string_view name )
        {
            for ( std::size_t i = 0; i < CONTENT_KIND_COUNT; ++i )
            {
                const auto kind = static_cast<ContentKind>( i );
                if ( KindSpec( kind ).Name == name )
                    return kind;
            }
            return std::nullopt;
        }

        // Rename when both ends share a volume (atomic); otherwise the bytes go through the atomic writer
        // (temp + verify + rename) and only then is the source removed, so no crash leaves zero copies.
        BoolResultStr MoveFileAtomic( const fs::path& from, const fs::path& to )
        {
            std::error_code ec;
            fs::rename( from, to, ec );
            if ( !ec )
                return MakeSuccess( true );
            const auto bytes = Utils::FileSystem::ReadFileContent( from );
            if ( !bytes )
                return MakeError( "move '" + from.string() + "' -> '" + to.string() + "': rename failed (" +
                                  ec.message() + ") and the source could not be read: " + bytes.GetError() );
            const std::string& text = bytes.GetValue();
            if ( const auto written = Utils::FileSystem::WriteBytesToFileAtomic(
                      to, std::as_bytes( std::span<const char>( text.data(), text.size() ) ) );
                 !written )
                return MakeError( "move '" + from.string() + "' -> '" + to.string() + "': " + written.GetError() );
            std::error_code removeEc;
            fs::remove( from, removeEc );
            if ( removeEc )
            {
                fs::remove( to, ec );
                return MakeError( "move '" + from.string() +
                                  "': the copy was written but the source could not be "
                                  "removed (" +
                                  removeEc.message() + ")" );
            }
            return MakeSuccess( true );
        }

        ResultStr<Utils::AssetRegistryEntry> RowAt( const fs::path& file, ContentKind kind )
        {
            return RegistryRowFor( AssetHandle::StableKeyForPath( file ), DescribeContentFile( file, kind ) );
        }
    } // namespace

    std::vector<std::string> ReferrersOf( const Utils::AssetRegistry&      registry,
                                          const Utils::AssetRegistryEntry& row )
    {
        std::vector<uint64_t> names = { row.PathHandle(), row.EffectiveHandle() };
        if ( row.Guid.has_value() )
            names.push_back( HandleForGuid( *row.Guid ) );
        std::vector<std::string> keys;
        for ( const Utils::AssetRegistryEntry& other : registry.Entries() )
        {
            // A redirector naming the row is the move's own trace, not a referrer anyone edits.
            if ( other.Key == row.Key || other.Kind == KindName( ContentKind::Redirector ) )
                continue;
            const bool namesIt =
                 std::any_of( other.Dependencies.begin(), other.Dependencies.end(), [&]( uint64_t edge )
                              { return std::find( names.begin(), names.end(), edge ) != names.end(); } );
            if ( namesIt )
                keys.push_back( other.Key );
        }
        return keys;
    }

    ResultStr<AssetMoveRecord> MoveAssetLeavingRedirector( Utils::AssetRegistry& registry, const fs::path& from,
                                                           const fs::path&                 to,
                                                           const std::optional<AssetGuid>& redirectorSelf )
    {
        const std::string oldKey = AssetHandle::StableKeyForPath( from );
        const std::string newKey = AssetHandle::StableKeyForPath( to );
        std::error_code   ec;
        if ( fs::exists( to, ec ) )
            return MakeError<AssetMoveRecord>( "move '" + oldKey + "': '" + to.string() +
                                               "' already exists; refusing to overwrite it" );
        if ( newKey.empty() )
            return MakeError<AssetMoveRecord>( "move '" + oldKey + "': '" + to.string() +
                                               "' is outside every content root" );
        if ( from.extension() != to.extension() )
            return MakeError<AssetMoveRecord>( "move '" + oldKey + "' -> '" + newKey +
                                               "': the extension changes ('" + from.extension().string() +
                                               "' -> '" + to.extension().string() +
                                               "'), which would change the asset's kind" );
        const Utils::AssetRegistryEntry* known = registry.FindByKey( oldKey );
        if ( known == nullptr )
            return MakeError<AssetMoveRecord>( "move '" + oldKey + "': the registry has no row for it" );
        if ( known->Kind == KindName( ContentKind::Redirector ) )
            return MakeError<AssetMoveRecord>( "move '" + oldKey +
                                               "': it is a redirector; move its target instead" );
        if ( !known->Guid.has_value() )
            return MakeError<AssetMoveRecord>( "move '" + oldKey + "': its header states no GUID (kind " +
                                               known->Kind + "), so no redirector could name it" );
        const std::optional<ContentKind> kind = KindNamed( known->Kind );
        if ( !kind )
            return MakeError<AssetMoveRecord>( "move '" + oldKey + "': unknown kind '" + known->Kind + "'" );

        AssetMoveRecord record{
             from, to, AssetRedirector{ redirectorSelf.value_or( AssetGuid::Generate() ), *known->Guid, oldKey },
             *known };

        if ( const auto moved = MoveFileAtomic( from, to ); !moved )
            return MakeError<AssetMoveRecord>( moved.GetError() );
        const auto rollbackFile = [&]
        {
            fs::remove( from, ec );
            (void)MoveFileAtomic( to, from );
        };
        if ( const auto written = WriteRedirectorFile( from, record.Redirector ); !written )
        {
            rollbackFile();
            return MakeError<AssetMoveRecord>( "move '" + oldKey + "': " + written.GetError() );
        }

        auto movedRow      = RowAt( to, *kind );
        auto redirectorRow = RowAt( from, ContentKind::Redirector );
        if ( !movedRow || !redirectorRow )
        {
            rollbackFile();
            return MakeError<AssetMoveRecord>(
                 "move '" + oldKey + "': " + ( movedRow ? redirectorRow.GetError() : movedRow.GetError() ) );
        }
        // The file did not change, so what the cook learned from loading it still holds.
        Utils::AssetRegistryEntry row = movedRow.GetValue();
        row.Dependencies              = known->Dependencies;
        row.Bounds                    = known->Bounds;

        registry.Remove( oldKey );
        const auto insertedMoved = registry.Insert( std::move( row ) );
        const auto insertedRedirector =
             insertedMoved ? registry.Insert( redirectorRow.GetValue() ) : BoolResultStr( MakeSuccess( true ) );
        if ( !insertedMoved || !insertedRedirector )
        {
            registry.Remove( newKey );
            registry.Remove( oldKey );
            (void)registry.Insert( record.MovedRow );
            rollbackFile();
            return MakeError<AssetMoveRecord>(
                 "move '" + oldKey +
                 "': " + ( insertedMoved ? insertedRedirector.GetError() : insertedMoved.GetError() ) );
        }
        return MakeSuccess( std::move( record ) );
    }

    BoolResultStr UndoAssetMove( Utils::AssetRegistry& registry, const AssetMoveRecord& record )
    {
        const auto found = ReadRedirectorFile( record.From );
        if ( !found || !( found.GetValue() == record.Redirector ) )
            return MakeError( "undo move: '" + record.From.string() +
                              "' is no longer the redirector the move wrote; nothing was changed" );
        std::error_code ec;
        if ( !fs::exists( record.To, ec ) )
            return MakeError( "undo move: the moved asset '" + record.To.string() +
                              "' is gone; nothing was changed" );
        if ( !fs::remove( record.From, ec ) )
            return MakeError( "undo move: could not remove the redirector '" + record.From.string() +
                              "': " + ec.message() );
        if ( const auto moved = MoveFileAtomic( record.To, record.From ); !moved )
        {
            (void)WriteRedirectorFile( record.From, record.Redirector );
            return MakeError( "undo move: " + moved.GetError() );
        }
        registry.Remove( AssetHandle::StableKeyForPath( record.To ) );
        registry.Remove( record.MovedRow.Key );
        if ( const auto restored = registry.Insert( record.MovedRow ); !restored )
            return MakeError( "undo move: the file is back but its row could not be restored: " +
                              restored.GetError() );
        return MakeSuccess( true );
    }
    namespace
    {
        bool IsInside( const fs::path& path, const fs::path& root )
        {
            const fs::path relative = path.lexically_normal().lexically_relative( root.lexically_normal() );
            return !relative.empty() && *relative.begin() != "..";
        }

        // Makes `dir` and every missing parent, recording each one it made (outermost first) so undo can
        // remove exactly those and no folder that was there before.
        BoolResultStr MakeDirectories( const fs::path& dir, std::vector<fs::path>& made )
        {
            std::vector<fs::path> missing;
            std::error_code       ec;
            for ( fs::path at = dir; !at.empty() && !fs::exists( at, ec ); at = at.parent_path() )
            {
                missing.push_back( at );
                if ( at == at.parent_path() )
                    break;
            }
            for ( const fs::path& folder : std::ranges::reverse_view( missing ) )
            {
                if ( !fs::create_directory( folder, ec ) && ec )
                    return MakeError( "move folder: could not create '" + folder.string() + "': " + ec.message() );
                made.push_back( folder );
            }
            return MakeSuccess( true );
        }

        // A plain file's move leaves its folder empty: remove it and each emptied parent below `stop`.
        void RemoveEmptiedFolders( fs::path dir, const fs::path& stop )
        {
            std::error_code ec;
            while ( IsInside( dir, stop ) && fs::is_empty( dir, ec ) && !ec && fs::remove( dir, ec ) )
                dir = dir.parent_path();
        }
    } // namespace

    BoolResultStr UndoFolderMove( Utils::AssetRegistry& registry, const AssetFolderMoveRecord& record )
    {
        std::error_code ec;
        if ( record.WholeDirectory )
        {
            fs::rename( record.To, record.From, ec );
            if ( ec )
                return MakeError( "undo move folder: '" + record.To.string() + "' -> '" + record.From.string() +
                                  "': " + ec.message() );
        }
        for ( const auto& [plainFrom, plainTo] : std::ranges::reverse_view( record.PlainFiles ) )
        {
            fs::create_directories( plainFrom.parent_path(), ec );
            if ( const auto moved = MoveFileAtomic( plainTo, plainFrom ); !moved )
                return MakeError( "undo move folder: " + moved.GetError() );
        }
        for ( const AssetMoveRecord& asset : std::ranges::reverse_view( record.Assets ) )
            if ( const auto undone = UndoAssetMove( registry, asset ); !undone )
                return MakeError( "undo move folder '" + record.From.string() + "': " + undone.GetError() );
        // Newest first, so a made folder is removed before the made folder holding it.
        for ( const fs::path& made : std::ranges::reverse_view( record.CreatedDirectories ) )
            if ( !fs::remove( made, ec ) || ec )
                return MakeError( "undo move folder: the folder the move made, '" + made.string() +
                                  "', could not be removed (" + ( ec ? ec.message() : "missing" ) + ")" );
        return MakeSuccess( true );
    }

    ResultStr<AssetFolderMoveRecord>
    MoveFolderLeavingRedirectors( Utils::AssetRegistry& registry, const fs::path& from, const fs::path& to,
                                  const std::map<fs::path, AssetGuid>& redirectorSelves )
    {
        std::error_code ec;
        if ( !fs::is_directory( from, ec ) )
            return MakeError<AssetFolderMoveRecord>( "move folder '" + from.string() + "': not a folder" );
        if ( IsInside( to, from ) || to.lexically_normal() == from.lexically_normal() )
            return MakeError<AssetFolderMoveRecord>( "move folder '" + from.string() + "': the destination '" +
                                                     to.string() + "' is inside it" );

        AssetFolderMoveRecord record{ from, to, false, {}, {}, {} };
        std::vector<fs::path> files = Utils::FileSystem::ListFilesRecursive( from );
        std::sort( files.begin(), files.end() );
        bool anyRow = false;
        for ( const fs::path& file : files )
        {
            if ( !fs::is_regular_file( file, ec ) )
                return MakeError<AssetFolderMoveRecord>( "move folder '" + from.string() + "': '" + file.string() +
                                                         "' exists only in a mounted pak and cannot be moved" );
            anyRow = anyRow || registry.FindByKey( AssetHandle::StableKeyForPath( file ) ) != nullptr;
        }

        if ( !anyRow )
        {
            if ( const auto made = MakeDirectories( to.parent_path(), record.CreatedDirectories ); !made )
                return MakeError<AssetFolderMoveRecord>( made.GetError() );
            fs::rename( from, to, ec );
            if ( ec )
            {
                record.WholeDirectory = false;
                static_cast<void>( UndoFolderMove( registry, record ) );
                return MakeError<AssetFolderMoveRecord>( "move folder '" + from.string() + "' -> '" + to.string() +
                                                         "': " + ec.message() );
            }
            record.WholeDirectory = true;
            return MakeSuccess( std::move( record ) );
        }

        // Any refusal from here takes back everything moved so far; a take-back that itself fails is named
        // too, because then the disk is between the two states and the user must know which files moved.
        const auto refuse = [&]( const std::string& why ) -> ResultStr<AssetFolderMoveRecord>
        {
            if ( const auto back = UndoFolderMove( registry, record ); !back )
                return MakeError<AssetFolderMoveRecord>( "move folder '" + from.string() + "': " + why +
                                                         "; AND the take-back failed: " + back.GetError() );
            return MakeError<AssetFolderMoveRecord>( "move folder '" + from.string() + "': " + why +
                                                     "; nothing was moved" );
        };
        for ( const fs::path& file : files )
        {
            const fs::path target = to / file.lexically_relative( from );
            if ( const auto made = MakeDirectories( target.parent_path(), record.CreatedDirectories ); !made )
                return refuse( made.GetError() );
            if ( registry.FindByKey( AssetHandle::StableKeyForPath( file ) ) != nullptr )
            {
                const auto self  = redirectorSelves.find( file );
                auto       moved = MoveAssetLeavingRedirector(
                     registry, file, target,
                     self != redirectorSelves.end() ? std::optional<AssetGuid>( self->second ) : std::nullopt );
                if ( !moved )
                    return refuse( moved.GetError() );
                record.Assets.push_back( moved.ExtractValue() );
                continue;
            }
            if ( fs::exists( target, ec ) )
                return refuse( "'" + target.string() + "' already exists; refusing to overwrite it" );
            if ( const auto moved = MoveFileAtomic( file, target ); !moved )
                return refuse( moved.GetError() );
            record.PlainFiles.emplace_back( file, target );
        }
        for ( const auto& [plainFrom, plainTo] : record.PlainFiles )
            RemoveEmptiedFolders( plainFrom.parent_path(), from );
        return MakeSuccess( std::move( record ) );
    }
} // namespace Common::Content
