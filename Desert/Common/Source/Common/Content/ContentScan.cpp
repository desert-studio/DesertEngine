#include <Common/Content/ContentScan.hpp>

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/AssetRedirector.hpp>
#include <Common/Content/TextAssetHeader.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Core/Core.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <string>
#include <cstdio>

namespace Common::Content
{
    namespace
    {
        std::string LowerExtension( const std::filesystem::path& file )
        {
            std::string ext = file.extension().string();
            std::transform( ext.begin(), ext.end(), ext.begin(),
                            []( unsigned char c ) { return static_cast<char>( ::tolower( c ) ); } );
            return ext;
        }

        // IS THIS FILE CONTENT, AND OF WHICH KIND — asked of a path rather than by walking a root, so
        // that the two lists in this file classify identically. The disk scan can iterate the roots;
        // the tracked list arrives as a flat list of repository paths and has to ask per file. One
        // answer, one census, two callers.
        //
        // The ROOT is tested and not only the extension: `Constants.hpp` puts `Clouds/Types` inside
        // `Clouds/`, so a `.dcnv` dropped into `Clouds/Types` is not a cloud type and a `.decloudtype`
        // loose in `Clouds/` is not enumerated by anything. Matching on the extension alone would make
        // the tracked list disagree with the walk about exactly those files.
        bool IsUnder( const std::filesystem::path& file, const std::filesystem::path& root )
        {
            std::error_code             ec;
            const std::filesystem::path absoluteFile =
                 file.is_absolute() ? file.lexically_normal()
                                    : std::filesystem::absolute( file, ec ).lexically_normal();
            const std::filesystem::path absoluteRoot =
                 root.is_absolute() ? root.lexically_normal()
                                    : std::filesystem::absolute( root, ec ).lexically_normal();
            if ( ec )
                return false;

            const std::string relative = absoluteFile.lexically_relative( absoluteRoot ).generic_string();
            return !relative.empty() && relative != "." && !relative.starts_with( ".." );
        }

        // Runs a command and returns its stdout, or an EMPTY STRING when the command itself failed.
        //
        // THE EXIT STATUS IS READ, and that is the whole reason the caller can return an optional.
        // `git` in a directory that is not a checkout prints nothing and exits non-zero, which is
        // byte-identical to a repository that tracks no content — and reporting that as "nothing is
        // tracked" would make the gate certify an empty answer. Exactly the failure
        // `scripts/CI/CheckTidy.sh` reserves its exit code 2 for.
        // `popen`/`pclose` ARE POSIX AND MSVC SPELLS THEM `_popen`/`_pclose`. This is a recorded
        // defect class of this repository and it reached `dev` again here: `Windows Shipping` failed
        // with `error C3861: 'popen': identifier not found`, on the platform the game actually ships
        // for, while every macOS suite was green. A green sweep here is not a green build there.
        std::string RunAndCapture( const std::string& command )
        {
#if defined( DESERT_PLATFORM_WINDOWS )
            FILE* pipe = _popen( command.c_str(), "r" );
#else
            FILE* pipe = popen( command.c_str(), "r" );
#endif
            if ( pipe == nullptr )
                return {};

            std::string output;
            char        buffer[4096];
            std::size_t read = 0;
            while ( ( read = std::fread( buffer, 1, sizeof( buffer ), pipe ) ) > 0 )
                output.append( buffer, read );

#if defined( DESERT_PLATFORM_WINDOWS )
            if ( _pclose( pipe ) != 0 )
#else
            if ( pclose( pipe ) != 0 )
#endif
                return {};
            return output;
        }

        // The null device, spelled for the shell `popen` actually starts. `/dev/null` is not a path on
        // Windows: cmd takes it as the relative file `\\dev\\null`, cannot create it, and fails the WHOLE
        // command — so `git ls-files` returned nothing, `RunAndCapture` reported an empty string, and the
        // gate said "the question could not be asked" on every Windows run. The `popen` fix made this
        // file COMPILE there; it did not make the command RUN, and only the second of those is the point.
        constexpr const char* kNullDevice =
#if defined( DESERT_PLATFORM_WINDOWS )
             "nul";
#else
             "/dev/null";
#endif

        // Drops a trailing separator before the path is quoted, and on Windows this is a correctness
        // fix rather than tidiness. `std::filesystem::path( "./" ).string()` is `".\\"` there, and the
        // argument parser git-for-Windows uses (the MSVCRT rule) reads a backslash before a closing
        // quote as an ESCAPED QUOTE — so `git -C ".\\"` loses its quoting, the command fails, and the
        // capture returns an empty string that reads exactly like "this repository tracks nothing".
        // The root is never the bare `\\` or `/` here, so removing the last separator cannot empty it.
        std::string WithoutTrailingSeparator( std::string path )
        {
            while ( path.size() > 1 && ( path.back() == '/' || path.back() == '\\' ) )
            {
                // Keep `C:\\` whole — a drive with no directory is not the same place as `C:`.
                if ( path.size() == 3 && path[1] == ':' )
                    break;
                path.pop_back();
            }
            return path;
        }

        // Quotes a path for the shell `popen` hands the command to, so a checkout under a directory
        // with a space in it does not silently become two arguments and an empty answer.
        //
        // THE TWO SHELLS DISAGREE ABOUT WHAT A QUOTE IS, and the Windows half is not cosmetic: `_popen`
        // runs `cmd /c`, where a single quote is an ORDINARY CHARACTER and quotes nothing. The
        // POSIX spelling on Windows would therefore pass `'D:\a\My` and `Checkout'` as two arguments,
        // git would fail, and `RunAndCapture` would return an empty string — which is precisely the
        // "empty answer reported as success" this whole file exists to refuse. cmd has no escape for a
        // double quote inside a quoted string, so an embedded one is stripped rather than mangled; a
        // path cannot contain `"` on Windows anyway, which is why that is safe here and would not be
        // for arbitrary text.
        std::string QuoteForShell( const std::string& text )
        {
#if defined( DESERT_PLATFORM_WINDOWS )
            std::string quoted = "\"";
            for ( const char c : text )
            {
                if ( c != '"' )
                    quoted += c;
            }
            quoted += "\"";
            return quoted;
#else
            std::string quoted = "'";
            for ( const char c : text )
            {
                if ( c == '\'' )
                    quoted += "'\\''";
                else
                    quoted += c;
            }
            quoted += "'";
            return quoted;
#endif
        }

        // The remedy, spelled once. Every disagreement below ends with it, because a gate that names a
        // problem without naming the command that fixes it is a gate people learn to disable.
        constexpr const char* kRemedy =
             " Fix: gather again - restart the editor (it gathers the content roots at start), or "
             "`cd Editor && ../build/Bin/Debug/AssetRegistryTool cook Desert.deproj` for a cooked copy in "
             "Saved/Cooked/<Platform>. The registry is gathered, never committed.";
    } // namespace

    std::optional<ContentKind> KindOfContentFile( const std::filesystem::path& file )
    {
        const std::string extension = LowerExtension( file );
        if ( extension.empty() )
            return std::nullopt;

        // LONGEST ROOT WINS, for `AssetHandle::StableKeyForPath`'s reason: the cloud roots nest, so a
        // file under `Clouds/Types` matches both that row and the `Clouds/` one, and only the deeper
        // answer is right. Without this the answer would depend on the order of the census array.
        std::optional<ContentKind> best;
        std::size_t                bestRootLength = 0;
        for ( std::size_t i = 0; i < CONTENT_KIND_COUNT; ++i )
        {
            const auto            kind = static_cast<ContentKind>( i );
            const ContentKindSpec spec = KindSpec( kind );
            if ( spec.Extension != extension || !IsUnder( file, *spec.Root ) )
                continue;

            const std::size_t rootLength = spec.Root->generic_string().size();
            if ( !best || rootLength > bestRootLength )
            {
                best           = kind;
                bestRootLength = rootLength;
            }
        }
        return best;
    }

    ContentFile DescribeContentFile( const std::filesystem::path& file, ContentKind kind )
    {
        ContentFile described{ kind, Utils::FileSystem::GetFileSize( file ), std::nullopt, {}, {}, std::nullopt };
        if ( kind == ContentKind::StaticMesh || kind == ContentKind::SkinnedMesh )
        {
            // The mesh's box, from its 64-byte header and not its body — the reason the box is in the header.
            const auto head = Utils::FileSystem::ReadFileContentPrefix( file, sizeof( MeshBinaryFileHeader ) );
            if ( head )
                described.MeshBounds = ReadMeshHeaderBounds( head.GetValue() );
        }
        // RECORD ONLY: the versions are the loading build's to judge, not this walk's (see the context).
        const AssetHeaderReadContext context{ {}, true };
        auto                         stated = ReadAssetHeaderIfStated( file, context );
        if ( !stated )
            described.HeaderError = stated.GetError();
        else
            described.Header = stated.GetValue();
        // A REDIRECTOR'S OLD KEY IS IN ITS META SECTION, which the header read does not reach. The file is a
        // header and one small section, so reading it whole here costs what the header read did.
        if ( described.Header && described.Header->Kind == ContentKind::Redirector )
        {
            auto redirector = ReadRedirectorFile( file );
            if ( !redirector )
                described.HeaderError = redirector.GetError();
            else
                described.RedirectedFrom = redirector.GetValue().OldKey;
        }
        return described;
    }

    namespace
    {
        // THE ONE WALK over the census roots; the scan and the gather differ only in what they do per file.
        template <class Visit>
        void ForEachContentFile( Visit&& visit )
        {
            for ( std::size_t i = 0; i < CONTENT_KIND_COUNT; ++i )
            {
                const auto            kind = static_cast<ContentKind>( i );
                const ContentKindSpec spec = KindSpec( kind );
                // A redirector is found under the moved asset's kind and states its own in the header.
                if ( spec.StatedOnly() )
                    continue;

                // Through `ListFilesRecursive` and not a bare iterator, because that primitive is the one
                // that also sees what a mounted `.dpak` holds — a packaged game's content directories do
                // not exist on disk at all. The font and icon services each hand-rolled the disk half once,
                // and a packaged game scanned nothing.
                for ( const std::filesystem::path& candidate :
                      Utils::FileSystem::ListFilesRecursive( *spec.Root ) )
                {
                    // Kinds may share an extension under nested roots (Texture/Skybox): a file belongs to the
                    // kind whose root is the LONGEST that contains it, never to whichever row was walked first.
                    if ( LowerExtension( candidate ) != spec.Extension || KindOfContentFile( candidate ) != kind )
                        continue;

                    const std::string key = AssetHandle::StableKeyForPath( candidate );
                    if ( key.empty() )
                        continue;

                    visit( candidate, kind, key );
                }
            }
        }

        std::int64_t ModifiedTime( const std::filesystem::path& file )
        {
            std::error_code ec;
            const auto      stamp = std::filesystem::last_write_time( file, ec );
            return ec ? 0 : static_cast<std::int64_t>( stamp.time_since_epoch().count() );
        }

        // 2: mesh rows read since carry their header box; a version-1 cache is rebuilt once.
        // 3: a row's identity and dependency edges are the header's GUIDs folded by HandleForGuid, and the
        //    header is their only writer. A version-2 cache holds identities and edges a running editor
        //    learned from loaded assets (path-derived numbers among them); reusing one would keep those rows
        //    for as long as their files' size and stamp hold, so a version-2 cache is rebuilt once.
        constexpr std::string_view kCacheMagic = "DesertAssetRegistryCache 3";
    } // namespace

    std::map<std::string, ContentFile> ScanContentRoots()
    {
        std::map<std::string, ContentFile> found;
        ForEachContentFile( [&found]( const std::filesystem::path& file, ContentKind kind, const std::string& key )
                            { found.emplace( key, DescribeContentFile( file, kind ) ); } );
        return found;
    }

    std::filesystem::path RegistryCachePath()
    {
        return Constants::Path::CurrentProjectRoot().ProjectDir / "Intermediate" / "AssetRegistry.cache";
    }

    ResultStr<Utils::AssetRegistryEntry> RegistryRowFor( const std::string& key, const ContentFile& file )
    {
        Utils::AssetRegistryEntry entry;
        entry.Key  = key;
        entry.Kind = std::string( KindName( file.Kind ) );
        entry.Size = file.Size;
        // A header that is there and unreadable keeps the file out: a row without it would publish "this
        // file states no GUID", which is false.
        if ( !file.HeaderError.empty() )
            return MakeFormattedError<Utils::AssetRegistryEntry>( "{}: {}", key, file.HeaderError );
        if ( file.Header )
        {
            // A REDIRECTOR SITS WHERE THE MOVED ASSET SAT, under that asset's extension and root, so the walk
            // finds it as the old kind and its header says otherwise; that disagreement is its whole shape.
            // What it must agree with instead is the key it was written for: a redirector copied or moved
            // elsewhere would answer for a path it was never left at.
            if ( file.Header->Kind == ContentKind::Redirector )
            {
                entry.Kind = std::string( KindName( ContentKind::Redirector ) );
                if ( file.RedirectedFrom != key )
                    return MakeFormattedError<Utils::AssetRegistryEntry>(
                         "{}: this redirector was written for '{}' and answers only there", key,
                         file.RedirectedFrom );
            }
            else if ( file.Header->Kind != file.Kind )
                return MakeFormattedError<Utils::AssetRegistryEntry>(
                     "{}: its header states kind '{}' and it sits where a '{}' belongs", key,
                     KindName( file.Header->Kind ), entry.Kind );
            entry.Guid     = file.Header->Guid;
            entry.Versions = file.Header->Subsystems;
            // THE ROW'S NUMBERS ARE THE HEADER'S GUIDS, FOLDED ONCE. The runtime keys a GUID asset by
            // HandleForGuid( Guid ), and an edge names its target the same way, so a registry built from
            // headers alone resolves every reference a loaded asset would make (UE builds FAssetData's
            // dependencies from the package summary for the same reason: no load is needed to know them).
            entry.Identity = static_cast<uint64_t>( HandleForGuid( file.Header->Guid ) );
            for ( const AssetGuid& dependency : file.Header->Dependencies )
            {
                if ( !dependency.IsNull() )
                    entry.Dependencies.push_back( static_cast<uint64_t>( HandleForGuid( dependency ) ) );
            }
            std::sort( entry.Dependencies.begin(), entry.Dependencies.end() );
            entry.Dependencies.erase( std::unique( entry.Dependencies.begin(), entry.Dependencies.end() ),
                                      entry.Dependencies.end() );
            std::sort( entry.Versions.begin(), entry.Versions.end(),
                       []( const SubsystemVersion& a, const SubsystemVersion& b ) { return a.Tag < b.Tag; } );
        }
        if ( file.MeshBounds && file.MeshBounds->Stated )
            entry.Bounds = file.MeshBounds->Bounds;
        return MakeSuccess( std::move( entry ) );
    }

    GatheredRegistry GatherContentRegistry( const RegistryCache& cache )
    {
        GatheredRegistry gathered;
        ForEachContentFile(
             [&]( const std::filesystem::path& file, ContentKind kind, const std::string& key )
             {
                 const Utils::AssetRegistryEntry* cached   = cache.Registry.FindByKey( key );
                 const auto                       modified = cache.Modified.find( key );
                 if ( cached != nullptr && modified != cache.Modified.end() && cached->Kind == KindName( kind ) &&
                      cached->Size == Utils::FileSystem::GetFileSize( file ) &&
                      modified->second == ModifiedTime( file ) )
                 {
                     if ( const auto inserted = gathered.Registry.Insert( *cached ); !inserted )
                         gathered.Refused.push_back( inserted.GetError() );
                     else
                         ++gathered.FromCache;
                     return;
                 }

                 const ContentFile described = DescribeContentFile( file, kind );
                 auto              row       = RegistryRowFor( key, described );
                 if ( !row )
                 {
                     gathered.Refused.push_back( row.GetError() );
                     return;
                 }
                 if ( const auto inserted = gathered.Registry.Insert( row.GetValue() ); !inserted )
                 {
                     gathered.Refused.push_back( inserted.GetError() );
                     return;
                 }
                 ++gathered.Read;
                 if ( ( kind == ContentKind::StaticMesh || kind == ContentKind::SkinnedMesh ) &&
                      !( described.MeshBounds && described.MeshBounds->Stated ) )
                     gathered.MeshesWithoutHeaderBounds.push_back( key );
             } );
        return gathered;
    }

    std::string SerializeRegistryCache( const Utils::AssetRegistry& registry )
    {
        std::string text( kCacheMagic );
        text += '\n';
        for ( const Utils::AssetRegistryEntry& row : registry.Entries() )
        {
            text += std::to_string( ModifiedTime( AssetHandle::PathForStableKey( row.Key ) ) );
            text += ' ';
            text += row.Key;
            text += '\n';
        }
        text += registry.Serialize();
        return text;
    }

    ResultStr<RegistryCache> ParseRegistryCache( std::string_view text )
    {
        // The WHOLE first line, not a prefix: "...Cache 3" is a prefix of "...Cache 30".
        const std::string_view magicLine = text.substr( 0, text.find( '\n' ) );
        if ( magicLine != kCacheMagic )
            return MakeFormattedError<RegistryCache>( "a registry cache of another form: its first line is '{}', "
                                                      "this build writes '{}'",
                                                      magicLine, kCacheMagic );
        const std::size_t registryAt = text.find( "\nDesertAssetRegistry " );
        if ( registryAt == std::string_view::npos )
            return MakeError<RegistryCache>( "the registry cache carries no registry" );
        // The embedded registry must be the form this build WRITES. `AssetRegistry::Parse` also reads older
        // forms (a committed file is migrated by reading it), but an older form inside a cache lacks columns
        // the gather fills, and the cache would hand those rows back unread for as long as their files'
        // size and stamp hold.
        const std::string      currentLine = Utils::AssetRegistry().Serialize();
        const std::string_view writtenLine = std::string_view( currentLine ).substr( 0, currentLine.find( '\n' ) );
        const std::string_view embedded    = text.substr( registryAt + 1 );
        const std::string_view embeddedLine = embedded.substr( 0, embedded.find( '\n' ) );
        if ( embeddedLine != writtenLine )
            return MakeFormattedError<RegistryCache>( "the registry cache embeds '{}', this build writes '{}'",
                                                      embeddedLine, writtenLine );

        RegistryCache cache;
        std::size_t   at = text.find( '\n' ) + 1;
        while ( at <= registryAt )
        {
            const std::size_t      end   = text.find( '\n', at );
            const std::string_view line  = text.substr( at, end - at );
            const std::size_t      space = line.find( ' ' );
            if ( space == std::string_view::npos )
                return MakeFormattedError<RegistryCache>( "the registry cache line '{}' has no key", line );
            cache.Modified.emplace( std::string( line.substr( space + 1 ) ),
                                    std::stoll( std::string( line.substr( 0, space ) ) ) );
            at = end + 1;
        }

        auto registry = Utils::AssetRegistry::Parse( text.substr( registryAt + 1 ) );
        if ( !registry )
            return MakeError<RegistryCache>( registry.GetError() );
        cache.Registry = registry.GetValue();
        return MakeSuccess( std::move( cache ) );
    }

    std::optional<std::map<std::string, ContentFile>> TrackedContent( const std::filesystem::path& repoRoot )
    {
        // `git ls-files` AND NOT A WALK, because the question is "what does a clean clone contain" and
        // only git can answer it. `-z` because a content path may legally contain a newline, which is
        // the same refusal `AssetRegistry::Insert` makes about keys; `--full-name` so the paths are
        // repository-relative whatever directory git was invoked from.
        //
        // popen rather than libgit2: the gate runs in CI, CI is a git checkout, and a dependency added
        // for one string list would have to be built on both platforms for ever.
        //
        // THE TOPLEVEL IS ASKED FOR RATHER THAN ASSUMED, and this cost a measured mistake inside the
        // very fix this function is: `--full-name` prints paths relative to the REPOSITORY ROOT, and
        // the first version joined them to the directory the caller passed. Passing `Editor/` produced
        // `<repo>/Editor/Editor/Resources/...`, which is under no content root, so every file was
        // filtered out and the answer was AN EMPTY LIST — reported as success. `AssetRegistryTool cook`
        // then rewrote a 247-row registry with nothing in it. That is the same defect this whole round
        // is about, committed once more inside its own repair: an instrument answered a different
        // question and had nothing in its output to say so. Hence: resolve the root, and let the caller
        // pass any directory inside the checkout.
        const std::string toplevelCommand = "git -C " +
                                            QuoteForShell( WithoutTrailingSeparator( repoRoot.string() ) ) +
                                            " rev-parse --show-toplevel 2>" + std::string( kNullDevice );
        std::string toplevel = RunAndCapture( toplevelCommand );
        while ( !toplevel.empty() && ( toplevel.back() == '\n' || toplevel.back() == '\r' ) )
            toplevel.pop_back();
        if ( toplevel.empty() )
            return std::nullopt;

        const std::filesystem::path root = std::filesystem::path( toplevel ).lexically_normal();

        const std::string command = "git -C " + QuoteForShell( WithoutTrailingSeparator( root.string() ) ) +
                                    " ls-files -z --full-name 2>" + std::string( kNullDevice );

        const std::string output = RunAndCapture( command );
        if ( output.empty() )
            return std::nullopt;

        std::map<std::string, ContentFile> tracked;

        std::size_t start = 0;
        while ( start < output.size() )
        {
            const std::size_t end = output.find( '\0', start );
            const std::string relative =
                 output.substr( start, end == std::string::npos ? std::string::npos : end - start );
            start = ( end == std::string::npos ) ? output.size() : end + 1;
            if ( relative.empty() )
                continue;

            const std::filesystem::path file = ( root / relative ).lexically_normal();

            // CLASSIFIED BY THE SAME CENSUS the disk scan uses, so a file is content here exactly when
            // it is content there — two answers to "is this content" is the shape this whole task closed.
            const std::optional<ContentKind> kind = KindOfContentFile( file );
            if ( !kind )
                continue;

            const std::string key = AssetHandle::StableKeyForPath( file );
            if ( key.empty() )
                continue;

            // The SIZE comes from the disk even here, and it has to: git knows the blob, not the bytes
            // as they land. In a clean checkout the two agree, and where they do not — a file edited but
            // not committed — the gate SHOULD report it: the registry is gathered from the disk, so it
            // describes bytes another checkout of the same commit does not have.
            tracked.emplace( key, DescribeContentFile( file, *kind ) );
        }

        return tracked;
    }

    std::vector<RegistryDisagreement> Compare( const Utils::AssetRegistry&               registry,
                                               const std::map<std::string, ContentFile>& present,
                                               std::string_view                          sourceName )
    {
        const std::string where( sourceName );

        std::vector<RegistryDisagreement> problems;

        for ( const auto& [key, file] : present )
        {
            const std::string kindName( KindName( file.Kind ) );

            const Utils::AssetRegistryEntry* row = registry.FindByKey( key );
            if ( row == nullptr )
            {
                problems.push_back(
                     { RegistryDisagreement::Kind::MissingRow, key,
                       "'" + key + "' (" + kindName + ") is " + where +
                            " and has no row in the cooked asset registry. Since the boot stopped "
                            "walking the content roots this file DOES NOT REACH THE ENGINE: no preload, "
                            "no picker, and not one byte of it in a packaged build — while the editor "
                            "session that added it looked entirely normal." +
                            kRemedy } );
                continue;
            }

            if ( row->Kind != kindName )
            {
                problems.push_back( { RegistryDisagreement::Kind::WrongKind, key,
                                      "'" + key + "' is recorded as a '" + row->Kind + "' and is a '" + kindName +
                                           "' on disk, so the loader would build it with the "
                                           "wrong asset class." +
                                           kRemedy } );
            }

            if ( !file.HeaderError.empty() )
            {
                problems.push_back( { RegistryDisagreement::Kind::BadHeader, key,
                                      "'" + key + "' has a header no reader accepts: " + file.HeaderError } );
            }
            else if ( file.Header && file.Header->Kind != file.Kind )
            {
                problems.push_back( { RegistryDisagreement::Kind::BadHeader, key,
                                      "'" + key + "' states kind '" +
                                           std::string( KindName( file.Header->Kind ) ) +
                                           "' in its header and sits where a '" + kindName +
                                           "' belongs, so the header and the census name two classes." } );
            }
            else
            {
                const std::optional<AssetGuid> stated =
                     file.Header ? std::optional<AssetGuid>( file.Header->Guid ) : std::nullopt;
                std::vector<SubsystemVersion> versions =
                     file.Header ? file.Header->Subsystems : std::vector<SubsystemVersion>{};
                std::sort( versions.begin(), versions.end(),
                           []( const SubsystemVersion& a, const SubsystemVersion& b ) { return a.Tag < b.Tag; } );
                if ( row->Guid != stated || row->Versions != versions )
                {
                    problems.push_back(
                         { RegistryDisagreement::Kind::StaleHeader, key,
                           "'" + key + "' is recorded with GUID " +
                                ( row->Guid ? AssetGuidToText( *row->Guid ) : std::string( "(none)" ) ) +
                                " and its header states " +
                                ( stated ? AssetGuidToText( *stated ) : std::string( "(none)" ) ) +
                                " (or other versions), so a reference by GUID would resolve through a row "
                                "that is not this file's." +
                                kRemedy } );
                }
            }

            if ( row->Size != file.Size )
            {
                problems.push_back(
                     { RegistryDisagreement::Kind::StaleSize, key,
                       "'" + key + "' is recorded at " + std::to_string( row->Size ) + " bytes and is " +
                            std::to_string( file.Size ) + " " + where +
                            ", so the registry predates an edit. For a `.tex` or a `.demat` that means "
                            "the identity column may name a number the file no longer carries, which is "
                            "a reference resolving to nothing." +
                            kRemedy } );
            }
        }

        for ( const Utils::AssetRegistryEntry& row : registry.Entries() )
        {
            if ( present.find( row.Key ) != present.end() )
                continue;

            problems.push_back(
                 { RegistryDisagreement::Kind::OrphanRow, row.Key,
                   "the registry has a row for '" + row.Key + "' and no such file is " + where +
                        ", so the loader will try to read it once per boot for ever and log a failure "
                        "naming a file the reader cannot find." +
                        kRemedy } );
        }

        return problems;
    }
} // namespace Common::Content
