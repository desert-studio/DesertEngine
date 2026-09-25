#include <Common/Content/AssetMove.hpp>

#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/ContentScan.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
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
} // namespace Common::Content
