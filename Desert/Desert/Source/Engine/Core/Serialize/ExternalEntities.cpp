#include <Engine/Core/Serialize/ExternalEntities.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <spdlog/fmt/fmt.h>

#include <cstdint>
#include <optional>
#include <system_error>
#include <unordered_set>

namespace Desert::Core::ExternalEntities
{
    namespace
    {
        // The two members this file reads off a JSON tree; every other member is carried as text.
        struct RecordIdentity
        {
            std::optional<std::uint64_t> id;
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
    } // namespace

    std::filesystem::path DirectoryOf( const std::filesystem::path& scenePath )
    {
        return scenePath.parent_path() / kFolder / scenePath.stem();
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
        auto split = Split( scene, scenePath.string() );
        if ( !split )
            return Common::MakeError<WriteOutcome>( split.GetError() );

        WriteOutcome                    outcome;
        std::unordered_set<std::string> claimed;
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
        auto onDisk = PiecesOnDisk( scenePath );
        if ( !onDisk )
            return Common::MakeError<WriteOutcome>( onDisk.GetError() );
        for ( const std::filesystem::path& piece : onDisk.GetValue() )
        {
            if ( claimed.count( piece.lexically_normal().generic_string() ) != 0 )
                continue;
            std::error_code ec;
            if ( !std::filesystem::remove( piece, ec ) || ec )
                return Common::MakeError<WriteOutcome>(
                     fmt::format( "could not remove {}, the file of an entity the scene no longer has: {}",
                                  piece.string(), ec ? ec.message() : "not removed" ) );
            ++outcome.Removed;
        }
        return Common::MakeSuccess( outcome );
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
} // namespace Desert::Core::ExternalEntities
