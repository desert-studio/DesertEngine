#include <Engine/Core/Serialize/ExternalEntities.hpp>

#include <Engine/Core/Serialize/EntityDescriptorIndex.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace Desert::Core::ExternalEntities
{
    namespace
    {
        // The two members this file reads off a JSON tree; every other member is carried as text.
        struct RecordIdentity
        {
            std::optional<std::uint64_t> id;
        };
        struct RecordTag
        {
            std::optional<std::string> Tag;
        };
        struct HeaderList
        {
            std::vector<std::uint64_t> ExternalEntities;
        };

        std::uint64_t Bits( Common::UUID id )
        {
            return static_cast<std::uint64_t>( id );
        }

        Common::ResultStr<Common::UUID> IdOf( const Common::Json::TextDocument& record )
        {
            auto identity = record.AsDocument<RecordIdentity>();
            if ( !identity )
                return Common::MakeError<Common::UUID>( identity.GetError() );
            if ( !identity.GetValue().id.has_value() )
                return Common::MakeError<Common::UUID>( "the record states no id" );
            return Common::MakeSuccess( Common::UUID( *identity.GetValue().id ) );
        }

        bool HasMember( const Common::Json::TextDocument& document, std::string_view member )
        {
            for ( const std::string& key : document.KeysAt() )
                if ( key == member )
                    return true;
            return false;
        }

        // Only a file whose bytes differ is written: an unchanged entity keeps its file untouched, which is
        // what lets an edit to one entity show up as a change to exactly one file.
        Common::BoolResultStr WriteIfChanged( const std::filesystem::path& path, const std::string& text,
                                              WriteOutcome& outcome )
        {
            std::error_code ec;
            if ( std::filesystem::is_regular_file( path, ec ) )
            {
                const auto existing = Common::Utils::FileSystem::ReadFileContent( path );
                if ( existing && existing.GetValue() == text )
                {
                    ++outcome.Unchanged;
                    return BOOLSUCCESS;
                }
            }
            std::filesystem::create_directories( path.parent_path(), ec );
            if ( ec )
                return Common::MakeFormattedError( "could not create the directory {}: {}",
                                                   path.parent_path().string(), ec.message() );
            if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, text ); !written )
                return Common::MakeFormattedError( "could not write {}: {}", path.string(), written.GetError() );
            ++outcome.Written;
            return BOOLSUCCESS;
        }

        // Every `.deent` below the scene's folder, as the set of paths a caller compares against the list.
        Common::ResultStr<std::vector<std::filesystem::path>>
        PiecesOnDisk( const std::filesystem::path& scenePath )
        {
            // Through the one content enumeration: a packaged world's pieces live in a mounted .dpak, and a
            // missing folder contributes nothing.
            std::vector<std::filesystem::path> pieces;
            for ( const std::filesystem::path& file :
                  Common::Utils::FileSystem::ListFilesRecursive( DirectoryOf( scenePath ) ) )
            {
                if ( file.extension() == kExtension )
                    pieces.push_back( file );
            }
            return Common::MakeSuccess( std::move( pieces ) );
        }

        // Deletes every `.deent` below DirectoryOf(scenePath) that `claimed` does not name, then the scene's
        // folder and the `__ExternalEntities__` folder above it if that left them empty.
        Common::BoolResultStr RemoveUnclaimed( const std::filesystem::path&           scenePath,
                                               const std::unordered_set<std::string>& claimed,
                                               WriteOutcome&                          outcome )
        {
            auto onDisk = PiecesOnDisk( scenePath );
            if ( !onDisk )
                return Common::MakeError( onDisk.GetError() );
            for ( const std::filesystem::path& piece : onDisk.GetValue() )
            {
                if ( claimed.contains( piece.lexically_normal().generic_string() ) )
                    continue;
                std::error_code ec;
                if ( !std::filesystem::remove( piece, ec ) || ec )
                    return Common::MakeFormattedError(
                         "could not remove {}, the file of an entity the scene no longer has: {}", piece.string(),
                         ec ? ec.message() : "not removed" );
                ++outcome.Removed;
                // The bucket folder, if this was its last file: an empty folder is not an entity, but it is a
                // leftover a rename would have to carry and a reviewer would have to explain.
                std::filesystem::remove( piece.parent_path(), ec );
            }
            std::error_code ec;
            std::filesystem::remove( DirectoryOf( scenePath ), ec );               // only when empty
            std::filesystem::remove( DirectoryOf( scenePath ).parent_path(), ec ); // only when empty
            return BOOLSUCCESS;
        }
    } // namespace

    std::filesystem::path DirectoryOf( const std::filesystem::path& scenePath )
    {
        return Common::Content::ExternalEntitiesDirectoryOf( scenePath );
    }

    std::filesystem::path FileOf( const std::filesystem::path& scenePath, Common::UUID id )
    {
        return DirectoryOf( scenePath ) / fmt::format( "{:02x}", Bits( id ) & 0xFFu ) /
               ( std::to_string( Bits( id ) ) + std::string( kExtension ) );
    }

    bool IsHeader( const Common::Json::TextDocument& document )
    {
        return HasMember( document, kListMember );
    }

    Common::ResultStr<SplitScene> Split( const Common::Json::TextDocument& scene, std::string_view source )
    {
        auto records = scene.RecordsAt( kRecords );
        if ( !records )
            return Common::MakeError<SplitScene>( fmt::format( "'{}': {}", source, records.GetError() ) );

        SplitScene                              split;
        std::vector<Common::Json::TextDocument> ids;
        std::unordered_set<std::uint64_t>       seen;
        for ( std::size_t i = 0; i < records.GetValue().size(); ++i )
        {
            const Common::Json::TextDocument& record = records.GetValue()[i];
            const auto                        id     = IdOf( record );
            if ( !id )
                return Common::MakeError<SplitScene>( fmt::format(
                     "'{}': entity record {} cannot be given its own file: {}", source, i, id.GetError() ) );
            if ( !seen.insert( Bits( id.GetValue() ) ).second )
                return Common::MakeError<SplitScene>( fmt::format(
                     "'{}': entity record {} states id {}, which an earlier record already states - two entities "
                     "cannot share one file",
                     source, i, Bits( id.GetValue() ) ) );
            auto idDocument = Common::Json::TextDocument::Parse( std::to_string( Bits( id.GetValue() ) ) );
            if ( !idDocument )
                return Common::MakeError<SplitScene>( idDocument.GetError() );
            ids.push_back( idDocument.ExtractValue() );
            split.Records.emplace_back( id.GetValue(), record );
        }

        auto header = scene.WithArrayMember( kRecords, kListMember, ids );
        if ( !header )
            return Common::MakeError<SplitScene>( fmt::format( "'{}': {}", source, header.GetError() ) );
        split.Header = header.ExtractValue();
        return Common::MakeSuccess( std::move( split ) );
    }

    Common::ResultStr<Common::Json::TextDocument>
    Assemble( const Common::Json::TextDocument& header, std::string_view source, const RecordReader& readRecord )
    {
        using Result = Common::Json::TextDocument;
        auto list    = header.AsDocument<HeaderList>();
        if ( !list )
            return Common::MakeError<Result>( fmt::format(
                 "'{}': the entity list of a partitioned world cannot be read: {}", source, list.GetError() ) );

        std::vector<Common::Json::TextDocument> records;
        std::unordered_set<std::uint64_t>       seen;
        for ( const std::uint64_t bits : list.GetValue().ExternalEntities )
        {
            const Common::UUID id( bits );
            if ( !seen.insert( Bits( id ) ).second )
                return Common::MakeError<Result>(
                     fmt::format( "'{}': entity {} is listed twice. Nothing was loaded.", source, Bits( id ) ) );
            auto text = readRecord( id );
            if ( !text )
                return Common::MakeError<Result>( fmt::format( "'{}': entity {}: {}. Nothing was loaded.", source,
                                                               Bits( id ), text.GetError() ) );
            auto record = Common::Json::TextDocument::Parse( text.GetValue() );
            if ( !record )
                return Common::MakeError<Result>(
                     fmt::format( "'{}': entity {}: its file is not JSON ({}). Nothing "
                                  "was loaded.",
                                  source, Bits( id ), record.GetError() ) );
            const auto stated = IdOf( record.GetValue() );
            if ( !stated || Bits( stated.GetValue() ) != Bits( id ) )
                return Common::MakeError<Result>( fmt::format(
                     "'{}': entity {}: its file states {} - a file is the record it is named for. Nothing was "
                     "loaded.",
                     source, Bits( id ),
                     stated ? "id " + std::to_string( Bits( stated.GetValue() ) ) : stated.GetError() ) );
            records.push_back( record.ExtractValue() );
        }

        auto scene = header.WithArrayMember( kListMember, kRecords, records );
        if ( !scene )
            return Common::MakeError<Result>( fmt::format( "'{}': {}", source, scene.GetError() ) );
        return scene;
    }

    Common::ResultStr<WriteOutcome> WriteSceneFile( const std::filesystem::path&      scenePath,
                                                    const Common::Json::TextDocument& scene )
    {
        WriteOutcome                    outcome;
        std::unordered_set<std::string> claimed;
        if ( !HasMember( scene, "WorldPartition" ) )
        {
            const auto text = Common::Json::WriteCanonical( scene );
            if ( !text )
                return Common::MakeError<WriteOutcome>(
                     fmt::format( "could not lay out {} as text: {}", scenePath.string(), text.GetError() ) );
            if ( const auto written = WriteIfChanged( scenePath, text.GetValue(), outcome ); !written )
                return Common::MakeError<WriteOutcome>( written.GetError() );
            // A scene that is no longer partitioned has no descriptor index: removed before the folder it sits in
            // is cleared, so a stale index never outlives the entities it described.
            std::error_code ec;
            std::filesystem::remove( DescriptorIndex::PathOf( scenePath ), ec );
            if ( const auto removed = RemoveUnclaimed( scenePath, claimed, outcome ); !removed )
                return Common::MakeError<WriteOutcome>( removed.GetError() );
            return Common::MakeSuccess( outcome );
        }

        auto split = Split( scene, scenePath.string() );
        if ( !split )
            return Common::MakeError<WriteOutcome>( split.GetError() );
        std::vector<Common::UUID>                      listed;
        std::unordered_map<std::uint64_t, std::string> texts;
        for ( const auto& [id, record] : split.GetValue().Records )
        {
            const std::filesystem::path file = FileOf( scenePath, id );
            const auto                  text = Common::Json::WriteCanonical( record );
            if ( !text )
                return Common::MakeError<WriteOutcome>(
                     fmt::format( "could not lay out entity {} ({}) as text: {}", Bits( id ), file.string(),
                                  text.GetError() ) );
            if ( const auto written = WriteIfChanged( file, text.GetValue(), outcome ); !written )
                return Common::MakeError<WriteOutcome>( written.GetError() );
            claimed.insert( file.lexically_normal().generic_string() );
            listed.push_back( id );
            texts.emplace( Bits( id ), text.GetValue() );
        }

        // The header after its records: a write that stops half way leaves the old list naming files that
        // still exist, never a new list naming files that were not written.
        const auto headerText = Common::Json::WriteCanonical( split.GetValue().Header );
        if ( !headerText )
            return Common::MakeError<WriteOutcome>(
                 fmt::format( "could not lay out {} as text: {}", scenePath.string(), headerText.GetError() ) );
        if ( const auto written = WriteIfChanged( scenePath, headerText.GetValue(), outcome ); !written )
            return Common::MakeError<WriteOutcome>( written.GetError() );

        // The files of deleted entities. Left in place they would be refused on the next load (a piece the
        // list does not name), so the save that dropped the entity drops its file.
        if ( const auto removed = RemoveUnclaimed( scenePath, claimed, outcome ); !removed )
            return Common::MakeError<WriteOutcome>( removed.GetError() );

        // The descriptor index follows every save (WP18): the texts just written are its input, so only the
        // entities whose file changed are re-described and an unchanged world leaves the index untouched.
        const auto indexed =
             DescriptorIndex::Refresh( scenePath, listed, [&]( Common::UUID id ) -> Common::ResultStr<std::string>
                                       { return Common::MakeSuccess( texts.at( Bits( id ) ) ); } );
        if ( !indexed )
            return Common::MakeError<WriteOutcome>( indexed.GetError() );
        return Common::MakeSuccess( outcome );
    }

    Common::ResultStr<WriteOutcome> WriteSceneDelta( const std::filesystem::path&      scenePath,
                                                     const Common::Json::TextDocument& scene,
                                                     std::span<const Common::UUID>     listed,
                                                     std::span<const Common::UUID>     changed,
                                                     std::span<const Common::UUID>     removed )
    {
        WriteOutcome outcome;
        auto         split = Split( scene, scenePath.string() );
        if ( !split )
            return Common::MakeError<WriteOutcome>( split.GetError() );

        std::unordered_set<std::uint64_t> wanted;
        for ( const Common::UUID id : changed )
            wanted.insert( Bits( id ) );
        std::unordered_map<std::uint64_t, std::string> texts;
        for ( const auto& [id, record] : split.GetValue().Records )
        {
            if ( !wanted.contains( Bits( id ) ) )
                continue;
            const std::filesystem::path file = FileOf( scenePath, id );
            const auto                  text = Common::Json::WriteCanonical( record );
            if ( !text )
                return Common::MakeError<WriteOutcome>(
                     fmt::format( "could not lay out entity {} ({}) as text: {}", Bits( id ), file.string(),
                                  text.GetError() ) );
            if ( const auto written = WriteIfChanged( file, text.GetValue(), outcome ); !written )
                return Common::MakeError<WriteOutcome>( written.GetError() );
            texts.emplace( Bits( id ), text.GetValue() );
        }
        for ( const Common::UUID id : changed )
            if ( !texts.contains( Bits( id ) ) )
                return Common::MakeError<WriteOutcome>(
                     fmt::format( "'{}': entity {} is to be written but the scene composed no record for it",
                                  scenePath.string(), Bits( id ) ) );

        // The header states the WHOLE list: the document carried only the changed records.
        std::vector<Common::Json::TextDocument> ids;
        ids.reserve( listed.size() );
        for ( const Common::UUID id : listed )
        {
            auto idDocument = Common::Json::TextDocument::Parse( std::to_string( Bits( id ) ) );
            if ( !idDocument )
                return Common::MakeError<WriteOutcome>( idDocument.GetError() );
            ids.push_back( idDocument.ExtractValue() );
        }
        const auto header = scene.WithArrayMember( kRecords, kListMember, ids );
        if ( !header )
            return Common::MakeError<WriteOutcome>( fmt::format( "'{}': {}", scenePath.string(), header.GetError() ) );
        const auto headerText = Common::Json::WriteCanonical( header.GetValue() );
        if ( !headerText )
            return Common::MakeError<WriteOutcome>(
                 fmt::format( "could not lay out {} as text: {}", scenePath.string(), headerText.GetError() ) );
        if ( const auto written = WriteIfChanged( scenePath, headerText.GetValue(), outcome ); !written )
            return Common::MakeError<WriteOutcome>( written.GetError() );

        // After the header, as in WriteSceneFile: the old list never names a file that is already gone.
        for ( const Common::UUID id : removed )
        {
            const std::filesystem::path file = FileOf( scenePath, id );
            std::error_code             ec;
            if ( !std::filesystem::exists( file, ec ) )
                continue;
            if ( !std::filesystem::remove( file, ec ) || ec )
                return Common::MakeError<WriteOutcome>(
                     fmt::format( "could not remove {}, the file of an entity the scene no longer has: {}",
                                  file.string(), ec ? ec.message() : "not removed" ) );
            ++outcome.Removed;
            std::filesystem::remove( file.parent_path(), ec ); // only when empty
        }

        const auto indexed = DescriptorIndex::Refresh(
             scenePath, listed,
             [&]( Common::UUID id ) -> Common::ResultStr<std::string>
             {
                 if ( const auto found = texts.find( Bits( id ) ); found != texts.end() )
                     return Common::MakeSuccess( found->second );
                 return Common::Utils::FileSystem::ReadFileContent( FileOf( scenePath, id ) );
             },
             [&]( Common::UUID id ) { return !texts.contains( Bits( id ) ); } );
        if ( !indexed )
            return Common::MakeError<WriteOutcome>( indexed.GetError() );
        return Common::MakeSuccess( outcome );
    }

    Common::BoolResultStr VerifyCleanRecords( const std::filesystem::path&      scenePath,
                                              const Common::Json::TextDocument& scene,
                                              std::span<const Common::UUID>     clean )
    {
        auto split = Split( scene, scenePath.string() );
        if ( !split )
            return Common::MakeError( split.GetError() );

        std::unordered_set<std::uint64_t> wanted;
        for ( const Common::UUID id : clean )
            wanted.insert( Bits( id ) );
        std::unordered_set<std::uint64_t> seen;
        for ( const auto& [id, record] : split.GetValue().Records )
        {
            if ( !wanted.contains( Bits( id ) ) )
                continue;
            seen.insert( Bits( id ) );
            const std::filesystem::path file = FileOf( scenePath, id );
            const auto                  text = Common::Json::WriteCanonical( record );
            if ( !text )
                return Common::MakeFormattedError( "could not lay out entity {} ({}) as text: {}", Bits( id ),
                                                   file.string(), text.GetError() );
            const auto onDisk = Common::Utils::FileSystem::ReadFileContent( file );
            if ( onDisk && onDisk.GetValue() == text.GetValue() )
                continue;
            const auto tag = record.AsDocument<RecordTag>();
            return Common::MakeFormattedError(
                 "entity '{}' ({}) differs from its file {} but nothing marked it modified - an edit that "
                 "bypassed "
                 "Scene::MarkModified and the command history would be lost by this save",
                 tag && tag.GetValue().Tag ? *tag.GetValue().Tag : std::string( "Entity" ), Bits( id ),
                 onDisk ? file.string() : fmt::format( "{} (unreadable: {})", file.string(), onDisk.GetError() ) );
        }
        for ( const Common::UUID id : clean )
            if ( !seen.contains( Bits( id ) ) )
                return Common::MakeFormattedError(
                     "'{}': entity {} is held clean but the scene composed no record for it", scenePath.string(),
                     Bits( id ) );
        return BOOLSUCCESS;
    }

    Common::ResultStr<WriteOutcome> WriteSceneText( const std::filesystem::path& scenePath, std::string_view json )
    {
        auto document = Common::Json::TextDocument::Parse( std::string( json ) );
        if ( !document )
            return Common::MakeError<WriteOutcome>(
                 fmt::format( "the text meant for {} is not JSON: {}", scenePath.string(), document.GetError() ) );
        return WriteSceneFile( scenePath, document.GetValue() );
    }

    Common::ResultStr<std::vector<Common::UUID>> ListedEntities( const std::filesystem::path& path )
    {
        using Result = std::vector<Common::UUID>;
        auto text    = Common::Utils::FileSystem::ReadFileContent( path );
        if ( !text )
            return Common::MakeError<Result>( text.GetError() );
        auto document = Common::Json::TextDocument::Parse( text.GetValue() );
        if ( !document )
            return Common::MakeError<Result>( fmt::format( "'{}' is not JSON: {}", path.string(), document.GetError() ) );
        if ( !IsHeader( document.GetValue() ) )
            return Common::MakeError<Result>(
                 fmt::format( "'{}' is not a partitioned world's header: it lists no {}", path.string(), kListMember ) );
        auto list = document.GetValue().AsDocument<HeaderList>();
        if ( !list )
            return Common::MakeError<Result>(
                 fmt::format( "'{}': the entity list cannot be read: {}", path.string(), list.GetError() ) );
        Result ids;
        for ( const std::uint64_t bits : list.GetValue().ExternalEntities )
            ids.emplace_back( bits );
        return Common::MakeSuccess( std::move( ids ) );
    }

    Common::ResultStr<std::string> ReadSceneFileText( const std::filesystem::path& path )
    {
        auto text = Common::Utils::FileSystem::ReadFileContent( path );
        if ( !text )
            return text;
        // A scene that names neither member is returned as read, unparsed: the 150 scenes that are not
        // partitioned pay nothing for this.
        if ( text.GetValue().find( "\"WorldPartition\"" ) == std::string::npos &&
             text.GetValue().find( fmt::format( "\"{}\"", kListMember ) ) == std::string::npos )
            return text;

        auto document = Common::Json::TextDocument::Parse( text.GetValue() );
        if ( !document )
            return text; // not JSON: ParseLoadableScene names the byte, with its own wording
        const bool partitioned = HasMember( document.GetValue(), "WorldPartition" );
        const bool header      = IsHeader( document.GetValue() );
        if ( !partitioned && !header )
            return text;
        if ( !header )
            return Common::MakeError<std::string>( fmt::format(
                 "[SceneSerializer] '{0}' is a partitioned world that states its entities inside the scene file - "
                 "the layout before scene v35. A partitioned world keeps one file per entity under {1}. Nothing "
                 "was loaded. Run: scripts/Dev/migrate.sh --write \"{0}\"",
                 path.string(), DirectoryOf( path ).string() ) );
        if ( !partitioned )
            return Common::MakeError<std::string>( fmt::format(
                 "[SceneSerializer] '{}' lists external entities but states no WorldPartition block - "
                 "only a partitioned world keeps its entities in separate files. Nothing was loaded.",
                 path.string() ) );

        std::unordered_set<std::string> listed;
        const auto                      read = [&]( Common::UUID id ) -> Common::ResultStr<std::string>
        {
            const std::filesystem::path file = FileOf( path, id );
            listed.insert( file.lexically_normal().generic_string() );
            std::error_code ec;
            if ( !std::filesystem::is_regular_file( file, ec ) )
                return Common::MakeError<std::string>(
                     fmt::format( "its file {} does not exist", file.string() ) );
            return Common::Utils::FileSystem::ReadFileContent( file );
        };
        auto scene = Assemble( document.GetValue(), path.string(), read );
        if ( !scene )
            return Common::MakeError<std::string>( "[SceneSerializer] " + scene.GetError() );

        auto onDisk = PiecesOnDisk( path );
        if ( !onDisk )
            return Common::MakeError<std::string>( "[SceneSerializer] " + onDisk.GetError() );
        for ( const std::filesystem::path& piece : onDisk.GetValue() )
            if ( listed.count( piece.lexically_normal().generic_string() ) == 0 )
                return Common::MakeError<std::string>( fmt::format(
                     "[SceneSerializer] '{}': {} is an entity file the scene does not list (a delete that did not "
                     "finish, or a merge that kept the file and dropped the entity). Delete it or list it. "
                     "Nothing "
                     "was loaded.",
                     path.string(), piece.string() ) );
        return Common::MakeSuccess( scene.GetValue().Text() );
    }

    Common::ResultStr<std::string> ReadSceneRegionText( const std::filesystem::path&             path,
                                                        const std::unordered_set<std::uint64_t>& wanted )
    {
        auto text = Common::Utils::FileSystem::ReadFileContent( path );
        if ( !text )
            return text;
        auto document = Common::Json::TextDocument::Parse( text.GetValue() );
        if ( !document )
            return Common::MakeError<std::string>(
                 fmt::format( "[SceneSerializer] '{}' is not JSON: {}", path.string(), document.GetError() ) );
        if ( !IsHeader( document.GetValue() ) || !HasMember( document.GetValue(), "WorldPartition" ) )
            return Common::MakeError<std::string>( fmt::format(
                 "[SceneSerializer] '{}' is not a partitioned world's header: only a world kept one file per entity "
                 "loads by region.",
                 path.string() ) );
        auto list = document.GetValue().AsDocument<HeaderList>();
        if ( !list )
            return Common::MakeError<std::string>( fmt::format(
                 "[SceneSerializer] '{}': the entity list cannot be read: {}", path.string(), list.GetError() ) );

        std::vector<Common::Json::TextDocument> kept;
        std::size_t                             found = 0;
        for ( const std::uint64_t bits : list.GetValue().ExternalEntities )
        {
            if ( !wanted.contains( bits ) )
                continue;
            ++found;
            auto idDocument = Common::Json::TextDocument::Parse( std::to_string( bits ) );
            if ( !idDocument )
                return Common::MakeError<std::string>( idDocument.GetError() );
            kept.push_back( idDocument.ExtractValue() );
        }
        if ( found != wanted.size() )
            for ( const std::uint64_t bits : wanted )
                if ( std::find( list.GetValue().ExternalEntities.begin(), list.GetValue().ExternalEntities.end(),
                                bits ) == list.GetValue().ExternalEntities.end() )
                    return Common::MakeError<std::string>( fmt::format(
                         "[SceneSerializer] '{}' does not list entity {}: a region loads only what the world holds.",
                         path.string(), bits ) );

        auto header = document.GetValue().WithArrayMember( kListMember, kListMember, kept );
        if ( !header )
            return Common::MakeError<std::string>(
                 fmt::format( "[SceneSerializer] '{}': {}", path.string(), header.GetError() ) );
        const auto read = [&]( Common::UUID id ) -> Common::ResultStr<std::string>
        {
            const std::filesystem::path file = FileOf( path, id );
            std::error_code             ec;
            if ( !std::filesystem::is_regular_file( file, ec ) )
                return Common::MakeError<std::string>( fmt::format( "its file {} does not exist", file.string() ) );
            return Common::Utils::FileSystem::ReadFileContent( file );
        };
        auto scene = Assemble( header.GetValue(), path.string(), read );
        if ( !scene )
            return Common::MakeError<std::string>( "[SceneSerializer] " + scene.GetError() );
        return Common::MakeSuccess( scene.GetValue().Text() );
    }
} // namespace Desert::Core::ExternalEntities
